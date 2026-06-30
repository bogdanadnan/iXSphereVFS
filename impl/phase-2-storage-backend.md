# Phase 2: StorageBackend

## Goal
Page-level allocation with a free bitmap, lazy mirror crash-safety, file I/O
with a unified page cache, and ordered flush with priority-based write-back.
All page indices seen by upper layers are logical — the StorageBackend owns
the physical backing file and the free-page tracking.

---

## Workload 2.1 — File Layout & XVFS Magic

**What:** Define the structure of the backing file: a two-page StorageBackend
header followed by bitmap pages and allocatable data pages. Encode a
4-byte "XVFS" magic number for file identification on mount.

**Why:** The backing file must be self-describing. On mount, the system needs
to verify it is opening a valid iXSphereVFS file — not arbitrary data — and
read the configuration needed to initialize the allocator.

**How:**
- Logical page 0 is the primary StorageBackend header. Its PageHeader carries
  `flags = 0x56585346`, which together form the ASCII
  string `"XVFS"` in little-endian byte order. This is the magic number.
- Logical page 1 is the continuation of the `bitmap_dir` array. It has a
  standard 16-byte PageHeader (type `0x02` PoolPage, `flags = 0`) but its
  payload is a pure array of `int64_t` entries — no configuration fields.
- The header page 0 payload starts with configuration fields at offset 0:
  `total_pages` (8 bytes), `page_size` (8 bytes, default 8192, caller-configurable at creation time), a `reserved`
  block (16 bytes). Starting at offset 32 is `bitmap_dir[]` — an array of
  `int64_t` logical page indices, zero-terminated (unused entries are zero
  because the page is zero-filled at allocation).
- The `bitmap_dir` span: page 0 holds entries `0 .. (page_size - 32) / 8 - 1`,
  page 1 holds the remainder. Total capacity at page_size=8192 is 2,044 entries,
  supporting up to ~134 million logical pages (~1.1 TB logical).
- On mount: read logical page 0, validate the magic (`pageType == 0x5658 &&
  flags == 0x5346`), validate CRC32C on the payload. If any check fails,
  reject — not a valid VFS file.
- On create: write header page 0 with `total_pages = 4` (pages 0–3 reserved),
  `page_size = 8192`, and `bitmap_dir[0] = 2` (the first bitmap page is at
  logical page 2). Write header page 1 with zeros (no entries yet).

**Acceptance:**
  - A freshly created backing file passes the magic + CRC validation on open.
  - Opening a file that lacks the magic fails with `VFS_ERR_IO`.
  - Opening a file with valid magic but corrupt CRC fails.
  - `total_pages` and `page_size` read back correctly after creation.

---

## Workload 2.2 — Free-Page Bitmap

**What:** A free-page allocator using a multi-page bitmap. Pages are tracked
by a single bit each (`1` = free, `0` = allocated). Allocation is zone-based
for thread safety.

**Why:** Every page in the system — data, pool, superblock, bitmap — is
obtained from a single allocator. The bitmap must support concurrent
allocations without a global lock, and must grow as the backing file grows.

**How:**
- The bitmap is stored in ordinary logical pages (pageType `0x01` Bitmap).
  Each page holds `page_size × 8` bits, covering that many logical pages.
- The `bitmap_dir[]` array in the header lists the logical page indices of
  all bitmap pages. It is zero-terminated: the first zero entry marks the
  end of the list. At creation, only `bitmap_dir[0] = 2` (the first bitmap
  page).
- To find the bit for logical page N: `bitmap_index = N / (page_size * 8)`,
  `bit_offset = N % (page_size * 8)`. If `bitmap_index` exceeds the number
  of allocated bitmap pages, extend the list: allocate a new page, zero-fill
  it (all bits `0` = allocated), then set all its bits to `1` (free) for
  the newly covered range. Append its page index to the first zero slot in
  `bitmap_dir` via CAS.
- Bit operations: set (mark allocated), clear (mark free), test (check status).
  Use atomic bit operations (`__sync_fetch_and_or` / `lock bts` / etc.) for
  thread safety.
- Zone-based scanning: divide the logical page space into zones of 1M pages.
  Each zone has a hint cursor — the starting point for next-fit search.
  `Allocate(count)` picks a zone based on the calling thread's ID, scans from
  the hint cursor, advances the cursor past the allocation. If the zone has
  no contiguous block, scan adjacent zones round-robin.
- `Allocate(count)` returns the first logical page index on success, or -1
  if no `count` contiguous pages exist.
- `Acquire(page)` atomically checks and sets a specific bit. Returns true
  if the page was free, false if already allocated. Used for fixed-location
  reservations (superblock at page 3).
- `Free(page)` clears the bit. If the page has a mirror sibling, frees the
  sibling too. Called primarily by GC.
- Maximum capacity: 2,044 bitmap pages at page_size=8192, covering ~134M pages
  (~1.1 TB logical). `Allocate` returns -1 when the `bitmap_dir` is full.

**Acceptance:**
  - Allocate 1 page: returns page index, bit is now 0.
  - Allocate 10 contiguous pages: bits are 0, returned index is the first.
  - Acquire a specific page that is free: returns true, bit is 0.
  - Acquire a page that is already allocated: returns false.
  - Free a page: bit returns to 1.
  - Allocate beyond the first bitmap page: second bitmap page is created
    and appended to `bitmap_dir`.
  - Four concurrent threads allocate 10,000 pages each: total allocated
    matches 40,000, no double-allocations. This verifies zone-based locking.

---

## Workload 2.3 — Lazy Mirror Pages

**What:** Crash-safe page writes using a lazy mirror scheme. Every logical
page is a single copy on first write; a mirror sibling is allocated only
on the second write. The PageHeader's `mirrorPage` and `generation` fields
track the mirror state.

**Why:** Eagerly allocating two physical pages for every logical page doubles
storage. Most pages are write-once (data pages filled during INSERT, metadata
pages created during tree growth). Deferring the mirror allocation saves
significant space.

**How:**
- Every logical page is backed by `page_size + 16` bytes of physical storage
  (16-byte PageHeader + `page_size`-byte payload).
- PageHeader fields: `pageType` (2), `flags` (2), `checksum` (4), `generation`
  (4), `mirrorPage` (4, signed, -1 means no mirror).
- First write: build payload, compute CRC32C, write PageHeader with `generation = 1`,
  `mirrorPage = -1`. This is the only copy.
- Second write: allocate a new logical page (the mirror sibling). Build the
  new payload, set its `generation = 2`, `mirrorPage = original`. Write the
  sibling page. Then atomically write `mirrorPage = sibling` on the original
  page header. The sibling is now reachable and has the higher generation.
- Subsequent writes: read both headers, find the inactive one (lower generation),
  write to it with `generation = active.generation + 1`.
- Read: check `mirrorPage`. If -1, this is a single-copy page — read the
  payload and validate CRC32C. If not -1, read both headers, compare
  generations, read the payload from the higher-generation page, validate
  CRC32C. If CRC fails, try the other page.
- Crash at mirror allocation: if the crash occurs before the `mirrorPage`
  write on the original, the original is intact (gen=1, mirror=-1). The
  sibling is an unreachable zombie — harmless, reclaimed by GC.
- Single-copy risk: before a mirror exists, a crash during any write may
  corrupt the sole copy. This risk window is exactly one write per page:
  the write that creates the mirror. Once the mirror exists, subsequent
  writes are protected by the alternate copy. This is the fundamental
  tradeoff for the storage savings.

**Acceptance:**
  - Write page, read back: payload matches.
  - Write same page a second time: read returns new payload.
  - Write second time triggers mirror allocation: verify both pages exist
    and the original's `mirrorPage` points to the sibling.
  - Kill process during mirror creation (before `mirrorPage` write on original):
    on remount, the original is intact with gen=1, mirror=-1. The sibling
    is an unreachable zombie.
  - Kill process after mirror creation: on remount, both pages are linked
    and the higher-generation page carries the latest data.

---

## Workload 2.4 — File I/O

**What:** `Read` and `Write` operations on logical pages, with transparent
PageHeader management, CRC32C validation, and a `Flush` function for
durability. A per-page write-back mechanism reduces the data loss window
without blocking writers.

**Why:** The VFS layer must never see PageHeaders or deal with physical
addressing. The StorageBackend provides a clean `Read(page) → payload`,
`Write(page, payload)` interface that hides all crash-safety and layout
concerns.

**How:**
- `Read(logicalPage)`: check the unified page cache (Workload 2.5). On hit,
  return the cached payload buffer. On miss, read from the backing file,
  apply lazy mirror logic to pick the correct physical page, validate
  CRC32C on the payload, insert into the cache as clean, and return the
  payload buffer. Returns NULL if the page has never been written.
- `Write(logicalPage, data)`: accept a `page_size`-byte payload buffer.
  Apply lazy mirror logic: on first write, write to the page directly
  (gen=1, mirror=-1). On second write, allocate a mirror sibling and link
  it. On subsequent writes, alternate between the two pages with incremented
  generation. The page is marked dirty in the cache — NOT written to disk.
- `Flush(logicalPage)`: if `logicalPage < 0`, write ALL dirty pages to the
  backing file in priority order (see Workload 2.6) and call `fsync`. If
  `logicalPage >= 0`, write only that single page without fsync. Marks
  flushed pages as clean but keeps them in the cache.
- All `Read` and `Write` calls operate on the payload only. The 16-byte
  PageHeader is added/stripped transparently by the StorageBackend. CRC32C
  is computed on `Write` and validated on `Read`.
- The flush priority (0=data, 1=pool, 2=bitmap, 3=superblock) is read from
  the PageHeader `flags` field, which was set at allocation time by the VFS
  layer. The StorageBackend maintains separate dirty lists per priority.

**Acceptance:**
  - Write payload, Read back: payload matches, CRC32C is valid.
  - Read a page that was never written: returns NULL.
  - Write, Flush(-1), kill process, remount, Read: payload survives.
  - Write two pages at different priorities, Flush(-1): pages are written
    to disk in the correct order (lowest priority first).

---

## Workload 2.5 — Unified Page Cache

**What:** An in-memory cache of page payloads with LRU eviction for clean
pages. Dirty pages are never evicted. Thread-safe for concurrent access.

**Why:** Repeated reads of the same page (common for metadata pages like
pool pages and directory nodes) should not re-read from disk. The cache
also absorbs writes, deferring disk I/O until `Flush`.

**How:**
- Cache entries keyed by logical page index. Each entry holds the 8192-byte
  payload and a state: clean (matches disk) or dirty (modified by `Write`).
- Hash table with per-bucket mutexes for thread safety. Bucket count is a
  configurable power of two (default 16,384 buckets).
- `Read` checks the cache first by hashing the logical page index. On hit,
  returns the cached payload immediately. In the common case (metadata pages),
  the payload was CRC-validated when first read from disk.
- `Write` inserts or updates the cache entry, marks it dirty.
- `Flush` writes dirty pages to disk and marks them clean. Clean pages remain
  in the cache — they are eligible for LRU eviction.
- Eviction: when the total number of cached pages exceeds the configured
  budget (default 256 MB = 32,768 pages), the LRU list is walked and clean
  pages are evicted until the budget is met. Dirty pages are skipped during
  eviction — they must be flushed first.
- LRU tracking: each cache entry has a timestamp or position in a doubly-linked
  list. On every access, the entry moves to the front. Eviction scans from
  the back.
- The cache is thread-safe: each hash bucket has its own mutex. Read/Write
  operations on different buckets proceed concurrently.

**Acceptance:**
  - Read same page twice: first hits disk, second hits cache (faster, no disk I/O).
  - Write page, Read back: returns dirty value without disk I/O.
  - Fill cache beyond budget: clean pages are evicted; dirty pages remain.
  - Flush then fill cache: previously-dirty pages are now clean and evictable.

---

## Workload 2.6 — Bootstrap & Mount

**What:** The sequence for creating a new VFS backing file and for opening
an existing one. Includes the superblock reservation at logical page 3.

**Why:** The VFS must be able to start from an empty file and initialize
all required system pages, and to reopen an existing file and restore
the allocator state.

**How:**
- **Create (file does not exist or is empty):**
  1. Create the backing file.
  2. Reserve logical pages 0–3 directly (before the bitmap exists, so
     `Allocate` cannot be used). Page 0 is header page 0, page 1 is header
     page 1, page 2 is the first bitmap page, page 3 is reserved for the
     superblock.
  3. Write header page 0 with `total_pages = 4`, `page_size = 8192`,
     `bitmap_dir[0] = 2`. Write header page 1 as zeros.
  4. Write bitmap page 2: all bits set to 1 (free), except bits 0–3 set to
     0 (allocated — the four reserved pages).
  5. Return success. The VFS layer then calls `Acquire(3)` to claim the
     superblock page and initializes it.
- **Open (file exists):**
  1. Read logical page 0. Validate magic and CRC32C.
  2. Read `total_pages` and `page_size` from the header.
  3. Load `bitmap_dir` from pages 0 and 1. Walk all referenced bitmap pages
     and reconstruct the in-memory free/allocated state.
  4. Return success. The VFS layer reads the superblock from page 3.

**Acceptance:**
  - Create new file, close, reopen: `total_pages` is 4, `page_size` is 8192.
  - After creation, `Allocate(1)` returns page 4 (pages 0–3 are reserved).
  - Opening a non-VFS file fails during magic validation.
  - Opening a truncated file fails during CRC32C validation.
