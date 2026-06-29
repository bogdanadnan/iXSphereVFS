# iXSphereVFS — Epoch-Versioned Unified Tree (v2)

**Status:** Draft. Unified pool-based metadata with 32-byte fixed slots.
**Archive:** SPEC.v1.md — previous section-array-based model.

---

## 1. Overview

iXSphereVFS is a copy-on-write storage layer providing atomic snapshots,
crash-safe writes, and per-page versioning. All metadata — directory entries,
file mappings, version nodes, epoch mappings — is stored in a single pool
of fixed-size 32-byte entries. There are no separate page types for sections,
directories, or file nodes.

**Key properties:**

- **Universal pool.** One page type for all metadata. Fixed 32-byte slots:
  255 entries per 8KB page. Predictable allocation,
  simple free list.
- **Epoch-keyed.** Even epochs = live head, odd epochs = snapshots.
- **O(1) snapshots.** Increment a counter. No page writes.
- **Crash-safe.** Lazy mirror pages (§3) + pool CAS. Mount is O(1).
- **Conditional COW.** First write per epoch = copy; same-epoch = in-place.
- **In-memory arrays.** Page chains are loaded into in-memory arrays on first
  access for O(1) subsequent lookups.

---

## 2. Page Format

Every page consists of a 16-byte PageHeader followed by a page_size-byte
payload (page_size + 16 bytes total per logical page). A mirror sibling
may be allocated lazily on the second write (§3.7). The header is
transparent to the VFS layer — `Read` and `Write` operate on the payload
only.

### 2.1 PageHeader (16 bytes)

```
Offset  Size  Field
──────  ────  ─────
 0       2    pageType       (uint16 — type code from PageType enum)
 2       2    flags          (uint16 — bits 0–1 = flush priority (0=data, 1=pool, 2=bitmap, 3=superblock); bits 2–15 per-type reserved; StorageHdr uses 0x5346 for 'FS' magic)
 4       4    checksum       (uint32 — CRC32C of payload bytes)
 8       4    generation     (uint32 — incremented on each write; higher = active)
12       4    mirrorPage     (int32 — logical page index of mirror sibling; -1 = none)
```

`generation` catches stale reads and picks the active page when a mirror
exists. `mirrorPage` is -1 after first allocation; on the second write to
the page, a mirror sibling is allocated and both pages are linked via this
field. See §3.7 for the write lifecycle.

The 16-byte header is power-of-2 aligned. All page types use this layout.
The StorageBackend header (logical page 0) uses `pageType=0x5658, flags=0x5346`
to form the 4-byte "XVFS" magic in little-endian byte order.

### 2.2 Page Types

```
Type   Name        Purpose
────   ────        ───────
0x00   Superblock  Tree root and epoch state (§4)
0x01   Bitmap      Free-page bitmap page (§3.7)
0x02   PoolPage    Metadata pool page (§5.2) — all VFS metadata
0x03   Data        User file content
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
management. All page indices used by the VFS data structures (version chain heads,
version node `physicalPage` fields, directory entries, superblock pointers) are
**logical page indices** allocated by and opaque to the StorageBackend. The VFS
never performs logical-to-physical translation.

### 3.1 File Layout

The backing file begins with a StorageBackend header block (at logical page 0,
lazy mirror backed), followed by free-bitmap pages, followed by allocatable data
pages.

```
Logical page   Content
────────────   ───────
0              StorageBackend header page 0 (magic "XVFS", config + first ~1K bitmap_dir entries)
1              StorageBackend header page 1 (continuation of bitmap_dir array)
2..B           Free-bitmap pages
B+1            First allocatable page — superblock
...            remaining logical pages
```

The header spans two logical pages (0 and 1). Page 0 holds the config
fields and the first portion of `bitmap_dir`. Page 1 continues the
`bitmap_dir` array — it is a pure array of int64 entries with no header
fields. Both pages are lazy mirror backed (§3.7).

**Header page 0 layout (page_size-byte payload):**

```
Offset  Size  Field
──────  ────  ─────
 0       8    total_pages       (int64 — highest allocated logical page + 1)
 8       8    page_size         (int64 — payload size in bytes, default 8192)
16      16    reserved
32     ...   bitmap_dir[]      (array of int64; continued on page 1)
```

**Header page 1 layout (page_size-byte payload):**

```
Offset  Size  Field
──────  ────  ─────
 0     ...   bitmap_dir[]      (continuation of the int64 array from page 0)
```

The `bitmap_dir` array spans both pages. Entry N lives at:
- If `N < (page_size − 32) / 8`: page 0, offset `32 + N × 8`
- Otherwise: page 1, offset `(N − ((page_size − 32) / 8)) × 8`

Total entries: `(2 × page_size − 32) / 8` = 2,044 at default page_size.
Maximum logical pages: 2,044 × 65,536 = ~134M pages (~1.1 TB logical).

Config area: 32 bytes. `bitmap_dir` starts at offset 32, spans pages 0 and 1.
Entries: (2 × page_size − 32) / 8 = 2,044 at default page_size.

The header is zero-filled at allocation. `bitmap_dir` is a compact array of
bitmap page indices starting at offset 32. Since newly allocated pages are
zero-filled, unused entries are 0. The number of bitmap pages is the count
of non-zero entries. The first entry is always 2 (the first bitmap page).

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

All pages, including the header block, use the lazy mirror mechanism (§3.7).

### 3.2 Initialization

When opening a VFS instance, the StorageBackend checks whether the backing
file exists:

**File does not exist or is empty:**
1. Create the backing file.
2. Bootstrap: reserve logical pages 0–3 directly — `Allocate` cannot be used
   yet because no bitmap exists. Pages 0–1 are the StorageBackend header,
   page 2 is the first bitmap page, page 3 is the superblock. All four
   are zero-filled and use lazy mirror backing.
3. Write header page 0: `total_pages = 4`, `page_size = 8192` (default).
   Write `2` to `bitmap_dir[0]`.
4. Write bitmap page 2: all bits set to `1` (free). Mark bits 0–3 (header
   pages, bitmap, superblock) as allocated (`0`).
5. Return success. The VFS layer initializes the superblock (§4) on page 3.

**File exists:**
1. Read logical page 0 (header block). Validate: `pageType == 0x5658 &&
   flags == 0x5346` (the "XVFS" magic) and CRC32C is valid. If any check
   fails, the file is not a valid iXSphereVFS instance.
2. Read `total_pages` and `page_size`. Load the `bitmap_dir`
   array (all non-zero entries). Read all referenced bitmap pages.

### 3.3 Page Allocation

```
int64_t Allocate(int count);
bool    Acquire(int64_t logicalPage);
void    Free(int64_t logicalPage);
```

- `Allocate(count)`: reserves `count` contiguous logical pages. Returns the
  first logical page index, or -1 if no `count` contiguous pages exist.
  Multi-page allocations are rare (most callers use `Allocate(1)` or
  `Acquire`). If a contiguous block cannot be found, the caller may fall
  back to individual `Acquire` calls for each needed page.
- `Acquire(logicalPage)`: atomically checks whether `logicalPage` is
  free. If yes, marks it allocated and returns true. If already allocated,
  returns false. This is a CAS on the bitmap bit — no scanning, O(1).
  Used for fixed-location allocations (superblock, GC target pages).
- `Free(logicalPage)`: marks the logical page as free. If a mirror sibling
  exists (`mirrorPage != -1`), the sibling is also freed. GC is the primary
  caller; normal VFS operations never free individual pages.

All newly allocated pages are zero-filled before returning.

**Capacity limit.** The `bitmap_dir` array in the header has a fixed capacity
of `(page_size − 32) / 8` entries. When all entries are non-zero and no
further bitmap pages can be allocated, the instance has reached its maximum
logical page count. `Allocate` returns -1. The maximum at page_size = 8192
is 2,044 bitmap pages covering 134M logical pages (~1.1 TB at default page_size).

**Thread safety.** `Allocate` is thread-safe. Allocation is zone-based: the
logical page space is divided into zones of 1M pages. `Allocate` picks a
zone by thread ID, scans from a per-zone hint cursor for `count` consecutive
free bits using atomic bit operations. Falls back to adjacent zones if the
home zone is full.

When no free pages exist in any zone, a new bitmap page must be allocated.
The StorageBackend extends the backing file, allocates the new bitmap page,
zero-fills it, and appends its index to the first zero slot in `bitmap_dir`
via CAS. Then it updates `total_pages` in the header via CAS. The header
page is lazy mirror backed §3.7  — the CAS on `bitmap_dir` entries and
`total_pages` is safe because the underlying page write is crash-safe.

### 3.4 Page I/O

The StorageBackend manages the 16-byte PageHeader transparently. Callers
pass and receive page_size-byte payload buffers. The StorageBackend reads and
writes the full page_size + 16 bytes (header + payload) internally, computing and
validating CRC32C on the payload on every I/O operation.

```
uint8_t* Read(int64_t logicalPage);
void     Write(int64_t logicalPage, uint8_t* data);
void     Flush(int64_t logicalPage);
```

- `Read`: resolves the page via the lazy mirror mechanism (§3.7). Checks
  the unified page cache (§3.6) first. Returns a pointer to a page_size-byte
  buffer containing the payload only. Returns NULL if the page has never
  been written.
- `Write(logicalPage, data)`: accepts a page_size-byte payload buffer.
  Handles the lazy mirror lifecycle (§3.7). Marks the page dirty but does
  not write to disk. The flush priority (0–3) is read from the page header's
  `flags` bits 0–1, which were set at allocation time by the VFS layer.
- `Flush(logicalPage)`: if `logicalPage < 0`, writes all dirty pages to the
  backing file and fsyncs — the durability barrier, called at commit
  boundaries. If a specific page index is given, writes only that page
  without fsync. Used for targeted flushes like superblock updates (§9.4).
  Marks flushed pages clean; clean pages remain cached.
- **Write-back:** the StorageBackend may proactively write dirty pages to
  disk (without fsync) when the dirty page count exceeds a configurable
  threshold. Write-back follows the same ordering as Flush (§3.5) to
  maintain on-disk consistency. It is non-blocking and does not mark pages
  clean — it only reduces the data loss window between explicit Flush
  calls. Crashes between write-back and Flush may lose un-fsynced data
  but cannot corrupt pages (ordering is preserved; lazy mirror protects
  individual writes).

### 3.5 Flush Ordering

The StorageBackend reads the flush priority from each dirty page's
PageHeader `flags` field (§2.1). Logical pages 0 and 1 (the StorageBackend
header) are always flushed at priority 3 (superblock-level) regardless of
their `flags` value — the magic bytes in `flags` are only validated at
mount, not during flush. For all other pages, `Flush(-1)` writes in
priority order: 0 (data) first, 3 (superblock) last. Within each priority,
write order is unspecified. The VFS sets the priority in `flags` at
allocation time; the StorageBackend never modifies it.

**Write order (enforced by the StorageBackend via flush priority):**

1. **Data pages** (priority 0). User file content.
2. **Pool pages** (priority 1). All metadata.
3. **Bitmap pages** (priority 2). Free-page bitmap state.
4. **Superblock** (priority 3). Written last. This is the atomic commit point.

**Crash during flush:**

- Crash before step 4 (superblock not written): on remount, the old
  superblock still points to the old tree. Any pages written in steps 1–3
  are unreachable zombies — wasted space reclaimed by the next GC. No
  corruption.
- Crash after step 4 (superblock written): all preceding pages are on disk.
  Mount loads the new state. The flush is complete from the reader's
  perspective even if `fsync` was interrupted — the superblock lazy mirror
  mechanism §3.7  ensures the active half points to a consistent state.

**Fsync:** after writing all dirty pages, `fsync` is called on the backing
file. If the OS or drive reorders writes, `fsync` acts as a barrier —
pages written before the fsync are durable before pages written after.

### 3.6 Unified Page Cache

Pages in the cache are in one of two states: clean (read from disk,
unmodified) or dirty (written via `Write`, not yet flushed). Eviction:
when a configurable memory budget is exceeded (default 256 MB, ~32,768
pages), evict clean pages via LRU. Dirty pages are never evicted. The
cache is thread-safe (concurrent reads/writes).

### 3.7 Lazy Mirror Pages

Most pages are write-once, read-many. Allocating a full mirror pair for every
page would waste storage. Instead, a mirror sibling is allocated lazily on
the **second write** to the page.

**Write lifecycle:**

1. **First allocation:** `Allocate` or `Acquire` returns a single logical page.
   `mirrorPage = -1`, `generation = 0`.
2. **First write:** write payload, then write PageHeader with `generation = 1`,
   `mirrorPage = -1`. No mirror needed — this is the only copy. One page
   occupies `page_size + 16` bytes of physical storage.
3. **Second write:** allocate a new logical page (the mirror sibling). Write
   the new payload to the sibling with `generation = 2`, `mirrorPage = original`.
   Then atomically store `mirrorPage = sibling` on the original page (8-byte
   aligned write). From this point forward, writes alternate between the two
   pages, each with an incremented generation.
4. **Subsequent writes:** read both headers, find the inactive one (lower
   generation), write to it with `generation = active.generation + 1`.

**Physical layout.** Each page is independent:

```
Page: [PageHeader: 16 bytes | payload: page_size bytes]
```

**Read:**
1. Read the page header. If `mirrorPage == -1`: use this page directly.
   Validate CRC32C on the payload. Done. (Most pages — O(1).)
2. If `mirrorPage != -1`: read the sibling's header. Compare generations.
   Use the page with the higher generation. Validate CRC32C. If CRC fails,
   try the other page.

**Crash safety at mirror allocation (the critical moment):**
- Sibling allocated, written with gen=2, mirrorPage=original.
- Original's mirrorPage is still -1. Reader sees gen=1, mirrorPage=-1
  → lone page, old data intact. Sibling is a zombie (unreachable). Safe.
- After atomic store of `mirrorPage` on the original: both linked. Reader
  compares gens (2 > 1), picks sibling. Write #2 survived. Safe.

**Shared pool page writes:** pool pages contain entries from multiple files.
Writing to one file may update a slot on a page that also hosts metadata
for another file. When the StorageBackend mirrors this page, it writes the
entire payload — including unrelated entries. This is safe: those entries
haven't changed, and the generation increment on the pool page is a side
effect of the mirror write, not a logical change to the other file's state.

**Storage savings:** pages written only once (the common case for data pages
filled by INSERT and metadata pages created during tree growth) consume
`page_size + 16` bytes instead of `2 × (page_size + 16)`. For a typical database
where 90% of pages are write-once, physical storage is dominated by the single-page
case rather than mirror pairs, saving significant space over always-allocated mirror
pairs.

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

The superblock lives at logical page 3. It contains the tree root pointer
and epoch state — the single entry point for mount and recovery. It is allocated by the VFS layer at logical page 3 via
`Acquire` after StorageBackend initialization. Like every other page, it
uses standard lazy mirror (§3.7) — there is no separate superblock alternate
page or special swap protocol. The VFS writes superblock updates via
`Write`, which writes to the inactive half and increments generation
atomically. Mount reads both headers and picks the active half (§3.7).

### 4.1 Layout (page_size-byte payload)

```
Offset  Size  Field
──────  ────  ─────
 0       8    rootNodeOffset  (int64 — byte offset of tree root node)
 8       8    currentEpoch    (int64 — latest epoch counter)
16       8    epochMapperPtr  (VirtualPtr — first epoch mapper entry, 0 if none)
24       8    poolListHead    (int64 — head of pool page flat list, 0 if none)
32       8    treeLockState   (int64 — §9.6 bit layout)
40       4    nextNodeId      (uint32 — next available node identifier)
44       4    reserved
48    8144    reserved
```

### 4.2 Tree Lock

`treeLockState` gates GC: shared lock for normal operations, exclusive for
GC. Bit layout and crash recovery semantics are in §9.6.

### 4.3 Mutable Field Persistence

`poolListHead` and `treeLockState` are CAS'd on **in-memory copies**, not
directly on the on-disk superblock byte array. They are written to disk
during GC (which rebuilds and flushes the superblock). Between GC cycles:

- `poolListHead` is purely in-memory. On crash, the on-disk value may be
  stale. This is safe — pool pages are also reachable via VirtualPtrs in
  PageNode.versionRootPtr entries. GC walks the tree and rebuilds the complete pool list.
- `treeLockState` is always 0 at flush time under normal operation. If GC
  sets bit 63 and crashes before completion, the on-disk superblock retains
  bit 63 from the partially-written GC output. Recovery handles this case
  (§11.2 step 5).

---


## 5. Metadata Pool

All VFS metadata is stored in a global pool of fixed-size 32-byte entries.
Pool pages are allocated by the StorageBackend and chained via `poolListHead`
in the superblock.

### 5.1 Virtual Pointer

A `VirtualPtr` is an 8-byte packed value referencing a pool entry:

```
VirtualPtr = (poolPageIndex << 8) | (slotIndex & 0xFF)
```

- Bits 0–7: slot index within the pool page (0–254)
- Bits 8–63: logical page index of the pool page

`VirtualPtr` of 0 means null. Logical page 0 is the StorageBackend header
and is never a pool page, so `(page=0, slot=0)` is never a valid reference.

### 5.2 Pool Page Layout

```
Offset  Size  Field
──────  ────  ─────
 0       8    nextPoolPage   (int64 — next pool page in global allocator list; write-once)
 8       4    poolState      (uint32 — packed freeCount|firstFreeSlot)
12       4    reserved
16    8160    slots[255]     (255 × 32 bytes entries)
8176     16    reserved       (padding)
```

Total: 8192 bytes, 255 usable entries per page.

`nextPoolPage` links pool pages into the global allocator chain rooted at
`poolListHead`. Set once when the page enters the list, never modified. GC
builds new pool pages and atomically swaps `poolListHead` — no second chain
pointer is needed.

### 5.3 Pool Allocation

When a slot is free, bytes 0–1 store the uint16 index of the next
free slot (0xFFFF = terminal sentinel). All other bytes are undefined.

On a freshly allocated pool page: `slot[i].bytes[0:2] = i+1` for `i < 254`,
`slot[254].bytes[0:2] = 0xFFFF`, `poolState = (255 << 16) | 0`.

**Allocation:** CAS on `poolState` (packed `freeCount | firstFreeSlot`) to
allocate a free slot. When `freeCount == 0`, allocate a new pool page and
prepend to the global list via `poolListHead` CAS. If the CAS fails
(another thread prepended first), the losing thread retries by prepending
its allocated page to the updated `poolListHead` — the page is valid and
must not be abandoned. Individual slots are never freed — GC rebuilds pool
pages from scratch.

**Arena optimization (optional):** under high thread counts, contention on
the head page's `poolState` can be reduced by maintaining per-thread or
per-CPU arena cursors. Each arena has a preferred active pool page;
threads allocated to arena N CAS only on that arena's page. When the arena
page is exhausted, a new page is allocated and prepended globally with the
arena ID recorded in a reserved field, so other arenas skip it during their
scans. This bounds global CAS to page-exhaustion events (~once per 255
allocations) rather than every slot allocation.

### 5.4 Name Entries

Names are stored as chained pool entries, each holding 24 bytes of UTF-8
data and an 8-byte `nextPtr` to the next slot in the chain:

```
Offset  Size  Field
──────  ────  ─────
 0      24    data           (UTF-8 bytes, zero-padded if shorter than 24)
24       8    nextPtr        (VirtualPtr — next NameSlot, 0 = end of name)
```

A name of length N occupies `ceil(N / 24)` pool slots linked via `nextPtr`.
The DirContent entry's `namePtr` points to the first slot. No multi-slot
reservation is needed — each slot is individually allocated from the pool.
GC traverses the chain to free all slots.

---

## 6. Node Types

All nodes are pool-allocated 32-byte entries linked via `VirtualPtr` chains.
Each entry has an implicit type from its position in the chain; no explicit
type field is needed for most entries.

### 6.1 DirNode

```
Offset  Size  Field
──────  ────  ─────
 0       2    type           (uint16 — 0x01 = DirNode, 0x03 = FileNode)
 2       2    reserved
 4       4    nodeId         (uint32 — unique identifier, sequential)
 8       8    headPtr        (VirtualPtr — first DirContent entry)
16      16    reserved
```

A DirNode is the head of a directory. `headPtr` points to the most recent
`DirContent` entry (descending epoch order). `nodeId` is globally unique,
assigned sequentially from a counter in the superblock.

### 6.2 DirContent

```
Offset  Size  Field
──────  ────  ─────
 0       4    childNodeId    (uint32 — nodeId of the child being listed)
 4       4    epoch          (uint32)
 8       8    childPtr       (VirtualPtr — DirNode or FileNode of the child)
16       8    namePtr        (VirtualPtr — NameEntry; 0 = deleted)
24       8    nextPtr        (VirtualPtr — next DirContent in chain)
```

`childNodeId` is used to deduplicate entries during directory listing. When
multiple `DirContent` entries exist for the same child (renames across epochs),
only the highest epoch ≤ query is used. `namePtr = 0` means the child was
deleted in this epoch — it is excluded from listings. Same-epoch renames
replace `namePtr` in-place on the existing entry.

### 6.3 FileNode

```
Offset  Size  Field
──────  ────  ─────
 0       2    type           (uint16 — 0x03)
 2       2    reserved
 4       4    nodeId         (uint32)
 8       8    headPtr        (VirtualPtr — first FileContent entry)
16       8    sizePtr        (VirtualPtr — first FileSize entry, 0 if none)
24       8    createdAt      (int64 — Unix timestamp, set once at creation)
```

`createdAt` is immutable — written once when the file is created, never
modified. `sizePtr` points to a separate chain of `FileSize` entries; the
`headPtr` chain holds only `FileContent` entries.

Same layout as DirNode, distinct type. FileNodes have no names — the name
is in the parent directory's DirContent.

### 6.4 FileContent

```
Offset  Size  Field
──────  ────  ─────
 0       8    pageRootPtr    (VirtualPtr — first PageNode for this segment)
 8       8    nextPtr        (VirtualPtr — next FileContent, or epoch-keyed next)
16      16    reserved
```

A FileContent groups consecutive logical pages into segments. The segment
size determines the fan-out: N pages per FileContent means 1 FileContent per
N pages. At page_size=8192, the segment size is fixed at 1,024 pages (~8 MB). This value is
hardcoded and not stored on disk. `pageRootPtr` points to the first `PageNode` for this segment.

`FileContent` entries are NOT epoch-keyed — they form a permanent linked list
of segments that grows as the file expands. The `nextPtr` links to the next
FileContent (higher logical page range). On first access, the VFS walks this
chain and builds an in-memory array of pointers to the live `versionRootPtr`
fields within the pool page buffers. This ensures reads always see the
current version chain head via `atomic_load_acquire`, even after concurrent
writes CAS the `versionRootPtr`. The array persists until the file is closed
or the pool page backing the array entries is evicted.

A reader at any epoch walks the full FileContent chain, including segments
added in later epochs. This is safe: VersionPage epoch filtering (§7.2) makes
pages in future segments resolve as "never written." FileSize entries bound
which segments are logically valid for a given epoch.

### 6.5 PageNode

```
Offset  Size  Field
──────  ────  ─────
 0       8    versionRootPtr (VirtualPtr — first VersionPage for this page)
 8       8    nextPtr        (VirtualPtr — next PageNode in segment)
16      16    reserved
```

One `PageNode` per logical page within a FileContent segment. Forms a linked
list. `versionRootPtr` points to the most recent `VersionPage` (descending
epoch). On first access to a segment, PageNodes are loaded into an array.

### 6.6 VersionPage

```
Offset  Size  Field
──────  ────  ─────
 0       4    epoch          (uint32)
 4       4    reserved
 8       8    dataPage       (int64 — physical data page index)
16       8    nextPtr        (VirtualPtr — next VersionPage in chain)
24       8    reserved
```

Records one version of one logical page. The version chain (linked via
`nextPtr`) is maintained in descending epoch order. The data page's
integrity is covered by its own PageHeader CRC32C and the lazy mirror
mechanism.

---


### 6.7 FileSize

```
Offset  Size  Field
──────  ────  ─────
 0       4    epoch          (uint32)
 4       8    modifiedAt     (int64 — Unix timestamp of this size change)
12       8    fileSize       (int64 — file size in bytes)
20       8    nextPtr        (VirtualPtr — next FileSize entry)
28       4    reserved
```

FileSize entries are epoch-keyed and hang off the FileNode's `sizePtr` chain.
Each entry records the file size at a given epoch plus the modification
timestamp. `stat()` resolves the file size and modification time via the
read rule (§7.2) on the `sizePtr` chain.
## 7. Tree Traversal

### 7.1 Valid Epochs

- **Reads:** any epoch — even (live head, committed base) or odd (active
  snapshot). The read rule (§7.2) resolves the correct version chain entry.
- **Writes:** restricted to **current live head** (the highest even epoch)
  and **active snapshots** (odd epochs that have not been committed or
  soft-deleted). Writing to a committed or deleted snapshot is an error.
  Writing to an older even epoch is an error — only the current live head
  is writable among even epochs.
- A snapshot becomes inactive when `vfs_commit` or `vfs_delete_snapshot`
  is called. Epoch mapping entries (§8) track which snapshots are active.

### 7.2 Read Rule

The epoch mapper (§8) affects traversal in two ways:

1. **Query resolution:** the requested epoch R is mapped to `R' = mapper.resolve(R)`.
   This always applies — if a mapping `S→E` exists, requesting epoch S returns
   data from epoch E.
2. **Traversal remapping:** when `traversalApply = true` for a mapping `S→E`,
   entries with `epoch == S` encountered during chain walking are treated as
   `epoch == E` before applying steps 3–6. When `traversalApply = false`,
   entries are NOT remapped — their original epoch is used and the default
   read rules apply (odd epochs are skipped per step 6, which is the desired
   behavior for soft-deleted snapshots).

To resolve a chain at mapped epoch R':
1. Walk from the head pointer.
2. If `traversalApply` is true for the entry's epoch mapping: treat the entry
   as if its epoch were the mapped `toEpoch`, then apply steps 3–6.
3. If `traversalApply` is false for the entry's epoch mapping: skip the entry.
4. If no mapping exists for the entry's epoch: proceed to steps 3–6 directly.
3. If entry.epoch == R' → use it (exact match, regardless of even/odd).
4. If entry.epoch > R' → skip.
5. If entry.epoch < R' AND even → use it (committed base). Stop.
6. If entry.epoch < R' AND odd → skip (unrelated snapshot).

(Chains are descending by epoch, so the first match at step 3 or 5 is the
most recent committed state at or before R'.)

### 7.3 File Read

To read logical page P of file F at epoch E:
1. Resolve FileNode → walk FileContent chain → find the segment containing P.
2. Walk PageNode chain within the segment to the entry for page P.
3. Follow `versionRootPtr` chain, apply read rule for epoch E → get dataPage.
4. `Read(dataPage)` via StorageBackend.

After first access to a segment, the FileContent → PageNode chain is cached
in an in-memory array. Subsequent reads are `array[P_in_segment] → versionRootPtr`.

### 7.4 File Write

To write to logical page P of file F at epoch E:
1. Resolve to the PageNode (via in-memory array or chain walk).
2. Walk the VersionPage chain. If a VersionPage with `epoch == E` exists:
   - In-place write. `Write(dataPage, newPayload)`.
3. If no VersionPage with `epoch == E` exists:
   - COW: resolve base physical page (highest even < E in chain).
   - Allocate new data page Q. Copy base → Q with overlay.
   - `Write(Q, newPayload)`.
   - Allocate new pool slot for VersionPage {epoch=E, dataPage=Q, nextPtr=oldHead}.
   - CAS `PageNode.versionRootPtr` to the new pool slot.

### 7.5 Directory Operations

- **Create:** allocate DirNode or FileNode, create DirContent in parent with
  `namePtr` pointing to a new NameEntry, `childPtr` to the new node.
  CAS-prepend to parent's `headPtr`.
- **Delete:** create DirContent with `namePtr = 0`, `childNodeId` matching
  the target. CAS-prepend to parent.
- **Rename (same dir, same epoch):** update `namePtr` in-place on the existing
  DirContent entry to point to a new NameEntry.
- **Rename (new epoch or cross-dir):** create new DirContent at destination
  with `childPtr` and `childNodeId` from source. Create delete entry (namePtr=0)
  at source.

Directory listing is accelerated by an in-memory dentry cache built on
first read of a directory. The cache maps `(childNodeId → name, childPtr)`
and is invalidated when a new DirContent is prepended to the directory chain.
Subsequent listings use the cache, avoiding raw pool chain traversal for
hot-path lookups.

---

## 8. Epoch Mapping

Epoch mappings are stored as pool entries in a dedicated chain. The superblock
holds `epochMapperPtr` — a VirtualPtr pointing to the first mapper entry
in any pool page. The mapper chain is
walked via VirtualPtr `nextPtr` within pool pages. Each mapping is a pool entry:

```
Offset  Size  Field
──────  ────  ─────
 0       4    fromEpoch      (uint32)
 4       4    toEpoch        (uint32)
 8       2    flags          (uint16 — bit 0 = traversalApply)
10       6    reserved
16       8    nextPtr        (VirtualPtr — next mapping entry)
24       8    reserved
```

**Resolution is single-hop.** Mappings must not chain. Commit adds a mapping
with `traversalApply = true`. Soft-delete adds a mapping with `traversalApply
= false`. GC drops old mappings and compacts the chain.

**Query resolution:** `mapper.resolve(R)` returns `toEpoch` if a mapping
exists for `R`, or `R` itself if no mapping exists. This ALWAYS applies.

**Traversal remapping:** the `traversalApply` flag controls how entries
with `epoch == fromEpoch` are treated during chain walking (§7.2).
- `traversalApply = true` (commit): entries with `epoch == S` are
  reinterpreted as `epoch == E`, making the snapshot's data visible to
  live-head readers as if it had been written directly at epoch E.
- `traversalApply = false` (soft-delete): entries with `epoch == S` are NOT
  remapped. The default read rules apply — odd epochs are skipped per §7.2
  step 6, making the snapshot's changes invisible. Querying epoch S returns
  the pre-snapshot base (highest even < S).

---

## 9. Concurrency

### 9.1 Version Chain Head CAS

Section page slots are 8-byte aligned `long` values. Updates use native
64-bit CAS (`atomic_compare_exchange_strong`):

```
long oldVal, newVal;
do {
    oldVal = atomic_load_acquire(ref PageNode.versionRootPtr);
    // Build version node in pool slot with next = oldVal (current chain head)
    UpdateVersionNode(newPoolSlot, next: oldVal);
    newVal = PackVirtualPtr(newPoolPage, newPoolSlot);
} while (atomic_compare_exchange_strong(ref PageNode.versionRootPtr, newVal, oldVal) != oldVal);
```

The `oldVal` read and version node construction MUST be inside the loop:
a concurrent CAS by another writer changes the chain head between attempts,
and the new version node's `next` field must point to the currently active
head at the moment of the successful CAS. The CAS is uncontested under
per-file serialization (§9.3) but is retained as a correctness invariant
and for future lock-free operation.

After a successful CAS on PageNode.versionRootPtr, the the pool page must be `Write`'d
to the dirty cache so the updated slot is persisted on flush.

Pool allocation (§5.2) uses a per-pool-page CAS on `freeCount`/`firstFreeSlot`.

### 9.2 Directory Node CAS

Directory and file node entry insertions CAS on the node's `headPtr`
(VirtualPtr, 8 bytes). The `oldHead` read and `newEntry.nextPtr`
assignment MUST be inside the retry loop:

```
VirtualPtr oldHead, newEntryPtr;
do {
    oldHead = atomic_load_acquire(ref headPtr);
    // newEntry already allocated in pool; set its nextPtr = oldHead
    newEntry.nextPtr = oldHead;
    newEntryPtr = PackVirtualPtr(newEntryPage, newEntrySlot);
} while (CAS(ref headPtr, newEntryPtr, oldHead) != oldHead);
```

CAS retries are rare — contention is only on creates/deletes in the same
directory. Data writes go through VersionPage CAS (§9.1) on different
section chains, with no contention between different files.

### 9.2.1 Pool Slot Publication Fences

When a writer allocates a new pool slot and links it into a chain (e.g.,
prepending a VersionPage to a PageNode.versionRootPtr), the writer must
guarantee publication ordering:

1. Allocate the pool slot and write all fields into the slot buffer.
2. Emit a **release memory barrier** before CAS-linking the slot into the
   chain via the parent's VirtualPtr field.

The lock-free reader uses **acquire loads** (`atomic_load_acquire`) when
following a VirtualPtr chain. This ensures all fields in the newly linked
slot are visible before the reader dereferences them.

### 9.3 Write Serialization (Per-File Locking)

Two lock modes: **global** (epoch 0) serializes all writes to a file across
all epochs; **per-epoch** (specific epoch) serializes only same-epoch writes,
allowing cross-epoch concurrency. Internal writes acquire the per-epoch lock
for the write's epoch automatically.

**Lock acquisition:**
```
mutex_t* lock = file_locks_get_or_create(nodeId, epoch);
mutex_lock(lock);
// ... perform writes ...
mutex_unlock(lock);
```

**Lock compatibility rules:**

- Per-epoch locks with different epochs are compatible — they do not block
  each other. Writers at epoch 1 and epoch 2 to the same file proceed
  concurrently.
- The global lock (epoch 0) is incompatible with all other locks on the
  same file — it serializes everything.
- A global lock request waits for all active per-epoch locks on that file
  to drain before proceeding. New per-epoch lock requests block while a
  global lock is held or pending.

**Implementation sketch:**
```
struct file_lock_state {
    int epoch_lock_count;     // active per-epoch locks
    bool global_held;          // global lock is held
    bool global_pending;       // global lock is waiting to acquire
};

global_lock_acquire:
    global_pending = true;
    while (epoch_lock_count > 0) spin/yield;
    global_held = true;
    global_pending = false;

epoch_lock_acquire(epoch):
    if (global_held || global_pending) block;
    epoch_lock_count++;
```

The lock is held for the duration of a single `WriteFile` call — typically
tens of microseconds — so contention is negligible. Deadlock is impossible
because each lock protects one file; no operation acquires multiple file
locks simultaneously.

### 9.4 Snapshot

Snapshot (`epoch += 2`) increments the epoch counter. **Before any write
at the new epoch proceeds, `currentEpoch` MUST be persisted to the superblock.**
The superblock page (a single page with lazy mirror) is written and fsync'd
directly — this is a targeted superblock flush, not a full dirty-cache flush.
Write the superblock bytes to the inactive physical half via Write,
then Flush only that page range. One fsync per snapshot. If the process
crashes before the flush, the on-disk superblock retains the old epoch.
On remount, the system re-uses that epoch — any previously written snapshot
data is unreachable via the old superblock pointer, but no corruption occurs
because the new snapshot data was never committed to disk.

### 9.5 Conflict Detection (Commit)

Commit scans version chains of all files modified in the snapshot epoch.
This is a read-only scan of the snapshot's dirty set — a `HashSet<long>`
of modified FileNode pool entries per epoch, maintained in-memory. On crash
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
   - **Version chains:** walk PageNode entries within each FileContent segment.
     Drop VersionPage entries belonging to soft-deleted epochs (per epoch mapper
     with `traversalApply = false`). Collapse committed mappings: if a
     version's epoch equals a committed snapshot epoch S, adjust to the
     live head epoch S+1. Build new version chains with surviving nodes,
     allocated sequentially into new pool pages.
   - **Directory entries:** scan DirContent chains per directory. Drop entries
     where the entry's epoch belongs to a deleted epoch AND no surviving
     entry for the same `childNodeId` exists. For deletions (`namePtr = 0`)
     in deleted epochs, drop the entry.
   - **FileSize:** drop entries from soft-deleted epochs (file falls back to
     baseline). Retain and rewrite entries from committed epochs. Drop
     FileContent segments beyond the highest surviving FileSize bound.
   - **Epoch mapper:** rebuild from surviving epochs. Drop mappings for
     deleted epochs. Collapse committed mappings. Write new mapper entries
     into new pool pages.
3. **Write new tree pages** — pool pages and data pages. Old pages are NOT
   freed until the swap is complete.
4. **Write new superblock** with the new root offset, `currentEpoch`,
   and new `poolListHead`. fsync.
5. **Atomically swap** the superblock pointer (lazy mirror (§3.7)).
6. **Release tree lock.**
7. **Free old pages.** Walk the old tree, free all pages not reachable from
   the new root. Every logical page freed releases its underlying lazy mirror
   physical pair internally. This phase runs in the background and does not
   block normal operations.

### 10.4 Crash During GC

If a crash occurs before step 6, the old superblock and old tree remain
intact. Recovery mounts the old state. The partially-written new tree pages
are unreachable and will be reclaimed on the next GC cycle. No corruption
possible.

### 10.5 Page Pinning

Direct page access (external API, pins pages for the duration of a direct I/O operation) pins pages. Pin count
is tracked in an in-memory `thread-safe hash table<long, int>` keyed by page
index. Pinned pages are skipped during step 7 freeing.

### 10.6 History Removal

GC is a destructive operation from the perspective of historical snapshots.
After GC completes:

- **Soft-deleted snapshots:** all version nodes, directory entries, and data
  pages belonging exclusively to that epoch are permanently removed.
  Reading the deleted epoch returns the base state (what existed before the
  snapshot was taken).
- **Committed snapshots:** the mapping is collapsed. Version nodes from the
  snapshot epoch are relabeled to the live head epoch at commit time.
  Original live-head pages overwritten by snapshot variants are freed.
  Reading the committed epoch returns the live head state as it existed at
  commit time — not subsequent live head changes.

Callers must ensure no active readers or queries depend on deleted or
committed snapshot epochs before invoking GC.

---

## 11. Crash Consistency

### 11.1 Page Durability

All pages — superblock, bitmap, FileNode/DirNode pool entries, pool pages, mapper pages, and
data pages — are backed by lazy mirror pairs §3.7. A crash
mid-write leaves the old half intact (generation not updated) or the new
half intact (CRC32C valid — generation was written last). No torn page is
ever visible. The reader picks the half with higher generation; if its CRC
fails, the backup half is used.

This eliminates the need for separate "torn-CAS recovery" for metadata
pages. The CAS-based updates to directory/file node `headPtr` and PageNode
slot arrays are crash-safe because the underlying page write is ping-pong
protected. A CAS that succeeds logically but whose page write is torn on
disk is recovered by the lazy mirror mechanism on the next read: the backup
half still holds the pre-CAS state, and the reader sees either the old or
new logical state — never a torn page.

### 11.2 Mount (No Recovery Needed)

1. Read both superblock pages (logical page 3 and its lazy mirror sibling).
2. Select the active one: higher PageHeader generation wins (§3.7 — Lazy Mirror).
   Validate its CRC32C. If invalid, try the other page.
3. Read `rootNodeOffset`, `currentEpoch`, `epochMapperPtr`, `poolListHead`.
4. Read the first epoch mapper page. Validate CRC32C. Walk the chain via
   `nextMapperPage`. Rebuild in-memory mapper dictionary.
5. Zero `treeLockState` reader count (in-memory only — the on-disk field
   is read only to check bit 63 for GC interruption; its reader count is
   always stale). If bit 63 is set, GC was interrupted; use the alternate
   superblock per §9.6.
6. System is ready. Tree pages are validated **lazily** on first access:
   - Directory/file page: CRC32C on header. If invalid, the last CAS was
     torn — skip the torn entry, continue from headPtr.
   - Pool page: CRC32C on header. If invalid, all entries in that page are
     treated as unallocated. Logical pages whose chain head points into the
     corrupt page appear as never written. Chains whose head is elsewhere
     but pass through a node in the corrupt page lose history below the
     corrupt entry but retain the head.
   - Data page: lazy mirror handles CRC failure on read (§3.7). No mount-time
     scan needed.

All corruption is detected and handled at read time, not mount time.
Pages that are never read after a crash need no validation.


## 12. Filesystem API

The VFS exposes a POSIX-like interface to callers. All operations accept an
optional `epoch` parameter: passing 0 or -1 uses the current live head epoch.
Passing a specific epoch provides snapshot isolation for that point in time.

### 12.1 Instance

```
vfs_t*    vfs_open(const char* path);
void      vfs_close(vfs_t* vfs);
int       vfs_flush(vfs_t* vfs);
```

### 12.2 File Operations

```
int       vfs_create(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch);
int       vfs_delete(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch);
int       vfs_rename(vfs_t* vfs, int64_t src_parent, const char* src,
                     int64_t dst_parent, const char* dst, int64_t epoch);

int64_t   vfs_open_file(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch);
int       vfs_read(vfs_t* vfs, int64_t file, void* buf, int64_t offset,
                   int64_t count, int64_t epoch);
int       vfs_write(vfs_t* vfs, int64_t file, const void* data, int64_t offset,
                    int64_t count, int64_t epoch);
int64_t   vfs_file_size(vfs_t* vfs, int64_t file, int64_t epoch);
int64_t   vfs_file_mtime(vfs_t* vfs, int64_t file, int64_t epoch);
int64_t   vfs_file_ctime(vfs_t* vfs, int64_t file);

int       vfs_lock(vfs_t* vfs, int64_t file, int64_t epoch);
int       vfs_unlock(vfs_t* vfs, int64_t file, int64_t epoch);
```

- `vfs_lock`/`vfs_unlock`: explicit per-file, per-epoch locking. Passing
  `epoch = 0` acquires the **global** file lock — all writes to this file
  are serialized regardless of epoch (SQLite compatibility). Passing a
  specific epoch acquires the per-epoch lock — only same-epoch writes block;
  cross-epoch writes proceed concurrently. Internal write operations
  acquire the per-epoch lock automatically. Reads remain lock-free.

- `vfs_create`: returns the nodeId of the created file, or -1 on error.
- `vfs_open_file`: resolves a path to a file nodeId. Returns -1 if not found.
- `vfs_read`/`vfs_write`: return bytes transferred, or -1 on error. Short
  reads/writes are possible at file boundaries.
- `vfs_file_size`/`mtime`/`ctime`: stat-like queries at a given epoch.

### 12.3 Directory Operations

```
int       vfs_mkdir(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch);
int       vfs_rmdir(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch);

typedef struct { int64_t nodeId; char name[256]; bool isDir; } vfs_dirent_t;
int       vfs_readdir(vfs_t* vfs, int64_t dir, vfs_dirent_t* entries,
                      int max, int64_t epoch);
```

- `vfs_readdir`: fills `entries` with up to `max` directory entries.
  Returns the number written. At `epoch`, only non-deleted children visible
  at that epoch are returned. The caller iterates by calling repeatedly.

### 12.4 Snapshot & Commit

```
int64_t   vfs_snapshot(vfs_t* vfs);
int       vfs_commit(vfs_t* vfs, int64_t snapshot_epoch);
int       vfs_delete_snapshot(vfs_t* vfs, int64_t snapshot_epoch);
```

- `vfs_snapshot`: increments the epoch counter and returns the new snapshot
  epoch (always odd). Snapshots are always taken from the **current live
  head** — not from another snapshot. Multiple active snapshots may coexist
  as siblings (all children of the live head at different times), but they
  never form a nested chain.
- `vfs_commit`: commits a snapshot to the live head. Fails if the same page
  was modified in both the snapshot and live head since the snapshot was taken.
- `vfs_delete_snapshot`: soft-deletes a snapshot. GC later reclaims space.

### 12.5 Manual GC

```
int       vfs_gc(vfs_t* vfs);
```

Shadow-compacts the tree, removing soft-deleted epochs and freeing
unreachable pages. Blocks all other operations while running.

### 12.6 Error Handling

```
typedef enum {
    VFS_OK = 0,
    VFS_ERR_IO = -1,
    VFS_ERR_NOTFOUND = -2,
    VFS_ERR_EXISTS = -3,
    VFS_ERR_NOTDIR = -4,
    VFS_ERR_NOTEMPTY = -5,
    VFS_ERR_CONFLICT = -6,
    VFS_ERR_FULL = -7,
    VFS_ERR_NOMEM = -8,
} vfs_error_t;

vfs_error_t vfs_last_error(vfs_t* vfs);
const char* vfs_error_string(vfs_error_t err);
```

---

## 13. Performance Model

Estimated per-operation cost for a single page_size-byte page write on the hot path.
Timings assume warm cache — pages resident in the unified page cache
§3.6, no disk I/O:

| Operation | Cost |
|---|---|
| In-memory page array lookup + VirtualPtr decode | ~1 µs |
| Version chain walk (typical: 2 hops) | ~0.1 µs |
| Data page write (lazy mirror: a page to inactive half, generation flip, CRC32C) | ~40 µs |
| Pool slot allocation (per-page CAS) | ~0.1 µs |
| Version chain CAS | ~0.1 µs |
| **Total per page write** | **~42 µs** |

At ~9 page writes per individual statement, predicted throughput is ~2,600
operations/second. For batched writes where metadata is amortized across
multiple writes within a transaction, throughput scales linearly with batch size.

The in-memory page array reduces per-page lookup from a chain walk to a
single indexed read (O(1)). Version chains are maintained in descending
epoch order; the latest live-head version is always the first entry.

PageNode metadata overhead: 32 bytes per logical page per segment, or
~0.39% of file size (32 KB per 8 MB segment). For a 1 TB file system,
~4 GB of PageNode entries — acceptable for O(1) lookup performance.

Epoch counter is an `int64_t`. At 2 snapshots per second, overflow occurs in
~146 billion years — practically unbounded.
