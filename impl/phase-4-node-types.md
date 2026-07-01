# Phase 4: Node Types

## Goal
Define all 32-byte pool entry layouts with serialization/deserialization
helpers. Every entry type must be storable in exactly one 32-byte pool slot
(except NameEntry which chains multiple slots). This phase produces no
runtime behavior — just data structures and read/write helpers.

## Non-Negotiable Constraints

- **Every entry fits in 32 bytes.** Fields must be packed to not exceed this.
  If a type needs more, it chains via VirtualPtr (like NameEntry).
- **No type field for most entries.** The chain position implies the type.
  Only DirNode and FileNode need an explicit `type` discriminator because they
  share a similar structure.
- **All multi-byte fields are little-endian.** Use `vfs_rd8/rd4/rd2` and
  `vfs_wr8/wr4/wr2` from Phase 1 for all serialization.
- **VirtualPtr is the only pointer type.** No raw C pointers in pool entries.
  Every reference to another entry uses VirtualPtr.
- **Zero-initialized pool slots are safe.** A freshly allocated slot is all
  zeros. The `nextPtr = 0` (null) and all other fields being zero must be a
  valid initial state for every type.

## File Organization

| File | Purpose |
|------|---------|
| `src/nodes.h` | Offset constants, struct typedefs for every node type |
| `src/nodes.c` | Read/write helpers (one set per type) |
| `test/test_nodes.c` | Allocate, write, read back, chain walk for each type |

---

## Workload 4.1 — DirNode

### Purpose
Represents a directory. Has a nodeId and a head pointer to a chain of DirContent
entries.

### Layout (32 bytes, 16 used, 16 reserved)
```
Offset  Size  Type        Name     Description
──────  ────  ───────     ────     ───────────
  0      2    uint16_t    type     0x01 = DirNode
  2      2    —           rsvd     Zero-filled
  4      4    uint32_t    nodeId   Globally unique, assigned from superblock.nextNodeId
  8      8    VirtualPtr  headPtr  First DirContent entry in chain, 0 if empty
 16     16    —           rsvd     Zero-filled, reserved for future use
```

### Write Helper
```c
void nodes_write_dirnode(uint8_t* slot, uint32_t nodeId, int64_t headPtr);
// Writes type=0x01, nodeId, headPtr at correct offsets
```

### Read Helper
```c
void nodes_read_dirnode(const uint8_t* slot, uint32_t* nodeId, int64_t* headPtr);
// Reads nodeId and headPtr from a slot already known to be a DirNode
```

### Acceptance
- [ ] Write DirNode with nodeId=5, headPtr=VFS_VPTR_MAKE(10,3) → read back
  nodeId=5, headPtr page=10 slot=3
- [ ] Fresh slot (all zeros): type=0x00 (not a valid DirNode — caller must
  check type before reading)
- [ ] Header field offsets: type at 0, nodeId at 4, headPtr at 8

---

## Workload 4.2 — FileNode

### Purpose
Represents a file. Same first 16 bytes as DirNode, plus sizePtr and createdAt.

### Layout (32 bytes, fully packed)
```
Offset  Size  Type        Name       Description
──────  ────  ───────     ────       ───────────
  0      2    uint16_t    type       0x03 = FileNode
  2      2    —           rsvd       Zero-filled
  4      4    uint32_t    nodeId     Globally unique
  8      8    VirtualPtr  headPtr    First FileContent entry, 0 if empty
 16      8    VirtualPtr  sizePtr    First FileSize entry, 0 if no writes yet
 24      8    int64_t     createdAt  Unix timestamp, set once at creation, immutable
```

### Write Helper
```c
void nodes_write_filenode(uint8_t* slot, uint32_t nodeId, int64_t headPtr,
                          int64_t sizePtr, int64_t createdAt);
```

### Read Helpers
```c
void nodes_read_filenode(const uint8_t* slot, uint32_t* nodeId,
                         int64_t* headPtr, int64_t* sizePtr, int64_t* createdAt);
int64_t nodes_read_filenode_ctime(const uint8_t* slot); // reads only createdAt
```

### Acceptance
- [ ] Write FileNode with all fields → read back all fields match
- [ ] `createdAt` is a valid Unix timestamp (within ±60 seconds of `time(NULL)`)
- [ ] `nodeId` assigned sequentially: two FileNodes created in order differ by 1

---

## Workload 4.3 — DirContent

### Purpose
A single directory listing entry. One per child per epoch that child was
created, renamed, or deleted.

### Layout (32 bytes, fully packed)
```
Offset  Size  Type        Name         Description
──────  ────  ───────     ────         ───────────
  0      4    uint32_t    childNodeId  NodeId of the child
  4      4    uint32_t    epoch        Epoch this entry was created
  8      8    VirtualPtr  childPtr     Points to child's DirNode or FileNode
 16      8    VirtualPtr  namePtr      First NameEntry slot; 0 = tombstone (deleted)
 24      8    VirtualPtr  nextPtr      Next DirContent in chain, 0 = end
```

### Key Semantics
- `namePtr = 0` means the child was DELETED in this epoch. It is excluded from
  directory listings.
- Same-epoch rename: update `namePtr` in-place to point to a new NameEntry.
  This is an 8-byte atomic store at offset 16 (release semantics).
- Cross-epoch rename: create new DirContent at destination, tombstone at source.
- CAS-prepend to parent's `headPtr`: set `nextPtr = current_head`, CAS headPtr.

### Helpers
```c
void nodes_write_dircontent(uint8_t* slot, uint32_t childNodeId, uint32_t epoch,
                             int64_t childPtr, int64_t namePtr, int64_t nextPtr);
void nodes_read_dircontent(const uint8_t* slot, uint32_t* childNodeId,
                            uint32_t* epoch, int64_t* childPtr,
                            int64_t* namePtr, int64_t* nextPtr);
```

### Acceptance
- [ ] Create DirContent for new file: childNodeId matches, namePtr non-zero
- [ ] Tombstone DirContent (namePtr=0): listing excludes it
- [ ] Chain: prepend 3 DirContents, walk chain via nextPtr, verify descending epoch

---

## Workload 4.4 — FileContent

### Purpose
Groups logical pages into segments. One FileContent per 1,024 pages. Not
epoch-keyed — segments are permanent.

### Layout (32 bytes, 16 used, 16 reserved)
```
Offset  Size  Type        Name         Description
──────  ────  ───────     ────         ───────────
  0      8    VirtualPtr  pageRootPtr  First PageNode in this segment
  8      8    VirtualPtr  nextPtr      Next FileContent (higher page range), 0 = end
 16     16    —           rsvd         Zero-filled
```

### Key Semantics
- `pageRootPtr` points to the first PageNode for this segment's page range.
- `nextPtr` chains to the next segment (higher logical pages).
- Segments are permanent — never epoch-keyed. A reader at any epoch walks all
  segments, but future pages resolve as "never written" via version chain filtering.
- On first access to a segment, the VFS walks the PageNode chain and builds an
  in-memory array for O(1) lookups (Phase 5).

### Helpers
```c
void nodes_write_filecontent(uint8_t* slot, int64_t pageRootPtr, int64_t nextPtr);
void nodes_read_filecontent(const uint8_t* slot, int64_t* pageRootPtr, int64_t* nextPtr);
```

### Acceptance
- [ ] Write FileContent → read back pageRootPtr and nextPtr match
- [ ] Chain: two FileContents with nextPtr linking correctly

---

## Workload 4.5 — PageNode

### Purpose
Maps a single logical page to its version chain head.

### Layout (32 bytes, 16 used, 16 reserved)
```
Offset  Size  Type        Name           Description
──────  ────  ───────     ────           ───────────
  0      8    VirtualPtr  versionRootPtr First VersionPage in chain, 0 if never written
  8      8    VirtualPtr  nextPtr        Next PageNode in same segment, 0 = end
 16     16    —           rsvd           Zero-filled
```

### Key Semantics
- Within a segment, 1,024 PageNodes are linked via `nextPtr` in ascending
  logical page order. Even unwritten pages have a PageNode (with
  `versionRootPtr = 0`).
- `versionRootPtr` points to the most recent VersionPage (descending epoch).
  CAS-updated on first write in an epoch.

### Helpers
```c
void nodes_write_pagenode(uint8_t* slot, int64_t versionRootPtr, int64_t nextPtr);
void nodes_read_pagenode(const uint8_t* slot, int64_t* versionRootPtr, int64_t* nextPtr);
```

### Acceptance
- [ ] Write PageNode with versionRootPtr=0, nextPtr=link → read back correctly
- [ ] Chain of 3 PageNodes within a segment walks correctly

---

## Workload 4.6 — VersionPage

### Purpose
Records one version of one logical page at a specific epoch.

### Layout (32 bytes, 20 used, 12 reserved)
```
Offset  Size  Type        Name      Description
──────  ────  ───────     ────      ───────────
  0      4    uint32_t    epoch     Epoch this version was written
  4      4    —           rsvd      Zero-filled
  8      8    int64_t     dataPage  Logical page index of the data page
 16      8    VirtualPtr  nextPtr   Next older VersionPage, 0 = base version
 24      8    —           rsvd      Zero-filled
```

### Key Semantics
- No `dataCrc32c` field — integrity is handled by the data page's own PageHeader
  CRC32C + lazy mirror.
- Chain is descending epoch order. The live-head version is first.
- On first write to a page in an epoch: allocate data page, allocate VersionPage,
  CAS-prepend to `PageNode.versionRootPtr`.
- On subsequent writes to same page in same epoch: in-place write to existing
  data page. No new VersionPage.

### Helpers
```c
void nodes_write_versionpage(uint8_t* slot, uint32_t epoch, int64_t dataPage,
                              int64_t nextPtr);
void nodes_read_versionpage(const uint8_t* slot, uint32_t* epoch,
                             int64_t* dataPage, int64_t* nextPtr);
```

### Acceptance
- [ ] Write VersionPage {epoch=3, dataPage=100, nextPtr=ptr} → read back all match
- [ ] Chain: {epoch=5, next→epoch3} → walk yields 5 then 3

---

## Workload 4.7 — FileSize

### Purpose
Records file size and modification time at a given epoch.

### Layout (32 bytes, 24 used, 8 reserved)
```
Offset  Size  Type        Name        Description
──────  ────  ───────     ────        ───────────
  0      4    uint32_t    epoch       Epoch this size was recorded
  4      8    int64_t     modifiedAt  Unix timestamp of this change
 12      8    int64_t     fileSize    File size in bytes
 20      8    VirtualPtr  nextPtr     Next FileSize entry in chain, 0 = end
 28      4    —           rsvd        Zero-filled
```

### Key Semantics
- Chained from `FileNode.sizePtr`. Epoch-keyed, descending epoch order.
- On each write that extends the file, CAS-prepend a new FileSize entry.
- `stat()` walks the chain via the read rule to find the size at a given epoch.

### Helpers
```c
void nodes_write_filesize(uint8_t* slot, uint32_t epoch, int64_t modifiedAt,
                           int64_t fileSize, int64_t nextPtr);
void nodes_read_filesize(const uint8_t* slot, uint32_t* epoch,
                          int64_t* modifiedAt, int64_t* fileSize, int64_t* nextPtr);
```

### Acceptance
- [ ] Write FileSize {epoch=2, modifiedAt=now, fileSize=500, nextPtr=0} → read
  back all match
- [ ] Chain of 2 FileSize entries: queries at different epochs return correct sizes

---

## Workload 4.8 — NameEntry

### Purpose
Stores a file or directory name as UTF-8 bytes. Chains multiple slots for
names longer than 24 bytes.

### Layout per Slot (32 bytes)
```
Offset  Size  Type        Name     Description
──────  ────  ───────     ────     ───────────
  0     24    uint8_t[24] data     UTF-8 name bytes, zero-padded if shorter than 24
 24      8    VirtualPtr  nextPtr  Next NameEntry slot, 0 = last in chain
```

### Key Semantics
- DirContent.`namePtr` points to the first NameEntry slot.
- A name of length N occupies `ceil(N / 24)` slots.
- Reconstruct the full name by walking the chain and concatenating `data` bytes,
  stopping at the first zero byte in any slot (or after 24 bytes if the slot's
  nextPtr is 0).

### Helpers
```c
// Returns number of slots written (1 or more)
int  nodes_write_name(Pool* pool, const char* utf8_name, int64_t* first_slot_vp);
// Reconstructs name into caller-provided buffer. Returns name length.
int  nodes_read_name(Pool* pool, int64_t first_slot_vp, char* out_buf, int max_len);
```

### Acceptance
- [ ] Short name (5 bytes) → one slot, `nextPtr = 0`, name reconstructed correctly
- [ ] 50-byte name → three slots (24+24+2), chain linked correctly, concatenated name matches
- [ ] Name with embedded null → handled correctly (unlikely for filesystem names,
  but the mechanism should not crash)

---

## Workload 4.9 — TouchedFile

### Purpose
Tracks that a specific file was modified in a specific snapshot epoch. Used by
commit for conflict detection.

### Layout (32 bytes, 16 used, 16 reserved)
```
Offset  Size  Type        Name     Description
──────  ────  ───────     ────     ───────────
  0      4    uint32_t    epoch    Snapshot epoch
  4      4    uint32_t    nodeId   Modified file's nodeId
  8      8    VirtualPtr  nextPtr  Next TouchedFile entry, 0 = end
 16     16    —           rsvd     Zero-filled
```

### Key Semantics
- One entry per (file, epoch). Deduplicated: before CAS-prepending, walk the
  chain to check if an entry for this (epoch, nodeId) already exists.
- Chained from `superblock.touchedFilesPtr`. All active snapshot epochs share
  one chain, filtered by `epoch` during commit.

### Helpers
```c
void nodes_write_touchedfile(uint8_t* slot, uint32_t epoch, uint32_t nodeId,
                              int64_t nextPtr);
void nodes_read_touchedfile(const uint8_t* slot, uint32_t* epoch,
                             uint32_t* nodeId, int64_t* nextPtr);
```

### Acceptance
- [ ] Write TouchedFile {epoch=3, nodeId=7} → read back correctly
- [ ] Two TouchedFile entries for same (epoch, nodeId) → only one created (dedup)

---

## Workload 4.10 — MapperEntry

### Purpose
Stores an epoch mapping for commit/soft-delete resolution.

### Layout (32 bytes, 16 used, 16 reserved)
```
Offset  Size  Type        Name      Description
──────  ────  ───────     ────      ───────────
  0      4    uint32_t    fromEpoch Source epoch
  4      4    uint32_t    toEpoch   Target epoch
  8      2    uint16_t    flags     Bit 0 = traversalApply
 10      6    —           rsvd      Zero-filled
 16      8    VirtualPtr  nextPtr   Next MapperEntry, 0 = end
 24      8    —           rsvd      Zero-filled
```

### Helpers
```c
void nodes_write_mapperentry(uint8_t* slot, uint32_t fromEpoch, uint32_t toEpoch,
                              uint16_t flags, int64_t nextPtr);
void nodes_read_mapperentry(const uint8_t* slot, uint32_t* fromEpoch,
                             uint32_t* toEpoch, uint16_t* flags, int64_t* nextPtr);
```

### Acceptance
- [ ] Commit mapping: {from=1, to=2, traversalApply=true} → read back correct
- [ ] Soft-delete mapping: {from=3, to=2, traversalApply=false} → read back correct

---

## Final Phase 4 Checklist

- [ ] Every node type can be allocated, written, and read back with all fields
  preserved
- [ ] DirContent chain: prepend 3, walk via nextPtr, verify descending epoch
- [ ] NameEntry chain: 50-byte name → 3 slots → reconstruction matches
- [ ] FileSize chain: 2 entries → query at different epochs → correct sizes
- [ ] TouchedFile dedup: second write for same (epoch, nodeId) creates no duplicate
- [ ] MapperEntry: all three flag states writable and readable
- [ ] Fresh zero-filled slot → safe to read as any type (all zeros is valid
  initial state for every type: nextPtr=0, etc.)

---

## Dependencies

| Dependency | Phase | Used For |
|------------|-------|----------|
| `pool_alloc` | Phase 3 | Allocating pool slots for node type tests |
| `pool_resolve` | Phase 3 | Reading back written node data |
| `VFS_VPTR_MAKE/PAGE/SLOT` | Phase 3 | VirtualPtr encoding in all helpers |
| `vfs_rd2/rd4/rd8/wr2/wr4/wr8` | Phase 1 | Serializing all fields |

Phase 4 has no dependencies on Phase 5 (Tree Operations) or Phase 6 (Epoch System).
All 10 workloads can be developed and tested independently given a working Pool.

## Staging Guidance

All workloads are independent — they are pure data structure definitions.
Build order is any, but recommended:

1. 4.1 + 4.2 (DirNode, FileNode) — simplest, only 2 fields each
2. 4.3 to 4.6 (DirContent, FileContent, PageNode, VersionPage) — the core chain types
3. 4.7 + 4.8 (FileSize, NameEntry) — file metadata
4. 4.9 + 4.10 (TouchedFile, MapperEntry) — epoch system types

**Note on Workload 4.8 (NameEntry):** The write helper `nodes_write_name` takes
a `Pool*` and calls `pool_alloc` to allocate chained slots for long names.
This is the only workload that depends on a fully functional Pool (Phase 3).
All other workloads write into caller-provided slots — they only need Phase 1
serialization helpers.

**Note on nodeId assignment:** DirNode and FileNode have a `nodeId` field but
Phase 4 does NOT assign it. The `nodeId` is passed as a parameter to the write
helper. The caller (Phase 5) is responsible for atomically incrementing
`superblock.nextNodeId` and passing the assigned value.

## Phase 3 Debt Check

| Item | Status |
|------|--------|
| VirtualPtr unsigned cast (Iteration 1a #4) | ✅ Acceptable |
| Slot cap at 255 (Iteration 1b #A) | ✅ Raised to 65535 |
| `nextFreeSlot` in CAS retry (Iteration 1a #7) | ✅ Fixed |
| Debug assert in pool_state_pack (Iteration 1a #5) | ✅ Added |
| Pinning race fix (Iteration 2b) | ✅ Fixed |
| No blocking debt carried into Phase 4 | ✅ |

## Gaps Noted

| # | Gap | Severity | Resolution |
|---|-----|----------|------------|
| 1 | No explicit dependency list | Low | Added above |
| 2 | `nodeId` assignment not clarified as caller's responsibility | Low | Noted above |
| 3 | Zero-length name overlaps with tombstone sentinel (`namePtr=0`) | Low | Document: zero-length names are invalid; caller must ensure `namePtr != 0` for live entries |
| 4 | No staging/build order guidance | Low | Added above |
