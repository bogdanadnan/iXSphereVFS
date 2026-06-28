# iXSphereVFS — Epoch-Versioned Unified Tree

**Status:** Draft. Self-contained specification for a crash-safe, snapshot-native
storage layer. Serves as the native storage engine for all iXSphere platform
services, including SQLite VFS integration, file storage, and content-addressable
data.

---

## 1. Overview

iXSphereVFS is a copy-on-write storage layer that provides atomic snapshots,
crash-safe writes, and per-page versioning without a write-ahead log. It is
the native storage engine for all iXSphere platform services — SQLite
databases, file storage, content-addressable data, and internal service state.

**Key properties:**

- **Single tree.** One structure serves as namespace index, inode store, and
  extent map. No separate trees or metadata pages.
- **Epoch-keyed.** Every mutation is tagged with a monotonically increasing
  epoch. Even epochs (0, 2, 4, …) are live head states. Odd epochs (1, 3, 5, …)
  are isolated snapshots.
- **O(1) snapshots.** Taking a snapshot increments an integer. No page writes,
  no tree walks, no metadata copies.
- **Crash-safe by design.** Ping-pong data pages and atomic CAS on metadata
  eliminate the need for a recovery journal. Mount is O(1).
- **Conditional COW.** First write to a page in an epoch copies the old version.
  Subsequent writes to the same page in the same epoch overwrite in-place.
- **Per-page version chains.** Each logical page has a linked history of
  `(epoch, physicalPage)` entries. Reads walk the chain to find the correct
  version for the requested epoch.
- **Manual shadow-compaction GC.** Deleted epochs are physically removed via a
  copy-then-swap garbage collector that never mutates the live tree in-place.

---

## 2. Page Format

Every page consists of a 12-byte PageHeader followed by a page_size-byte
payload (page_size + 12 bytes total per logical page, doubled to 2 × (page_size + 12) bytes with
ping-pong). The header is transparent to the VFS layer — `ReadPage` and
`WritePage` operate on the payload only.

### 2.1 PageHeader (12 bytes)

```
Offset  Size  Field
──────  ────  ─────
 0       2    pageType       (uint16 — type code from PageType enum)
 2       2    flags          (uint16 — per-type flags)
 4       4    checksum       (uint32 — CRC32C of bytes [12..end))
 8       4    generation     (uint32 — incremented by allocator on every allocation of this page slot)
```

`generation` catches stale reads: if a page is freed then reallocated, a
reader holding an old page index will see a different generation. Combined
with CRC32C validation, this prevents use-after-free.

Checksum covers bytes 12 to end of page. Computed via CRC32C (Castagnoli,
hardware-accelerated when SSE4.2 or ARMv8 CRC32 is available, software
fallback otherwise). On write: `PageHeader.WriteHeader` leaves checksum
at 0, then `ComputeAndWriteChecksum` computes and stores it. On read:
`ValidateHeader` reads the header, recomputes CRC, compares.

### 2.2 Page Types

```
Type   Name        Purpose
────   ────        ───────
0x00   Superblock  Tree root and epoch state (§4)
0x01   Bitmap      Free-page bitmap page §3.8
0x02   TreeNode    Directory, file, or section node (§5)
0x03   PoolPage    Version node pool (§5.4)
0x04   MapperPage  Epoch mapper (§8.5)
0x05   Data        User file content
```

The StorageBackend header page (logical page 0) uses a special pageType value
of `0x5658` and flags of `0x5346` — the ASCII string `"XVFS"` in little-endian
byte order. This serves as the VFS file magic number. On mount, the
StorageBackend reads logical page 0 and validates: `pageType == 0x5658 &&
flags == 0x5346 && CRC32C valid`. If any check fails, the file is not a valid
iXSphereVFS instance.

### 2.3 Bit-Set Helper

`R8(p, o)` / `R2(p, o)` / `W8(p, o, v)` / `W2(p, o, v)` read/write
integers from byte arrays at given offsets. `BitConverter` equivalents
in C. Used throughout for page-level serialization.

---

## 3. StorageBackend

The StorageBackend is the sole owner of page-level I/O, allocation, and free-list
management. All page indices used by the VFS data structures (section slots,
version node `physicalPage` fields, directory entries, superblock pointers) are
**logical page indices** allocated by and opaque to the StorageBackend. The VFS
never performs logical-to-physical translation.

### 3.1 File Layout

The backing file begins with a StorageBackend header block (at logical page 0,
ping-pong backed), followed by free-bitmap pages, followed by allocatable data
pages.

```
Logical page   Content
────────────   ───────
0              StorageBackend header (magic "XVFS" at pageType+flags, config + bitmap directory)
1..B           Free-bitmap pages (chained via bitmap directory in header)
B+1            First allocatable page — superblock (ping-pong pair)
B+2            Superblock alternate
...            remaining logical pages
```

**Header page layout (page_size-byte payload):**

```
Offset  Size  Field
──────  ────  ─────
 0       8    total_pages       (int64 — highest allocated logical page + 1)
 8       8    page_size         (int64 — payload size in bytes, default 8192)
16       8    first_data_page   (int64 — logical page index of first allocatable page)
24       8    flags             (int64 — reserved for future use)
32      32    reserved
64     ...   bitmap_dir[]      (array of int64 logical page indices; zero-terminated)
```

The header is zero-filled at allocation. `bitmap_dir` is a compact array of
bitmap page indices starting at offset 64. Since newly allocated pages are
zero-filled, unused entries are 0. The number of bitmap pages is the count
of non-zero entries. The first entry is always 1 (the first bitmap page).

**Bitmap page layout (page_size-byte payload):**

```
Offset  Size  Field
──────  ────  ─────
 0    page_size  bits[]          (page_size bytes = page_size × 8 bits, covering that many logical pages)
```

Bitmap pages have no `next` pointer — the header's `bitmap_dir` array is
the authoritative chain. To find the bitmap bit for logical page N:
`bitmap_index = N / (page_size * 8)`, `bit_offset = N % (page_size * 8)`.
Allocation scans across all bitmap pages via zone cursors; freed pages are
reused regardless of which bitmap page they reside on. New bitmap pages are
appended to `bitmap_dir` only when the last existing page is exhausted.

All pages, including the header block, use ping-pong §3.7.

### 3.2 Initialization

When opening a VFS instance, the StorageBackend checks whether the backing
file exists:

**File does not exist or is empty:**
1. Create the backing file.
2. Bootstrap: reserve logical pages 0–3 directly — `Allocate` cannot be used
   yet because no bitmap exists. Pages 0 (header), 1 (first bitmap), 2
   (superblock, reserved for VFS), and 3 (superblock alternate). All four
   are zero-filled and use ping-pong backing.
3. Write the header: `total_pages = 4`, `page_size = 8192` (default),
   `first_data_page = 4`, `flags = 0`. Write `1` to `bitmap_dir[0]`.
4. Write bitmap page 1: all bits set to `1` (free). Mark bits 0–3 (header,
   bitmap, superblock, superblock alternate) as allocated (`0`).
5. Return success. The VFS layer initializes the superblock (§4) on page 2.

**File exists:**
1. Read logical page 0 (header block). Validate: `pageType == 0x5658 &&
   flags == 0x5346` (the "XVFS" magic) and CRC32C is valid. If any check
   fails, the file is not a valid iXSphereVFS instance.
2. Read `total_pages`, `page_size`, `first_data_page`. Load the `bitmap_dir`
   array (all non-zero entries). Read all referenced bitmap pages.

### 3.3 Page Allocation

```
int64_t Allocate(int count);
void    FreePage(int64_t logicalPage);
```

- `Allocate(count)`: reserves `count` contiguous logical pages. Each logical
  page is internally backed by a ping-pong physical pair §3.7. Returns the
  first logical page index, or -1 if no space.
- `FreePage(logicalPage)`: marks the logical page as free, releasing the
  underlying ping-pong pair. GC is the primary caller; normal VFS operations
  never free individual pages.

All newly allocated pages are zero-filled before returning.

**Capacity limit.** The `bitmap_dir` array in the header has a fixed capacity
of `(page_size − 64) / 8` entries. When all entries are non-zero and no
further bitmap pages can be allocated, the instance has reached its maximum
logical page count. `Allocate` returns -1. The maximum at page_size = 8192
is 1,016 bitmap pages covering 66,570,496 logical pages (~545 GB).

**Thread safety.** `Allocate` is thread-safe. Allocation is zone-based: the
logical page space is divided into zones of 1M pages. `Allocate` picks a
zone by thread ID, scans from a per-zone hint cursor for `count` consecutive
free bits using atomic bit operations. Falls back to adjacent zones if the
home zone is full.

When no free pages exist in any zone, a new bitmap page must be allocated.
The StorageBackend extends the backing file, allocates the new bitmap page,
zero-fills it, and appends its index to the first zero slot in `bitmap_dir`
via CAS. Then it updates `total_pages` in the header via CAS. The header
page is ping-pong backed §3.8  — the CAS on `bitmap_dir` entries and
`total_pages` is safe because the underlying page write is crash-safe.

### 3.4 Page I/O

The StorageBackend manages the 12-byte PageHeader transparently. Callers
pass and receive page_size-byte payload buffers. The StorageBackend reads and
writes the full page_size + 12 bytes (header + payload) internally, computing and
validating CRC32C on the payload on every I/O operation.

```
uint8_t* ReadPage(int64_t logicalPage);
void     WritePage(int64_t logicalPage, uint8_t* data);
void     Flush(void);
```

- `ReadPage`: checks the unified page cache §3.7  first. On cache hit,
  returns the cached buffer immediately (CRC32C was validated on initial
  load). On cache miss, reads the physical page pair from disk, validates
  the active half via generation, validates CRC32C on the payload, and
  inserts into the cache. Returns NULL if the page has never been written.
- `WritePage`: accepts a page_size-byte payload buffer. The StorageBackend
  writes it to the inactive physical half with incremented generation and
  computed CRC32C §3.7. Marks the page dirty but does not write to disk.
- `Flush`: writes all dirty pages to the backing file and fsyncs. Marks
  them clean. Clean pages remain cached. Called explicitly by the VFS
  layer at commit boundaries — this is the durability barrier.
- **Write-back:** the StorageBackend may proactively write dirty pages to
  disk (without fsync) when the dirty page count exceeds a configurable
  threshold. Write-back follows the same ordering as Flush (§3.5) to
  maintain on-disk consistency. It is non-blocking and does not mark pages
  clean — it only reduces the data loss window between explicit Flush
  calls. Crashes between write-back and Flush may lose un-fsynced data
  but cannot corrupt pages (ordering is preserved; ping-pong protects
  individual writes).

### 3.5 Flush Ordering

`Flush` writes dirty pages in a specific order to guarantee crash safety.
If a crash occurs mid-flush, the on-disk state must always be consistent —
either the pre-flush state or a recoverable partial state.

**Write order (must be followed strictly):**

1. **Data pages.** User file content — the leaves of the dependency tree.
   Nothing references these except version nodes.
2. **Pool pages.** Version nodes that reference data pages via `physicalPage`.
3. **Section pages.** Slot arrays that reference pool pages via VirtualPtr.
4. **Directory and file node pages.** Entries that reference section pages,
   child nodes, and FileSize entries.
5. **Mapper pages.** Epoch mapping entries.
6. **Bitmap pages.** Free-page bitmap state.
7. **Superblock.** Written last. This is the atomic commit point.

**Crash during flush:**

- Crash before step 7 (superblock not written): on remount, the old
  superblock still points to the old tree. Any pages written in steps 1–6
  are unreachable zombies — wasted space reclaimed by the next GC. No
  corruption.
- Crash after step 7 (superblock written): all preceding pages are on disk.
  Mount loads the new state. The flush is complete from the reader's
  perspective even if `fsync` was interrupted — the superblock ping-pong
  mechanism §3.8  ensures the active half points to a consistent state.

**Fsync:** after writing all dirty pages, `fsync` is called on the backing
file. If the OS or drive reorders writes, `fsync` acts as a barrier —
pages written before the fsync are durable before pages written after.

### 3.6 Unified Page Cache

Pages in the cache are in one of two states: clean (read from disk,
unmodified) or dirty (written via `WritePage`, not yet flushed). Eviction:
when a configurable memory budget is exceeded (default 256 MB, ~32,768
pages), evict clean pages via LRU. Dirty pages are never evicted. The
cache is thread-safe (concurrent reads/writes).

### 3.7 Ping-Pong Pages

Every logical page is backed by two physical pages for crash-safe writes.
When `Allocate` is called, the StorageBackend internally reserves two
adjacent physical pages and returns a single logical page index. Both halves
have full PageHeaders (§2). The active half is the one with the higher
`generation` field. Cold start: half 0 is active.

**Physical layout.** Each logical page is stored as one contiguous block of
`2 × (page_size + 12)` bytes:

```
Offset                Size   Content
──────                ────   ───────
0                      12    PageHeader for half 0
12                     12    PageHeader for half 1
24                page_size  Payload for half 0
24 + page_size    page_size  Payload for half 1
```

Both headers are adjacent — a single 24-byte read fetches both generations.
The winning half's payload follows at a known offset.

**Write:** read the active half's generation. Build the new payload in
memory. Compute CRC32C of the payload. Write the payload to the inactive
half, then write the PageHeader with generation = active.generation + 1
and the computed CRC32C. The header write is the atomic commit point —
the generation increment makes this half active.

**Read:** read both PageHeaders (24 contiguous bytes at offset 0). Use the
half with higher generation. Read its payload at offset `24 + (half * page_size)`.
Validate CRC32C. If CRC fails, read the other half's payload as fallback.
If both fail, the page is unrecoverable.

**Crash safety:** a crash mid-write leaves the old half intact (generation
not updated) or the new half intact (CRC valid). No torn page is ever
visible. All page types — superblock, bitmap, tree nodes, pool pages,
mapper pages, and data pages — share this mechanism. There is no distinction
between "data" and "metadata" durability.

### 3.8 Free-Page Bitmap (Internal)

The StorageBackend maintains a private free-page bitmap. One bit per
allocatable logical page. `1` = free, `0` = allocated.

**Layout.** The header's `bitmap_dir` array (§3.1) lists bitmap page indices.
Bitmap pages have no internal linking — the directory is authoritative. Each
bitmap page holds page_size * 8 bits (a full page_size-byte payload). Bit position `N`
in page `bitmap_dir[M]` corresponds to logical page `M * page_size * 8 + N`.

**Growth.** When `Allocate` needs a page beyond the current bitmap capacity,
a new bitmap page is allocated, zero-filled (all bits `0` = allocated), then
its index is written to the first zero slot in `bitmap_dir`. The new page's
bits are then set to `1` (free) for the newly covered range.

**Mount.** On mount, `bitmap_dir` is scanned. All referenced bitmap pages are
read. The allocator is initialized with the free/allocated state.

**GC.** The bitmap chain is rebuilt during the GC copy phase. Unused bitmap
pages are freed.

The VFS layer never accesses the bitmap directly.

---

## 4. Superblock

The first page (page 0) of the VFS backing file is a superblock containing
the tree root pointer and epoch state. It is the single entry point for
mount and recovery. The superblock is allocated by the VFS layer at `first_data_page` (logical
page 2 in a fresh instance) after StorageBackend initialization. The
StorageBackend provides the blank pages; the VFS writes the initial
superblock values.

### 4.1 Layout (page_size-byte payload)

```
Offset  Size  Field
──────  ────  ─────
 0       8    rootNodeOffset  (int64 — byte offset of tree root node)
 8       8    currentEpoch    (int64 — latest epoch counter)
16       8    epochMapperPage (int64 — page index of first epoch mapper page, 0 if none)
24       8    poolListHead    (int64 — head of pool page flat list, 0 if none)
32       8    treeLockState   (int64 — §9.6 bit layout)
40      16    reserved
56    8136    reserved / future use
```

### 4.2 Atomic Root Swap

The tree root is never modified in-place. When GC produces a new tree,
the new root offset is written to a fresh superblock page, and the
superblock pointer is swapped atomically. This guarantees the old tree
remains consistent until the swap completes.

Two superblock pages (page 0 and page 1) in a ping-pong arrangement:
- Write new superblock to the inactive page.
- fsync.
- Write the active page indicator (the `generation` field in PageHeader
  of the new page, incremented beyond the old page's generation).
- On mount: read both pages. The page with the **higher generation field**
  is the active one. If generations are equal (should not happen), prefer
  the page with the higher CRC32C. If both identical, prefer page 0.
- The previous active page becomes the inactive page for the next swap.

### 4.3 Tree Lock

`treeLockState` gates GC: shared lock for normal operations, exclusive for
GC. Bit layout and crash recovery semantics are in §9.6.

### 4.4 Mutable Field Persistence

`poolListHead` and `treeLockState` are CAS'd on **in-memory copies**, not
directly on the on-disk superblock byte array. They are written to disk
during GC (which rebuilds and flushes the superblock). Between GC cycles:

- `poolListHead` is purely in-memory. On crash, the on-disk value may be
  stale. This is safe — pool pages are also reachable via VirtualPtrs in
  section slots. GC walks the tree and rebuilds the complete pool list.
- `treeLockState` is always 0 at flush time (no operations in flight during
  a flush), so recovery zeros it unconditionally ping-pong §3.7.

---

## 5. Node Layout

Each node occupies one or more linked page_size-byte pages. Entries within a page form
a singly-linked list ordered by epoch descending (newest prepended at head).

### 5.1 Directory and File Node Entry Format (24 bytes + variable name)

Used in **directory and file nodes**. These are linked-list entries within a
directory or file node page.

```
Offset  Size  Field
──────  ────  ─────
 0       4    epoch          (uint32 — epoch this entry belongs to)
 4       2    type           (uint16 — DirChild, FileChild, or Tombstone)
 6       2    nameLen        (uint16 — length of name in bytes)
 8       8    childOffset    (int64  — page index of child node, 0 for Tombstone)
16       8    nextOffset     (int64  — byte offset of next entry in this page, or 0)
24      n     name           (UTF-8, nameLen bytes, max 255 per POSIX)
```

**Section entries (type=0x04):** `nameLen` is repurposed as `sectionIdx`
(uint16), identifying which 1021-page segment of the file this section
page covers (0, 1, 2... up to 65,535 → 535 GB max logical file size).
With ping-pong page backing §3.7, physical storage is up to ~1.1 TB for a
fully written 535 GB logical file. No name bytes follow. `childOffset` is
the section page index.

### 5.2 Virtual Pointer

A `VirtualPtr` is a 6-byte (48-bit) packed value referencing a version node
inside a pool page. It is stored in an **8-byte aligned slot** with 2 bytes
of trailing padding for native 64-bit CAS compatibility.

```
VirtualPtr = (poolPageNumber << 10) | (slotIndex & 0x3FF)
```

- Bits 0-9: slot index within the pool page (0-339, max 340 slots)
- Bits 10-47: physical page number of the pool page (supports multi-TB)

Resolution: `poolPage = vp >> 10; slot = vp & 0x3FF`. O(1).
A `VirtualPtr` of 0 means null (end of chain or unallocated).
Pool pages are never allocated at page index 0 (reserved for the
superblock), so `(poolPage=0, slot=0)` is never a valid reference.

### 5.3 Version Node (24 bytes, stored in Pool pages)

Each version node records one epoch of one logical page's history.
They are packed 340 per pool page and linked via `next` VirtualPtrs.

```
Offset  Size  Field
──────  ────  ─────
 0       4    epoch          (uint32)
 4       4    dataCrc32c     (uint32 — CRC32C of the data page; 0xFFFFFFFF if unknown)
 8       8    physicalPage   (int64  — data page index)
16       8    next           (VirtualPtr — next version in chain, 0 = end)
```

The `dataCrc32c` field stores the CRC32C of the physical data page's content
(excluding the page header). The sentinel value 0xFFFFFFFF means "unknown."

On write: compute CRC32C of the new content, store via `atomic_store_release`.
On read: recompute the data page's CRC32C. If it matches `dataCrc32c`, the
page is valid. If not, the ping-pong mechanism §3.8  tries the backup half.
If both halves fail CRC, fall back to the previous version in the chain.

This provides defense-in-depth against silent corruption (SSD bit rot, memory
errors) beyond the ping-pong torn-write protection. The CRC is checked on
every read, so corruption is detected immediately, not at mount time.

Total: 24 bytes. Available payload in a pool page: page_size − 16 (nextPoolPage
+ poolState + reserved) = page_size − 16 bytes. ⌊8,176 / 24⌋ = 340 per pool page.

### 5.4 Pool Page

An page_size-byte page storing up to 340 version nodes in a contiguous array.

```
Offset  Size  Field
──────  ────  ─────
 0       8    nextPoolPage   (int64 — next pool page in flat list; write-once, immutable after insertion)
 8       4    poolState      (uint32 — packed: bits 0–15 = firstFreeSlot, bits 16–31 = freeCount)
12       4    reserved       (uint32)
16    8160    slots[340]     (340 × 24 bytes version nodes; 8,160 + 16 overhead = 8,176 ≤ page_size)
```

`poolState` packs `freeCount` and `firstFreeSlot` into a single 32-bit word
for atomic CAS:
```
poolState = (freeCount << 16) | (firstFreeSlot & 0xFFFF)
```

Allocation from a pool:

Pool pages form a **flat lock-free list** rooted at the superblock's
`poolListHead`. Each page's `nextPoolPage` is write-once — set when the
page enters the list, never modified after. Threads scan the list for any
page with `freeCount > 0` and CAS-allocate on that page's local `poolState`.

1. Walk from `poolListHead` via `nextPoolPage`. Find the first page where
   `freeCount > 0`. If all pages are full (freeCount == 0 on every page):
   a. Allocate a new pool page from FreeBitmap.
   b. Set its `nextPoolPage` to the current `poolListHead`.
   c. CAS `poolListHead` in the superblock from old value to new page.
      If CAS fails, another thread added a page — discard (or return) this
      one and retry from the winning `poolListHead`.
   d. Use the newly added page.
2. On the chosen page, read `poolState`. Extract `freeCount` and `firstFreeSlot`.
3. Read the free slot's `next` field (low 16 bits = next free slot index).
4. CAS `poolState` to `((freeCount-1) << 16) | (nextFreeSlot & 0xFFFF)`.
   If CAS fails (another thread raced on the same page), retry from step 1
   (pick any page, not necessarily the same one).
5. Write the version node fields into `slots[firstFreeSlot]`.
   Return `(poolPage << 10) | firstFreeSlot`.

**Contention profile:** most allocations hit step 2-4 on pages with free
slots — only the local `poolState` CAS, no global lock. Only when all
pages are full does step 1d touch `poolListHead` (once per 340 allocations
per page). Threads naturally distribute across different pool pages.
Under contention, CAS failures on the head page cause threads to disperse
to other pages with free slots; in steady state the head page is the
common target but contention is bounded by the retry loop.

Freeing (during GC compaction only — no individual frees):
- Pool pages for deleted epochs are rebuilt from scratch via array filtration
  (see §10.3). Individual slots are never freed during normal operation.
- When a slot is free, the low 16 bits of its `next` field store the next
  free slot index (0–339). The upper 32 bits are undefined and must be masked.
- **Initialization:** on a freshly allocated pool page, `slot[i].next.lo16`
  is set to `i+1` for `i < 339`. `slot[339].next.lo16` is `0xFFFF` (terminal
  sentinel). `firstFreeSlot = 0`, `freeCount = 340`.

### 5.5 Section Page (pageType = TreeNode)

A fixed-size array of 1,021 VirtualPtr slots, one per logical page in an
~(page_size × 1021) file range. Each slot is an 8-byte aligned `long` storing a 6-byte
VirtualPtr with 2 bytes trailing zero-padding. Each slot is the **head** of
a version chain for that logical page.

```
Offset  Size  Field
──────  ────  ─────
 0       8    fileNodePage   (int64 — page index of this file's node, for GC back-ref)
 8       4    reserved
12    8168    slots[1021]    (1021 × 8 bytes, 8-byte aligned)
```

Total: 8 + 4 + 8,168 = 8,180 bytes payload; page_size − 8,180 bytes padding.

`slots[P] = 0` means the page has never been written. Non-zero means follow
the version chain starting at that pool slot. A slot value of 0 is the null
VirtualPtr.

### 5.6 Node Kinds

- **Directory node:** holds `DirChild`, `FileChild`, `Tombstone` entries as a
  linked list within the page (§5.1). Names are variable-length.
- **File node:** holds `Section` entries as a linked list. One entry per epoch
  that a new section was allocated. `childOffset` = page index of the section
  page. `nameLen` repurposed as `sectionIdx` (0, 1, 2...). type = 0x04.
- **Section node:** fixed array of 1021 VirtualPtrs (§5.5). No linked list.
- **Pool node:** fixed array of 340 version nodes (§5.4). Global, shared by
  all sections.

Entry types (directory/file nodes only — section and pool nodes use
their own fixed layouts):

| Type | Name | childOffset | Used in |
|---|---|---|---|
| 0x01 | DirChild | Page index of child directory | Directory nodes |
| 0x02 | FileChild | Page index of child file | Directory nodes |
| 0x03 | Tombstone | 0 | Directory nodes |
| 0x04 | Section | Page index of section page; nameLen=sectionIdx | File nodes |
| 0x05 | FileSize | New file size in bytes (repurposed: childOffset stores the size, not a page index) | File nodes |

### 5.7 Example Tree

```
root (dir):
  [epoch=0, DirChild,  name="docs",    child=pg20]
  [epoch=0, FileChild, name="readme",  child=pg30]

pg20 — docs (dir):
  [epoch=0, FileChild, name="rpt.txt", child=pg40]

pg30 — readme (file):                ← 2-page file, section 0
  [epoch=0, Section, sec=0, child=pg70]

pg40 — rpt.txt (file):               ← single-page file, section 0
  [epoch=0, Section, sec=0, child=pg71]

pg70 — section 0 of readme:         pg71 — section 0 of rpt.txt:
  slots[0] → 50:0                     slots[0] → 50:2
  slots[1] → 50:1

Pool page 50:
  [0] {epoch=4, phys=301, next=0}
  [1] {epoch=2, phys=200, next=50:3}
  [2] {epoch=0, phys=500, next=0}
  [3] {epoch=0, phys=100, next=0}
```

Read `readme` page 0 at epoch 4: `section[0]` → pool 50 slot 0 → version
{epoch=4, phys=301}. Epoch matches exactly. ✓

Read `readme` page 1 at epoch 3: `section[1]` → pool 50 slot 1 → version
{epoch=2}. Epoch 2 is the highest even ≤ 3. phys=200. ✓
(If a next entry existed with a lower even epoch, it is ignored — first
match with a valid epoch wins, because the chain is maintained in descending
epoch order.)

### 5.8 Directory Node Header & Page Chaining

Directory nodes use the linked-list entry format (§5.1). Each directory node
page has a 12-byte PageHeader (type `TreeNode`), then:

```
Offset  Size  Field
──────  ────  ─────
 0       8    headOffset     (int64 — byte offset of first entry in this page)
 8       2    entryCount     (uint16)
10       6    reserved
16      ...   entries        (variable-length, linked via nextOffset)
```

If entries overflow a page, allocate a new overflow page. The directory
node header is only on the first page. Overflow pages have this layout:

```
Offset  Size  Field
──────  ────  ─────
 0       8    nextOverflow   (int64 — page index of next overflow page, 0 = end)
 8       2    entryCount     (uint16 — entries in this overflow page)
22       2    reserved
12    8168    entries        (variable-length, linked via nextOffset)
```

**Entry traversal across pages:** `nextOffset` points to the byte offset of
the next entry within the current page. A value of 0 signals either "end of
chain" or "continue on the next overflow page." The traversal algorithm:

1. Read entry at current offset. Process it.
2. If `nextOffset != 0`: jump to `nextOffset` within the current page.
3. If `nextOffset == 0`: check the current page's `nextOverflow` field.
   - If `nextOverflow != 0`: switch to that page, continue from its offset 24.
   - If `nextOverflow == 0`: end of chain.
4. `nextOffset` of 0 with `nextOverflow` of 0 is the terminal sentinel.

**Memory ordering:** when a writer links a new overflow page, it must use
a release store before updating the preceding page's pointer. Readers use
acquire loads when following `nextOverflow`. See §9.2.1.
   wrap-around, but this should not occur in practice — use nextOverflow
   instead.

File nodes use the same linked-list structure (Section entries only, no names).
Section and Pool pages use the fixed layouts in §5.4 and §5.5 — no linked lists.

---

## 6. Read Rule

To read at epoch R:

0. **Apply epoch mapper** (§8.3): resolve R → R' via the mapper.
   All subsequent traversal uses R'.
1. Start at `headOffset` (directory/file nodes) or `section[logicalPage]`
   (section nodes).
2. For directory/file nodes — walk linked list (following `nextOffset`).
   For section nodes — resolve VirtualPtr → pool slot, walk version chain
   via `next` VirtualPtrs.
3. If entry.epoch == requested epoch → use it.
4. If entry.epoch > requested epoch → skip (from a later epoch).
5. If entry.epoch < requested epoch AND entry.epoch is even → use it. Stop.
6. If entry.epoch < requested epoch AND entry.epoch is odd → skip. Continue.

(With entries ordered by epoch descending, step 5 hits the first even epoch
below target — immediate stop. Chains are maintained in descending order by
construction: new entries are always CAS-prepended at the head.)


For directory entries: a single entry per (child, epoch) — the first match
per the read rule above gives the child's name and node pointer.

For version nodes: resolve `section[logicalPage]` → VirtualPtr → pool slot.
Walk the version chain via `next` VirtualPtrs. Find the first version where
`epoch` satisfies the read rule (highest even ≤ requested). Use
`physicalPage` from that version.

For file nodes: compute `sectionIdx = logicalPage / 1021`. Walk the linked
list. Find the first Section entry where `sectionIdx` (stored in the
`nameLen` field for type=0x04) matches AND the epoch satisfies the read
rule. Use `childOffset` as the section page.

---

## 7. Write Rule

All writes occur within a **write epoch** — the live head epoch (even) or
a snapshot epoch (odd).

### 7.1 Data Page Write

Given file node F, logical page P, offset O within page, data D, write epoch E:

1. Locate section node: `sectionIdx = P / 1021`. If no Section entry exists
   for this section at epoch E (or highest even < E), create a new section
   page (allocate from FreeBitmap, zero-fill slots).
2. Read `vp = section[P]` (the VirtualPtr, 0 if never written).
3. Walk the version chain starting at `vp`. Find any existing version node
   with `epoch == E`.
   - **FOUND** → in-place write. Ping-pong write to the data page ping-pong §3.7:
     write new content to inactive half, increment generation.
     Update `version.dataCrc32c` via `atomic_store_release`.
   - **NOT FOUND, vp == 0 (first-ever write):** Allocate new data page Q
     (two physical pages for ping-pong). Write content to Q.half[0],
     set generation=1. Compute CRC32C. Allocate pool slot. Write version
     node `{epoch=E, physicalPage=Q, next=0, dataCrc32c=...}`.
     CAS `section[P]` from 0 to new VirtualPtr.
   - **NOT FOUND, vp != 0 (standard COW):** Resolve the base physical page
     (highest even < E by walking the chain). Allocate new data page Q
     (two physical pages). Read base page active half → copy to Q.half[0]
     with overlay of D at intra-page offset O. Set generation=1.
     Compute CRC32C. Allocate pool slot. Write version node `{epoch=E,
     physicalPage=Q, next=vp, dataCrc32c=...}`. CAS `section[P]` from
     `vp` to new VirtualPtr (§9.1).
4. If offset + length extends beyond current fileSize, update by CAS-prepending
   a `FileSize` entry (type=0x05, epoch=E, childOffset=newSize) to the file
   node. On read, resolve via the read rule (§6); 0 if no entry exists.

### 7.2 Directory Operations

- **Create:** CAS-prepend `{epoch=E, type=DirChild|FileChild, name, childOffset}` to the parent node.
- **Delete:** CAS-prepend `{epoch=E, type=Tombstone, name, childOffset=0}`.
- **Rename:** Delete at source (tombstone) + Create at destination. Two CAS
  operations — **not atomic**. Neither same-directory nor cross-directory
  rename is atomic to concurrent readers. Even same-directory rename has a
  brief window where the old name is tombstoned and the new name is absent,
  leaving neither path visible. This is a known divergence from POSIX
  rename(2) which guarantees atomicity for same-directory renames.

### 7.3 Copy

Given source file node S, destination directory node D, write epoch E:

1. Create a new file node under D at epoch E (CAS-prepend FileChild entry).
2. Traverse S's file node to discover all Section entries (applying read
   rule for epoch E to pick the active section page for each offset range).
3. For each source section page:
   a. Allocate a new target section page from FreeBitmap.
   b. For each slot P (0..1020) in the source section:
      i. Follow the version chain using read rule for epoch E to find the
         active physical data page Q. If Q is 0 (never written), leave the
         target slot as 0.
      ii. If Q exists: allocate a new data page P'. Read Q → write P'
          (full page copy). Compute CRC32C(P').
      iii. Allocate a new pool slot (§5.4). Write version node
           `{epoch=E, physicalPage=P', next=0, dataCrc32c=...}`.
      iv. Write the new VirtualPtr into the target section slot[P].
   c. CAS-prepend Section entry `{epoch=E, childOffset=targetSectionPage}`
      to the target file node.
4. The copy is independent of the source after this point — COW on first
   write to the copy will create new version nodes in the target's chains.

---

## 8. Epoch Mapping

A global dictionary `epochMapper: Map<int, int>` with a `traversalApply` flag
per mapping.

### 8.1 Commit (snapshot → live head)

To commit snapshot epoch S (odd) to live head E (even, E = S+1):

1. Scan all nodes modified in both S and E. Conflict: if any file has extent
   entries for BOTH epoch S and epoch E covering the same offset range → abort.
2. Add mapping `S → E` with `traversalApply = true`.
3. Subsequent reads at epoch E: when encountering an epoch S entry, replace
   S with E and include it. The committed snapshot data becomes visible to
   the live head.

### 8.2 Delete Snapshot (soft)

To soft-delete snapshot S:

1. Add mapping `S → S-1` with `traversalApply = false`. The deleted epoch
   reverts to the pre-snapshot base (the highest even epoch < S).
2. Traversal at any epoch > S: when encountering epoch S, do NOT include it
   (flag false suppresses the entry). Effectively invisible.
3. Traversal at epoch S: query epoch S → replaced by S-1 via mapping → result
   is the base state without S's changes.

### 8.3 Read with Mapping

Given requested epoch R and a node:

1. Apply mapping to R: if mapper has R → R', use R'.
2. Traverse node entries. For each entry:
   - If `traversalApply` is true for the entry's epoch mapping: apply the
     mapping in the traversal (include the entry at the mapped epoch).
   - If `traversalApply` is false: skip the entry (do not include it).
   - If no mapping: proceed with normal read rule (see §6 steps 3–6).
3. The mapping context travels with the read — all nested node traversals
   use the same mapper.

**Resolution is single-hop.** `mapper.Resolve(R)` returns `R'` if an entry
exists for R, or `R` if no entry exists. Mappings are constrained to never
chain: inserting a mapping where `fromEpoch` or `toEpoch` already appears
in another mapping is an error. At commit time, if snapshot epoch S maps to
live head E, and a later soft-delete of S' would create a chain, the
soft-delete is rejected or GC consolidates the chain first. In practice,
mapping depth is always 1.

### 8.5 Epoch Mapper Page Layout (pageType = MapperPage, 0x0B)

The epoch mapper is stored in one or more linked page_size-byte pages. Each page holds
up to 680 entries. When full, a new page is allocated and linked via
`nextMapperPage` in the page header.

```
Offset  Size  Field
──────  ────  ─────
 0       8    nextMapperPage (int64 — page index of next mapper page, 0 = end)
 8       2    entryCount     (uint16 — entries in this page, max ~680)
22       2    reserved
24    8168    entries[]      (12 bytes each)
```

Each entry:
```
Offset  Size  Field
──────  ────  ─────
 0       4    fromEpoch      (uint32)
 4       4    toEpoch        (uint32)
 8       2    flags          (uint16 — bit 0 = traversalApply)
10       2    reserved
```

Allocation: when a page is full (entryCount == max), allocate a new page.
Set the current page's `nextMapperPage` to the new page. The mapper chain is
independent of the pool page chain; each is tracked by its own superblock
field.

On mount, the superblock's `epochMapperPage` points to the first mapper
page. Walk the chain via `nextMapperPage` to load all mappings.

GC (§10.3) compacts the mapper: drops entries for deleted epochs, collapses
committed mappings, rebuilds clean pages, frees old ones. Soft-deleted
mappings (traversalApply = false) remain until GC physically removes
them and the associated data pages.

---

## 9. Concurrency

### 9.1 Section Slot CAS

Section page slots are 8-byte aligned `long` values. Updates use native
64-bit CAS (`atomic_compare_exchange_strong`):

```
long oldVal, newVal;
do {
    oldVal = atomic_load_acquire(ref section[P]);
    // Build version node in pool slot with next = oldVal (current chain head)
    UpdateVersionNode(newPoolSlot, next: oldVal);
    newVal = PackVirtualPtr(newPoolPage, newPoolSlot);
} while (atomic_compare_exchange_strong(ref section[P], newVal, oldVal) != oldVal);
```

The `oldVal` read and version node construction MUST be inside the loop:
a concurrent CAS by another writer changes the chain head between attempts,
and the new version node's `next` field must point to the currently active
head at the moment of the successful CAS. The CAS is uncontested under
per-file serialization (§9.3) but is retained as a correctness invariant
and for future lock-free operation.

After a successful section slot CAS, the section page must be `WritePage`'d
to the dirty cache so the updated slot is persisted on flush. Similarly,
after an in-place `dataCrc32c` update via `atomic_store_release`, the pool page
containing that version node must be `WritePage`'d.

Pool allocation (§5.4) uses a per-pool-page CAS on `freeCount`/`firstFreeSlot`.

### 9.2 Directory Node CAS

Directory and file node entry insertions use CAS on the node's `headOffset`
(8-byte aligned, native CAS). The `oldHead` read and `newEntry.next`
assignment MUST be inside the retry loop:

```
long oldHead, newEntryOffset;
do {
    oldHead = atomic_load_acquire(ref headOffset);
    // newEntry already allocated; set its nextOffset = oldHead
    newEntry.nextOffset = oldHead;
    newEntryOffset = offset_of(newEntry);
} while (atomic_compare_exchange_strong(ref headOffset, newEntryOffset, oldHead) != oldHead);
```

CAS retries are rare — contention is only on creates/deletes in the same
directory. Data writes go through section slot CAS (§9.1) on different
section pages, with no contention between different files.

### 9.2.1 Overflow Page Memory Fences

When appending a directory or file node overflow page (§5.8), the writer
must guarantee publication ordering so lock-free readers never observe
uninitialized memory:

1. Allocate and fully populate the new overflow page buffer.
2. Compute its CRC32C and commit to the dirty cache via `WritePage`.
3. Emit a **release memory barrier** (e.g., `Thread.MemoryBarrier` or a
   volatile store) before linking the new page to the preceding page's
   `nextOverflow` or `nextOffset` pointer.

The lock-free reader must use an **acquire load** (`atomic_load_acquire`) when
following a non-zero `nextOverflow` or cross-page `nextOffset`. This
ensures all bytes inside the newly linked page are visible before
dereferencing the pointer.

**In-place CRC32C ordering:** When a writer does an in-place data page write
(version node already exists for this epoch), it must complete the data page
write BEFORE updating `dataCrc32c` in the existing version node, using a
release store (`atomic_store_release`). The lock-free reader must use an acquire
load (`atomic_load_acquire`) on `dataCrc32c`. If a CRC check fails (possible torn
read caught mid-write), the reader retries once before falling back to the
previous version in the chain.

### 9.3 Write Serialization (Per-File Locking)

Each file node has an associated lock. Before any write that modifies the
file's section pages, version chains, or file node entries, the writer
acquires the file's lock. The lock is released after all modifications
for that operation complete.

**Lock acquisition:**
```
mutex_t* lock = file_locks_get_or_create(fileNodePage);
mutex_lock(lock);
// ... perform writes ...
mutex_unlock(lock);
```

- The lock is stored in a thread-safe hash table keyed by file node page
  index (logical page, §3). Locks are created lazily on first access to a
  file and never removed.
- Same-file concurrent writes are serialized: only one thread may modify a
  given file's data at a time. This prevents races between COW version
  chain prepends on the same section slot.
- Different files have independent locks — no contention between writers
  to different tables or database files.
- Read operations do NOT acquire the file lock. Reads are lock-free: they
  traverse the version chain via `atomic_load_acquire` on the section slot and
  pool `next` pointers. The section slot CAS (§9.1) ensures a reader sees
  either the old chain head (before the write) or the new chain head (after
  the write) — never a torn intermediate state.
- Directory operations (create, delete, rename) CAS on the parent directory
  node's `headOffset` (§9.2). No file lock is involved — directory mutations
  are serialized by the CAS retry loop itself.

The lock is held for the duration of a single `WriteFile` call — typically
tens of microseconds — so contention is negligible. Deadlock is impossible
because each lock protects one file; no operation acquires multiple file
locks simultaneously.

### 9.4 Snapshot

Snapshot (`epoch += 2`) increments the epoch counter. **Before any write
at the new epoch proceeds, `currentEpoch` MUST be persisted to the superblock.**
The superblock page (two physical ping-pong pages) is written and fsync'd
directly — this is a targeted superblock flush, not a full dirty-cache flush.
Write the superblock bytes to the inactive physical half via WritePage,
then Flush only that page range. One fsync per snapshot. If the process
crashes before the flush, the on-disk superblock retains the old epoch.
On remount, the system re-uses that epoch — any previously written snapshot
data is unreachable via the old superblock pointer, but no corruption occurs
because the new snapshot data was never committed to disk.

### 9.5 Conflict Detection (Commit)

Commit scans version chains of all files modified in the snapshot epoch.
This is a read-only scan of the snapshot's dirty set — a `HashSet<long>`
of modified file node pages per epoch, maintained in-memory. On crash
before commit, the dirty set is lost; the next commit re-scans (safe:
over-scan finds no false conflicts, under-scan is impossible because
there's no commit to re-attempt after crash).

### 9.6 Tree Lock (GC Exclusion)

The global `treeLockState` in the superblock (§4.1) gates GC:

- Normal operations (read, write, snapshot, commit) acquire a **shared**
  read lock. Multiple operations proceed concurrently.
- GC acquires an **exclusive** write lock. All normal operations block
  until GC completes and releases the lock.

**Bit layout:**

```
bit 63           exclusive write lock (1 = GC active)
bits 32–62       reader count (max ~2 billion concurrent readers)
bits 0–31        reserved
```

**Reader acquisition:** before incrementing the reader count, check bit 63.
If set → GC is active → block (spin or sleep) until bit 63 clears. If clear
→ CAS-increment bits 32–62. This prevents new readers from starting after GC
has begun acquiring the write lock.

**Writer acquisition (GC):** CAS-set bit 63. After setting bit 63, spin until
bits 32–62 reach 0 (all pre-existing readers have exited). Then proceed with
GC. New reader acquisitions fail while bit 63 is set.

**Crash recovery:** on mount, if bit 63 is set, GC was interrupted — release
the lock and use the alternate superblock. If bit 63 is clear, unconditionally
zero bits 32–62 (all prior readers have exited).

---

## 10. Garbage Collection

GC is a manual, heavy operation that physically removes entries from deleted
epochs and frees data pages. **GC does not modify the live tree in-place.**
It builds a new, cleaned tree and swaps the root atomically via the superblock.

### 10.1 Trigger

Called explicitly after one or more epoch soft-deletes. Not automatic.

### 10.2 Tree Lock

Before starting, GC acquires the exclusive write lock via `treeLockState`
(layout in §9.6). All normal operations block until GC completes. This
prevents concurrent modifications during the copy phase.

### 10.3 Process

1. **Acquire exclusive tree lock.**
2. **Walk the live tree** from the current root. Process ALL node types:
   - **Section pages:** scan all 1021 slots. Follow each version chain.
     Drop version nodes belonging to soft-deleted epochs (per epoch mapper
     with `traversalApply = false`). Collapse committed mappings: if a
     version's epoch equals a committed snapshot epoch S, adjust to the
     live head epoch S+1. Build new version chains with surviving nodes,
     allocated sequentially into new pool pages.
   - **Directory and file nodes:** scan all linked-list entries. Drop
     Tombstone entries from deleted epochs. Drop Section/DirChild/FileChild
     entries from deleted epochs.
     - **FileSize, epoch S being soft-deleted:** completely DROP all FileSize
       entries belonging to S. The file falls back to the highest surviving
       baseline size entry at or below S−1.
     - **FileSize, epoch S being committed to live head E (E = S+1):** retain
       the highest FileSize entry within the snapshot range, rewrite its
       epoch to E, and discard all older FileSize entries.
     Build new directory/file node pages with surviving entries.
   - **Epoch mapper:** rebuild from surviving epochs. Drop mappings for
     deleted epochs. Collapse committed mappings (remove the mapping entry
     — the data has already been adjusted in step 2). Write new mapper
     pages. Free old mapper pages.
3. **Write new tree pages** — section, pool, directory, file, and mapper
   pages. Old pages are NOT freed until the swap is complete.
4. **Write new superblock** with the new root offset, `currentEpoch`
   page, and new `poolListHead` pointing to the first page of the rebuilt
   pool list.
5. **fsync** the new superblock.
6. **Atomically swap** the superblock pointer (ping-pong, §4.2).
7. **Release tree lock.**
8. **Free old pages.** Walk the old tree, free all pages not reachable from
   the new root. Every logical page freed releases its underlying ping-pong
   physical pair internally. This phase runs in the background and does not
   block normal operations.

### 10.4 Crash During GC

If a crash occurs before step 6, the old superblock and old tree remain
intact. Recovery mounts the old state. The partially-written new tree pages
are unreachable and will be reclaimed on the next GC cycle. No corruption
possible.

### 10.5 Page Pinning

Direct page access (§10.5) pins pages. Pin count
is tracked in an in-memory `thread-safe hash table<long, int>` keyed by page
index. Pinned pages are skipped during step 8 freeing.

### 10.6 History Removal

GC is a destructive operation from the perspective of historical snapshots.
After GC completes:

- **Soft-deleted snapshots:** all version nodes, directory entries, and data
  pages belonging exclusively to that epoch are permanently removed.
  Reading the deleted epoch returns the base state (what existed before the
  snapshot was taken).
- **Committed snapshots:** the mapping is collapsed. Version nodes from the
  snapshot epoch are relabeled to the live head epoch. Original live-head
  pages that were overwritten by snapshot variants are freed. Reading the
  committed epoch returns the live head state.

Callers must ensure no active readers or queries depend on deleted or
committed snapshot epochs before invoking GC.

---

## 11. Crash Consistency

### 11.1 Page Durability

All pages — superblock, bitmap, tree nodes, pool pages, mapper pages, and
data pages — are backed by ping-pong physical pairs §3.7. A crash
mid-write leaves the old half intact (generation not updated) or the new
half intact (CRC32C valid — generation was written last). No torn page is
ever visible. The reader picks the half with higher generation; if its CRC
fails, the backup half is used.

This eliminates the need for separate "torn-CAS recovery" for metadata
pages. The CAS-based updates to directory/file node `headOffset` and section
slot arrays are crash-safe because the underlying page write is ping-pong
protected. A CAS that succeeds logically but whose page write is torn on
disk is recovered by the ping-pong mechanism on the next read: the backup
half still holds the pre-CAS state, and the reader sees either the old or
new logical state — never a torn page.

### 11.2 Mount (No Recovery Needed)

1. Read both superblock pages (page 0 and page 1).
2. Select the active one: higher PageHeader generation wins (§4.2).
   Validate its CRC32C. If invalid, try the other page.
3. Read `rootNodeOffset`, `currentEpoch`, `epochMapperPage`, `poolListHead`.
4. Read the first epoch mapper page. Validate CRC32C. Walk the chain via
   `nextMapperPage`. Rebuild in-memory mapper dictionary.
5. Zero `treeLockState` reader count (in-memory only — the on-disk field
   is read only to check bit 63 for GC interruption; its reader count is
   always stale). If bit 63 is set, GC was interrupted; use the alternate
   superblock per §9.6.
6. System is ready. Tree pages are validated **lazily** on first access:
   - Directory/file page: CRC32C on header. If invalid, the last CAS was
     torn — skip the torn entry, continue from headOffset.
   - Section page: CRC32C on header. If invalid, treat all slots as 0
     (never written). Affected data pages become unreachable — reclaimed
     on next GC.
   - Pool page: CRC32C on header. If invalid, version nodes in that page
     are lost. Logical pages whose chain head (the section slot) points
     into the corrupt pool page appear as never written (slot treated as 0).
     Logical pages whose chain head is elsewhere but whose chain passes
     through a node in the corrupt page lose history below the corrupt
     entry but retain the head — the chain terminates at the corrupt node.
   - Data page: ping-pong handles CRC failure on read §3.7. No mount-time
     scan needed.

All corruption is detected and handled at read time, not mount time.
Pages that are never read after a crash need no validation.


## 12. Performance Model

Estimated per-operation cost for a single page_size-byte page write on the hot path.
Timings assume warm cache — pages resident in the unified page cache
§3.6, no disk I/O:

| Operation | Cost |
|---|---|
| Section array lookup + VirtualPtr decode | ~1 µs |
| Version chain walk (typical: 2 hops) | ~0.1 µs |
| Data page write (ping-pong: a page to inactive half, generation flip, CRC32C) | ~40 µs |
| Pool slot allocation (per-page CAS) | ~0.1 µs |
| Section slot CAS | ~0.1 µs |
| **Total per page write** | **~42 µs** |

At ~9 page writes per individual statement, predicted throughput is ~2,600
operations/second. For batched writes where metadata is amortized across
multiple writes within a transaction, throughput exceeds 80,000 rows/second.

The section array reduces per-page lookup from a linked-list scan to a
single indexed read (O(1)). Version chains are maintained in descending
epoch order; the latest live-head version is always the first entry.

Epoch counter is an `int64_t`. At 2 snapshots per second, overflow occurs in
~146 billion years — practically unbounded.
