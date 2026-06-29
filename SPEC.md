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
  255 usable entries + 1 chain link per 8KB page. Predictable allocation,
  simple free list.
- **Epoch-keyed.** Even epochs = live head, odd epochs = snapshots.
- **O(1) snapshots.** Increment a counter. No page writes.
- **Crash-safe.** Lazy mirror pages (§3) + pool CAS. Mount is O(1).
- **Conditional COW.** First write per epoch = copy; same-epoch = in-place.
- **In-memory arrays.** Section/page chains are loaded into arrays on first
  access for O(1) subsequent lookups.

---

## 2. Shared Infrastructure

§2–§4 (Page Format, StorageBackend, Superblock) are unchanged from the
v1 specification (SPEC.v1.md). Key points:

- **PageHeader:** 16 bytes (§2.1). pageType + flags + checksum + generation + mirrorPage.
- **StorageBackend:** §3. Allocate/Acquire/Read/Write/Flush, unified page cache,
  lazy mirror pages (§3.7), 32-byte config header, bitmap_dir at offset 32.
- **Superblock:** §4. At logical page 2. Holds rootNodeOffset, currentEpoch,
  epochMapperPage, poolListHead, treeLockState. Lazy mirror backed.

All references to StorageBackend APIs and page layout in this document use the
definitions from SPEC.v1.md §2–§4.

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

- Bits 0–7: slot index within the pool page (0–254; 255 is the link slot)
- Bits 8–63: logical page index of the pool page

`VirtualPtr` of 0 means null.

### 5.2 Pool Page Layout

```
Offset  Size  Field
──────  ────  ─────
 0       8    nextPoolPage   (int64 — next pool page in list; write-once)
 8       4    poolState      (uint32 — packed freeCount|firstFreeSlot)
12       4    reserved
16    8160    slots[255]     (255 × 32 bytes entries)
8176     16    link           (MetaPoolLink: next page index + reserved)
```

The last 16 bytes are the MetaPoolLink:
```
Offset  Size  Field
──────  ────  ─────
 0       8    nextMetaPage   (int64 — next pool page in chain, 0 = end)
 8       8    reserved
```

Total: 8192 bytes. 255 usable metadata entries, 1 link entry.

### 5.3 Pool Allocation

Same algorithm as v1 §5.4: CAS on `poolState` to allocate a free slot. When
`freeCount == 0`, allocate a new pool page and prepend to the global list via
`poolListHead` CAS. Individual slots are never freed — GC rebuilds pool pages
from scratch.

### 5.4 Name Entries

Names are stored in separate pool entries, padded to a multiple of 32 bytes.
A name of length N occupies `ceil((N + 1) / 32) * 32` bytes as one or more
contiguous pool slots. The name is UTF-8, zero-padded. A `VirtualPtr` in a
`DirContent` references the first slot of the name.

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
16      16    reserved
```

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
N pages. At page_size=8192, a segment size of 1024 pages (~8 MB) is
recommended. `pageRootPtr` points to the first `PageNode` for this segment.

`FileContent` entries are NOT epoch-keyed — they form a permanent linked list
of segments that grows as the file expands. The `nextPtr` links to the next
FileContent (higher logical page range). On first access, the VFS walks this
chain and builds an in-memory array for O(1) lookups.

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

## 7. Tree Traversal

### 7.1 Read Rule

To resolve a query at epoch R:
1. Apply epoch mapper (§8): `R' = mapper.resolve(R)`.
2. Walk the chain from the head pointer.
3. First entry with `epoch ≤ R'` AND `epoch` is even → use it.
4. If `epoch` is odd → skip, continue. If `epoch > R'` → skip, continue.
   (Chains are descending by epoch, so the first valid match is the most
   recent committed state at or before R'.)

### 7.2 File Read

To read logical page P of file F at epoch E:
1. Resolve FileNode → walk FileContent chain → find the segment containing P.
2. Walk PageNode chain within the segment to the entry for page P.
3. Follow `versionRootPtr` chain, apply read rule for epoch E → get dataPage.
4. `Read(dataPage)` via StorageBackend.

After first access to a segment, the FileContent → PageNode chain is cached
in an in-memory array. Subsequent reads are `array[P_in_segment] → versionRootPtr`.

### 7.3 File Write

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

### 7.4 Directory Operations

- **Create:** allocate DirNode or FileNode, create DirContent in parent with
  `namePtr` pointing to a new NameEntry, `childPtr` to the new node.
  CAS-prepend to parent's `headPtr`.
- **Delete:** create DirContent with `namePtr = 0`, `childNodeId` matching
  the target. CAS-prepend to parent.
- **Rename (same dir, same epoch):** update `namePtr` in-place on the existing
  DirContent entry to point to a new NameEntry.
- **Rename (new epoch or cross-dir):** create new DirContent at destination
  with `childPtr` and `childNodeId` from source. Create tombstone (namePtr=0)
  at source.

---

## 8. Epoch Mapping

Epoch mappings are stored in a separate pool chain with its own head pointer
in the superblock (`epochMapperPage`). Each mapping is a pool entry:

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

---

## 9. Concurrency

### 9.1 Pool Slot CAS

Section slot CAS uses native 64-bit CAS (§7.1 of v1). Same retry loop.

### 9.2 In-Memory Array Access

After initial load (under file lock), the in-memory FileContent → PageNode
array is accessed lock-free. Writes CAS the `versionRootPtr` on the PageNode
pool entry; no lock on the array itself.

### 9.3 File Locking

Per-file write serialization (§9.3 of v1). Same mechanism — lock per file
node before modifying version chains or directory entries for that file.

### 9.4 Snapshot

`epoch += 2`. Atomic increment. No page writes.

### 9.5 Tree Lock (GC)

Same as v1 §9.6.

---

## 10. Garbage Collection

Shadow compaction (same as v1 §10). GC walks the tree, copies surviving
entries into new pool pages sequentially, drops entries from deleted epochs,
collapses committed mappings, and atomically swaps the superblock.

---

## 11. Crash Consistency

Lazy mirror pages (§3.7 of v1) handle crash safety for all pages including
pool pages. Mount is O(1) (§11.2 of v1).

---

## 12. Performance Model

Estimated per-operation cost for a single 8KB page write:

| Operation | Cost |
|---|---|
| In-memory array lookup (cached segment) | ~0.1 µs |
| Pool slot allocation | ~0.1 µs |
| VersionPage CAS | ~0.1 µs |
| Data page write (lazy mirror) | ~40 µs |
| **Total per page write** | **~40 µs** |

Version chains average ~2 entries for a live-head lookup. In-memory arrays
eliminate chain walks after first access to a segment. Segment granularity
(1024 pages) bounds initial load cost to ~128 pool reads (~13 KB) per 8 MB
of file accessed.
