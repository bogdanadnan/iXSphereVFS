# Phase 4: Node Types

## Goal
Define all 32-byte pool entry layouts that make up the VFS metadata: directory
nodes, file nodes, directory content entries, file content entries, page
nodes, version pages, file size entries, name entries, touched-file tracking
entries, and epoch mapper entries. Each type occupies exactly one 32-byte
pool slot (names may chain multiple slots).

---

## Workload 4.1 — DirNode and FileNode

**What:** The pool entries that represent directories and files. Both share
the same 32-byte layout with a type discriminator in the first two bytes.

**Why:** Every file and directory in the VFS is identified by a nodeId and
has a head pointer to its content chain. A single layout for both types
simplifies allocation and traversal; the type field distinguishes them.

**How:**
- Layout: bytes 0–1 are `type` (`uint16_t`, 0x01 for DirNode, 0x03 for
  FileNode). Bytes 2–3 are reserved. Bytes 4–7 are `nodeId` (`uint32_t`,
  globally unique, assigned sequentially from the superblock's `nextNodeId`
  counter). Bytes 8–15 are `headPtr` (VirtualPtr to the first DirContent
  for a directory, or the first FileContent for a file).
- FileNode adds at bytes 16–23 a `sizePtr` (VirtualPtr to the first FileSize
  entry, a separate chain), and at bytes 24–31 a `createdAt` timestamp
  (int64_t Unix epoch, set once at creation, immutable). DirNode leaves
  bytes 16–31 as reserved.
- The `headPtr` chain for a DirNode contains DirContent entries (one per
  child per epoch that child was created, renamed, or deleted). The chain
  is in descending epoch order — the newest entry is at the head.
- The `headPtr` chain for a FileNode contains FileContent entries (one per
  segment, not epoch-keyed).
- `nodeId` assignment: on creation of a new node, atomically increment
  `nextNodeId` in the superblock and use the old value. NodeId 0 is reserved
  for the root directory.
- Write helpers use `vfs_wr2`, `vfs_wr4`, `vfs_wr8` to serialize into a
  32-byte slot buffer. Read helpers use `vfs_rd2`, `vfs_rd4`, `vfs_rd8`.

**Acceptance:**
  - Allocate a DirNode: type=0x01, nodeId is a valid sequential number,
    headPtr=0 (null).
  - Allocate a FileNode: type=0x03, sizePtr=0, createdAt is the current
    Unix timestamp.
  - Two FileNodes created sequentially have nodeId values differing by 1.
  - Read back a DirNode from its pool slot: all fields match what was written.

---

## Workload 4.2 — DirContent

**What:** A directory listing entry. One DirContent exists per child per
epoch that the child was created, renamed, or deleted in that directory.

**Why:** Directory listing at a given epoch requires knowing the name and
target of each child visible at that epoch. Multiple DirContent entries
for the same child (across epochs) are deduplicated by `childNodeId` during
listing — only the entry with the highest epoch ≤ the query epoch is used.

**How:**
- Layout (fully packed 32 bytes): bytes 0–3 = `childNodeId` (uint32_t, the
  nodeId of the child being listed). Bytes 4–7 = `epoch` (uint32_t).
  Bytes 8–15 = `childPtr` (VirtualPtr to the child's DirNode or FileNode).
  Bytes 16–23 = `namePtr` (VirtualPtr to the first NameEntry slot, or
  0 if the child was deleted in this epoch). Bytes 24–31 = `nextPtr`
  (VirtualPtr to the next DirContent in the chain).
- `namePtr = 0` represents a deletion (tombstone). When listing a directory,
  entries with `namePtr = 0` are excluded. This is how file/directory
  deletion is represented — no separate tombstone type.
- Rename in the same directory at the same epoch: update `namePtr` in-place
  on the existing DirContent entry. This is an 8-byte atomic store at
  offset 16 (release semantics).
- Rename to a different directory or at a new epoch: create a new DirContent
  at the destination with the same `childNodeId` and `childPtr` as the
  source entry, and a new `namePtr`. Then create a deletion entry (namePtr=0)
  at the source.
- CAS-prepend to the parent DirNode's `headPtr`: allocate the pool slot,
  set `nextPtr = oldHead`, CAS `headPtr` to the new VirtualPtr.

**Acceptance:**
  - Create a DirContent for a new file: childNodeId matches the file's
    nodeId, namePtr is non-zero.
  - Delete a file: a new DirContent with the same childNodeId and namePtr=0
    is prepended. Listing the directory at the current epoch excludes that
    child.
  - Rename in-place: the existing DirContent's namePtr is updated; no new
    entry is created.
  - Listing at a historical epoch correctly deduplicates entries by childNodeId,
    picking the name from the highest epoch ≤ query.

---

## Workload 4.3 — FileContent and PageNode

**What:** FileContent groups logical pages into segments (1,024 pages each).
PageNode maps a single logical page to its version chain head. Together
they form the two-level index that replaces the old section-page array.

**Why:** The in-memory page array (Phase 5) needs to find the version chain
head for any logical page in O(1). FileContent provides the segment-level
index; PageNode provides the per-page mapping. Both are pool-allocated and
linked into chains.

**How:**
- FileContent layout (16 bytes used, 16 reserved): bytes 0–7 = `pageRootPtr`
  (VirtualPtr to the first PageNode in this segment). Bytes 8–15 = `nextPtr`
  (VirtualPtr to the next FileContent for a higher page range).
- FileContent entries are NOT epoch-keyed. They form a permanent chain that
  grows as the file expands. A reader at any epoch walks the full chain,
  including segments added in later epochs. This is safe because VersionPage
  epoch filtering makes pages in future segments resolve as "never written,"
  and FileSize entries bound which segments are logically valid.
- Segment size is fixed at 1,024 pages (~8 MB at 8,192-byte pages). This
  value is stored in the StorageBackend header (`segment_size` field at
  offset 16 of header page 0) and is read once at mount.
- PageNode layout (16 bytes used, 16 reserved): bytes 0–7 = `versionRootPtr`
  (VirtualPtr to the first VersionPage for this page, in descending epoch
  order). Bytes 8–15 = `nextPtr` (VirtualPtr to the next PageNode within
  the same segment).
- Within a segment, PageNodes are linked via `nextPtr` in ascending logical
  page order. A segment always has exactly 1,024 PageNodes — even for pages
  that have never been written. The `versionRootPtr` of an unwritten page
  is 0 (null).
- On first access to a segment, the VFS walks the PageNode chain and builds
  an in-memory array of VirtualPtrs (see Phase 5). Subsequent accesses are
  O(1) array lookups.
- When a file grows and a new FileContent is appended, the in-memory array
  is invalidated and rebuilt on the next access.

**Acceptance:**
  - Write to logical page 5 of a file: a FileContent covering pages 0–1023
    is created if it doesn't exist. A PageNode is allocated for page 5 with
    `versionRootPtr` pointing to the new VersionPage.
  - Write to logical page 2,000: a second FileContent covering pages 1024–2047
    is created and appended to the chain.
  - Reading page 5 at an older epoch (before it was written): walks the
    version chain, finds no matching epoch, returns "never written."
  - The StorageBackend header's `segment_size` field is read on mount and
    used to compute `segmentIndex = logicalPage / segment_size`.

---

## Workload 4.4 — VersionPage

**What:** Records one version of one logical page. Each VersionPage stores
the epoch, the physical data page index, and a link to the next older version.

**Why:** Version chains provide snapshot isolation. A reader at epoch E walks
the chain to find the VersionPage with the highest epoch ≤ E (applying the
read rule with epoch mapper). The chain is in descending epoch order, so the
live-head version is always the first entry.

**How:**
- Layout (20 bytes used, 12 reserved): bytes 0–3 = `epoch` (uint32_t).
  Bytes 4–7 reserved. Bytes 8–15 = `dataPage` (int64_t, logical page index
  of the physical data page). Bytes 16–23 = `nextPtr` (VirtualPtr to the
  next older VersionPage, 0 if this is the base version). Bytes 24–31 reserved.
- On first write to a logical page in a given epoch: allocate a new data
  page (via StorageBackend `Allocate(1)`), copy the base page content, overlay
  the new data, write the new page. Allocate a pool slot for the VersionPage.
  Set `nextPtr` to the current `versionRootPtr`. CAS `versionRootPtr` on the
  PageNode to the new VirtualPtr.
- On subsequent writes to the same page in the same epoch: the VersionPage
  for this epoch already exists. In-place write to the data page (via lazy
  mirror, Phase 2). No new pool slot is needed.
- The data page's integrity is covered by its own PageHeader CRC32C and the
  lazy mirror mechanism. The VersionPage does not duplicate a CRC field.
- No `dataCrc32c` field exists in VersionPage (removed from the v1 design
  as redundant).

**Acceptance:**
  - First write to page 0 at epoch 0: VersionPage {epoch=0, dataPage=X,
    nextPtr=0}. PageNode.versionRootPtr updated via CAS.
  - Second write to same page at epoch 2 (after snapshot): new VersionPage
    {epoch=2, dataPage=Y, nextPtr=vp_to_epoch0}. Chain is now epoch2→epoch0.
  - Write again to page 0 at epoch 2 (same epoch): no new VersionPage; the
    data page is overwritten in-place.
  - Read page 0 at epoch 1: walks chain, epoch2 > 1 (skip), epoch0 < 1 and
    even (use it). Returns data from epoch 0.

---

## Workload 4.5 — FileSize

**What:** Records the file size and modification timestamp at a given epoch.
Hangs off a dedicated chain from the FileNode's `sizePtr` field.

**Why:** File size must be queryable at any epoch for snapshot isolation.
Each size change creates a new FileSize entry. The read rule resolves the
current size and modification time.

**How:**
- Layout (20 bytes used, 12 reserved): bytes 0–3 = `epoch` (uint32_t).
  Bytes 4–11 = `modifiedAt` (int64_t Unix timestamp). Bytes 12–19 = `fileSize`
  (int64_t, file size in bytes). Bytes 20–27 = `nextPtr` (VirtualPtr to the
  next FileSize entry). Bytes 28–31 reserved.
- On `vfs_write` when the write extends beyond the current file size:
  allocate a new pool slot, CAS-prepend to FileNode.`sizePtr` with the new
  file size and the current Unix timestamp.
- On `vfs_file_size(file, epoch)`: walk the `sizePtr` chain via the read rule.
  Return the `fileSize` from the first matching entry. If no entry exists,
  the file has never been written (size 0).
- On `vfs_file_mtime(file, epoch)`: same chain walk, return `modifiedAt`.
- On `vfs_file_ctime(file)`: return the FileNode's `createdAt` field directly
  (immutable).

**Acceptance:**
  - Write 100 bytes at offset 0 to a new file: a FileSize entry with
    fileSize=100 is created. `vfs_file_size` returns 100.
  - Write 200 more bytes at offset 100: a new FileSize entry with fileSize=300
    is prepended. `vfs_file_size` at the current epoch returns 300.
  - `vfs_file_size` at an epoch before the second write returns 100.
  - `mtime` differs between the two FileSize entries; `ctime` is the same
    for both queries.

---

## Workload 4.6 — NameEntry

**What:** Stores a file or directory name as UTF-8 bytes. Names longer than
24 bytes chain multiple pool slots.

**Why:** Names are variable-length. Fixed-size slots can't hold arbitrarily
long names. Chaining slots avoids the need for contiguous multi-slot
allocation (which the free list can't provide).

**How:**
- Each NameEntry slot: bytes 0–23 = 24 bytes of UTF-8 name data (zero-padded
  if the name ends within those 24 bytes). Bytes 24–31 = `nextPtr` (VirtualPtr
  to the next NameEntry slot, 0 if this is the last slot in the chain).
- A name of length N occupies `ceil(N / 24)` slots. The DirContent's `namePtr`
  points to the first slot.
- Example: `"report.txt"` (10 bytes) → one slot. `"a very long filename
  that exceeds twenty four bytes.txt"` (54 bytes) → three slots (24+24+6).
- GC traverses the chain to identify all slots belonging to a name. When the
  pool page containing the slots is rebuilt during GC, the chain walk ensures
  all slots are accounted for.

**Acceptance:**
  - A short name (5 bytes) occupies one slot with `nextPtr = 0`.
  - A 50-byte name occupies three slots: the first two have `nextPtr` pointing
    to the next; the third has `nextPtr = 0`.
  - Reading a chained name reconstructs the full UTF-8 string.
  - GC correctly frees all slots in a name chain when the DirContent entry
    is dropped.

---

## Workload 4.7 — TouchedFile

**What:** A per-epoch tracking entry that records which file nodes were
modified in a given snapshot epoch. Used for commit conflict detection.

**Why:** Commit must scan all files modified in a snapshot to detect conflicts
with the live head. Without a persisted list, a crash would require a full
tree scan to find modified files. The TouchedFile chain makes commit O(modified),
not O(total).

**How:**
- Layout (16 bytes used, 16 reserved): bytes 0–3 = `epoch` (uint32_t, the
  snapshot epoch). Bytes 4–7 = `nodeId` (uint32_t, the file's nodeId).
  Bytes 8–15 = `nextPtr` (VirtualPtr to the next TouchedFile entry for this
  epoch). Bytes 16–31 reserved.
- When a VersionPage is first written for a file in a snapshot epoch:
  CAS-prepend a TouchedFile entry to the epoch's chain, rooted at the
  superblock's `touchedFilesPtr` for that epoch. If an entry for this
  (epoch, nodeId) already exists (the file was already written in this epoch),
  skip — only one entry per file per epoch.
- At commit time: walk the TouchedFile chain for epoch S. For each file,
  scan its version chains for conflicts with live-head epochs.
- After commit or soft-delete: the TouchedFile chain is dropped. GC reclaims
  the pool slots.

**Acceptance:**
  - Write to file A and file B in snapshot epoch 3: two TouchedFile entries
    exist in the chain for epoch 3.
  - Write to file A again in the same epoch: no duplicate entry.
  - Commit epoch 3: the TouchedFile chain is walked and all modified files
    are scanned for conflicts. After commit, the chain is dropped.

---

## Workload 4.8 — MapperEntry

**What:** Stores an epoch mapping: a source epoch, a target epoch, and a
`traversalApply` flag. Chained from the superblock's `epochMapperPtr`.

**Why:** Committed snapshots need their data remapped to the live head during
reads. Soft-deleted snapshots need their data hidden. The mapper provides
a single-hop lookup for both cases.

**How:**
- Layout (16 bytes used, 16 reserved): bytes 0–3 = `fromEpoch` (uint32_t).
  Bytes 4–7 = `toEpoch` (uint32_t). Bytes 8–9 = `flags` (uint16_t, bit 0 =
  `traversalApply`). Bytes 10–15 reserved. Bytes 16–23 = `nextPtr` (VirtualPtr
  to the next MapperEntry). Bytes 24–31 reserved.
- Commit epoch S: add entry `{fromEpoch=S, toEpoch=S+1, traversalApply=true}`.
- Soft-delete epoch S: add entry `{fromEpoch=S, toEpoch=S-1, traversalApply=false}`.
- The mapper resolves queries: `mapper.resolve(R)` walks the chain. If an
  entry with `fromEpoch == R` exists, returns `toEpoch`. Otherwise returns `R`.
- Traversal remapping: during chain walking (read rule), if an entry's epoch
  has a mapping with `traversalApply == true`, the entry's epoch is remapped
  before applying the standard read rule. If `traversalApply == false`, the
  entry is NOT remapped — its original epoch is used and the standard read
  rule handles it (odd epochs are skipped, which is the desired behavior for
  soft-deletes).
- Single-hop invariant: no `fromEpoch` or `toEpoch` may appear in another
  mapping. This is enforced at insert time (commit rejects if a chain would
  form; soft-delete is rejected or GC consolidates first).
- GC drops mapper entries for committed and soft-deleted epochs during
  compaction.

**Acceptance:**
  - `mapper.resolve(1)` with no mapping returns 1.
  - After commit of epoch 1→2: `mapper.resolve(1)` returns 2.
  - After soft-delete of epoch 3→2: `mapper.resolve(3)` returns 2.
  - During traversal of a chain entry with epoch 1 and a commit mapping:
    the entry is treated as epoch 2 and passes/fails the read rule accordingly.
  - During traversal of a chain entry with epoch 3 and a soft-delete mapping
    (traversalApply=false): the entry keeps epoch 3, which is odd, so the
    standard read rule skips it.

---

## Deliverables

| File | Purpose |
|------|---------|
| `src/nodes.h` | Struct definitions and field offsets for all 8 node types |
| `src/nodes.c` | Serialization/deserialization helpers for each type |
| `test/test_nodes.c` | Allocate each type, write fields, read back, verify |

## Success Criteria
- Every node type can be allocated via the pool, written to its slot, and
  read back with all fields preserved.
- DirContent chaining: prepend 3 entries, walk the chain, verify descending
  epoch order.
- NameEntry chaining: a 50-byte name occupies 3 slots and the full name is
  reconstructed by walking the chain.
- FileSize chain: prepend 2 entries, query at different epochs, get correct
  sizes.
- TouchedFile deduplication: writing to the same file twice in one epoch
  produces exactly one TouchedFile entry.
- MapperEntry single-hop: attempting to create a chain (fromEpoch or toEpoch
  already mapped) is rejected.
