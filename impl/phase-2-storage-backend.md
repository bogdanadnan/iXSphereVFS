# Phase 2: StorageBackend

## Goal
Page-level allocation with a dynamic indirection table, lazy mirror crash-safety,
file I/O with a unified page cache, and ordered flush with priority-based
write-back. All page indices seen by upper layers are logical — the StorageBackend
owns the physical backing file and the logical-to-physical mapping.

---

## Workload 2.1 — File Layout & XVFS Magic

**What:** Define the structure of the backing file: a header page that doubles
as the first indirection directory page, followed by dynamically chained
indirection overflow pages and physical page data. Encode a 4-byte "XVFS"
magic number for file identification on mount.

**Why:** The backing file must be self-describing. On mount, the system needs
to verify it is opening a valid iXSphereVFS file — not arbitrary data — and
read the configuration needed to initialize the allocator.

**How:**
- Logical page 0 is the StorageBackend header. Its PageHeader `flags` field
  carries `0x56585346` — the ASCII string "XVFS" in little-endian byte order.
  This is the magic number.
- The header page payload serves double duty: the first 40 bytes are
  configuration fields; the remainder (~1,020 entries at page_size=8192) is
  inline indirection entries. Small files never need a separate indirection
  page.
- Config fields (offset 0): `total_pages` (8 bytes, highest allocated logical
  page + 1), `page_size` (8 bytes, default 8192, caller-configurable at
  creation time), `segment_size` (4 bytes, uint32_t, default 1024 — pages
  per FileContent segment), `reserved` (4 bytes), `physical_tail` (8 bytes —
  next available physical byte offset).
- Inline indirection entries start at offset 40. Each entry is an `int64_t`:
  0 = free / never-written. Non-zero = physical byte offset within the
  backing file. A physical page at offset O occupies `page_size + 16` bytes
  (16-byte PageHeader + payload).
- On mount: read logical page 0, validate `flags == 0x56585346` and CRC32C
  on the payload. If either check fails, reject — not a valid VFS file.
- On create: write header page 0 with `total_pages = 2`, `page_size = 8192`,
  `segment_size = 1024`, `physical_tail = 2 * (page_size + 16)`. Set inline
  entries 0 and 1 to their computed physical offsets (0, page_size + 16).
  All other inline entries remain 0 (free). `indirection_head = 0` (no
  overflow pages yet).

**Acceptance:**
  - A freshly created backing file passes the magic + CRC validation on open.
  - Opening a file that lacks the magic fails with `VFS_ERR_IO`.
  - `total_pages`, `page_size`, `segment_size`, `physical_tail` read back
    correctly after creation.

---

## Workload 2.2 — Indirection Table

**What:** An indirection table mapping logical page indices to physical byte
offsets. The header page provides inline entries for the first ~1,020 logical
pages. Overflow pages are allocated dynamically and chained via a `next`
pointer. Allocation is sequential via an atomic `physical_tail` counter —
no bitmap, no zone scanning.

**Why:** Physical contiguity produces sequential disk I/O even under concurrent
writers. An atomic tail is O(1) per allocation with a single CAS. The inline
header entries eliminate a separate page allocation for small files.

**How:**
- The header page holds inline entries starting at offset 40, covering the
  first `(page_size - 40) / 8` logical pages.
- Additional logical pages are covered by overflow pages, chained from
  `indirection_head` (offset 32 of the header, 0 if no overflow). Each
  overflow page layout: `next` (8 bytes at offset 0, logical page index
  of next overflow page, 0 = end of chain), then entries (int64_t array
  filling the rest of the page — `(page_size / 8) - 1` entries per page).
- To find the indirection entry for logical page N: if N < inline_capacity,
  read `header_payload[40 + N * 8]`. Otherwise, walk the overflow chain,
  tracking entries_so_far, until reaching the page containing N. The entry
  is at offset `8 + (N - entries_so_far) * 8` within that page.
- `Allocate(count)`: find `count` consecutive logical pages whose indirection
  entries are 0. For each, atomically advance `physical_tail` by
  `page_size + 16` via CAS, and write the old tail value into the entry.
  Returns the first logical page index, or -1 if out of space in the
  StorageBackend. Multi-page allocations are physically contiguous.
- `Acquire(page)`: CAS-set a specific indirection entry from 0 to an
  allocated physical offset via `physical_tail`. Returns true on success.
  Used for fixed-location reservations (superblock at page 1).
- `Free(page)`: set the indirection entry to 0. Physical space is not
  reclaimed — it becomes unreachable and is compacted by GC. If the
  page has a mirror sibling, free the sibling too.
- Thread safety: a single atomic CAS on `physical_tail` replaces zone-based
  scanning. Contention only at the tail counter.
- When the indirection table capacity is exhausted (no free logical pages
  within the current inline + overflow range), allocate a new overflow page
  via `Allocate(1)`, zero-fill it, and CAS-append it to the end of the
  overflow chain.

**Acceptance:**
  - Allocate 1 page: entry becomes non-zero, page is zero-filled.
  - Allocate 10 pages: entries are sequential physical offsets, returned
    index is the first.
  - Acquire a specific page that is free: returns true, entry is non-zero.
  - Acquire an already-allocated page: returns false.
  - Free a page: entry returns to 0, physical space not reclaimed.
  - Allocate beyond the inline entry count: an overflow page is created and
    chained via `next`.
  - Four concurrent threads allocate 10,000 pages each: physical offsets are
    sequential, no double-allocations, total matches 40,000 allocated.

---

## Workload 2.3 — Lazy Mirror Pages

**What:** Crash-safe page writes using a lazy mirror scheme. Every logical
page is a single copy on first write; a mirror sibling is allocated only
on the second write. The PageHeader's `mirrorPage` and `generation` fields
track the mirror state.

**Why:** Eagerly allocating two physical pages for every logical page doubles
storage. Most pages are write-once. Deferring the mirror allocation saves
significant space.

**How:**
- Every logical page is backed by `page_size + 16` bytes of physical storage
  (16-byte PageHeader + `page_size`-byte payload). The physical offset comes
  from the indirection table entry.
- PageHeader fields: `flags` (4 bytes, priority in bits 0–1), `checksum` (4),
  `generation` (4), `mirrorPage` (4, signed, -1 = no mirror).
- First write: build payload, compute CRC32C, write PageHeader with
  `generation = 1`, `mirrorPage = -1`. This is the only copy.
- Second write: allocate a new logical page (the mirror sibling). Build the
  new payload, set its `generation = 2`, `mirrorPage = original`. Write the
  sibling page. Then atomically write `mirrorPage = sibling` on the original
  page header. The sibling is now reachable and has the higher generation.
- Subsequent writes: read both headers, find the inactive one (lower
  generation), write to it with `generation = active.generation + 1`.
- Read: check `mirrorPage`. If -1, this is a single-copy page — read the
  payload and validate CRC32C. If not -1, read both headers, compare
  generations, read the payload from the higher-generation page, validate
  CRC32C. If CRC fails, try the other page.
- Crash at mirror allocation: the original is intact (gen=1, mirror=-1).
  The sibling is an unreachable zombie — harmless, reclaimed by GC.
- Single-copy risk: before a mirror exists, a crash during any write may
  corrupt the sole copy. This risk window is exactly one write per page.
  Once the mirror exists, subsequent writes are protected.

**Acceptance:**
  - Write page, read back: payload matches.
  - Second write to same page triggers mirror allocation: verify both pages
    exist and links are correct.
  - Kill process during mirror creation (before `mirrorPage` write): on
    remount, original is intact, sibling is a zombie.

---

## Workload 2.4 — File I/O

**What:** `Read` and `Write` operations on logical pages, with transparent
PageHeader management, CRC32C validation, and a `Flush` function for
durability. A per-page write-back mechanism reduces the data loss window
without blocking writers.

**Why:** The VFS layer must never see PageHeaders or deal with physical
addressing. The StorageBackend provides a clean `Read(page) → payload`,
`Write(page, payload)` interface.

**How:**
- `Read(logicalPage)`: look up the physical offset from the indirection
  table. Check the unified page cache (Workload 2.5). On hit, return the
  cached payload. On miss, apply lazy mirror logic to pick the correct
  physical page, validate CRC32C on the payload, insert into the cache as
  clean, and return the payload buffer. Returns NULL if the indirection
  entry is 0 (never written).
- `Write(logicalPage, data)`: accept a `page_size`-byte payload buffer.
  Apply lazy mirror logic. The page is marked dirty in the cache — NOT
  written to disk.
- `Flush(logicalPage)`: if `logicalPage < 0`, write ALL dirty pages to the
  backing file in priority order and call `fsync`. If `logicalPage >= 0`,
  write only that single page without fsync.
- The flush priority (0=data, 1=pool, 2=indirection, 3=superblock) is read
  from the PageHeader `flags` bits 0–1 of each dirty page. The VFS layer sets
  these bits at allocation time. The StorageBackend maintains separate dirty
  lists per priority.
- Write-back: priority 0 (data pages) only — pool, indirection, and superblock
  pages are never write-backed and must wait for explicit Flush.

**Acceptance:**
  - Write payload, Read back: payload matches, CRC32C is valid.
  - Read a never-written page: returns NULL.
  - Write, Flush(-1), kill process, remount, Read: payload survives.

---

## Workload 2.5 — Unified Page Cache

**What:** An in-memory cache of page payloads with LRU eviction for clean
pages. Dirty pages are never evicted. Thread-safe.

**Why:** Repeated reads of hot metadata pages should not re-read from disk.

**How:**
- Cache entries keyed by logical page index. Each entry holds the
  `page_size`-byte payload and a state: clean or dirty.
- Hash table with per-bucket mutexes for thread safety. Default bucket count
  is 16,384.
- `Read` checks the cache first. On hit, returns the cached payload
  immediately. On miss, reads from the backing file via the indirection table,
  validates CRC32C, inserts into cache as clean.
- `Write` inserts or updates the cache entry, marks it dirty.
- `Flush` writes dirty pages to disk in priority order, marks them clean.
- Eviction: when the total cached page count exceeds the configured budget
  (default 256 MB = 32,768 pages), evict clean pages via LRU. Dirty pages
  are skipped.
- LRU tracking: each entry has a position in a doubly-linked list. On access,
  the entry moves to the front. Eviction scans from the back.

**Acceptance:**
  - Read same page twice: first hits disk, second hits cache.
  - Fill cache beyond budget: clean pages evicted, dirty pages remain.

---

## Workload 2.6 — Bootstrap & Mount

**What:** The sequence for creating a new VFS backing file and for opening
an existing one. Includes the superblock reservation at logical page 1.

**Why:** The VFS must start from an empty file and also reopen existing files.

**How:**
- **Create (file does not exist or is empty):**
  1. Create the backing file.
  2. Reserve logical pages 0 and 1 directly (before the indirection table
     exists, so `Allocate` cannot be used). Page 0 is the header + inline
     indirection page. Page 1 is the superblock.
  3. Write header page 0 with `total_pages = 2`, `page_size = 8192`,
     `physical_tail = 2 * (page_size + 16)`. Set inline entries 0 and 1
     to their computed physical offsets (0, page_size + 16).
     `indirection_head = 0`.
  4. Return success. The VFS layer calls `Acquire(1)` for the superblock
     page and initializes it.
- **Open (file exists):**
  1. Read logical page 0. Validate `flags == 0x56585346` and CRC32C.
  2. Read `total_pages`, `page_size`, `segment_size`, `indirection_head`.
     Load all indirection entries (inline + overflow chain).
     Reconstruct the in-memory logical-to-physical mapping.
  3. Return success. The VFS layer reads the superblock from page 1.

**Acceptance:**
  - Create new file, close, reopen: `total_pages` is 2.
  - After creation, `Allocate(1)` returns page 2 (pages 0–1 are reserved).
  - Opening a non-VFS file fails during magic validation.
