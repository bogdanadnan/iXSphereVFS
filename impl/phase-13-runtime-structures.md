# Phase 13: In-Memory Runtime Structures (SPEC)

## Goal
Build in-memory access structures that mirror on-disk pool data but are
optimized for O(1) runtime lookups. Pool pages remain the source of truth
for persistence and crash recovery. Runtime structures are rebuilt from
pool pages on mount and kept in sync by mutating operations.

## Principle
Pool pages store data. Runtime structures access it. Every time a pool page
is modified (by create, write, delete, rename, snapshot, commit, GC), the
corresponding runtime structure must also be updated. A writer updates both
the on-disk pool page AND the runtime structure atomically. A reader only
reads the runtime structure.

## Non-Negotiable Constraints

- **Pool pages remain the sole source of truth.** Runtime structures are
  derivative. On mount, all structures are rebuilt from pool pages. If a
  runtime structure is corrupted, it is discarded and lazily rebuilt.
- **No double bookkeeping.** A mutating operation writes to the pool page
  (via `nodes_write_*`) AND updates the runtime structure. The two updates
  must be consistent — if the pool page write fails, the runtime update is
  rolled back.
- **Reads never touch pool pages.** After mount, all read operations
  (`vfs_read`, `vfs_file_size`, `vfs_readdir`, `vfs_open_file`, `vfs_epoch_is_writable`)
  use only runtime structures. Pool pages are accessed only during writes and GC.
- **Lazy building is acceptable.** Large structures (directory indices,
  segment arrays) can be built on first access and cached. Small structures
  (mapper table, file node table) are built eagerly at mount.

---

## Workload 13.1 — Mapper Table

### What
Replace `mapper_resolve` and `mapper_traversal_apply` chain walks with an
in-memory hash table `{fromEpoch → {toEpoch, traversalApply}}`.

### Current State
Every `vfs_read`, `vfs_write`, `vfs_epoch_is_writable` walks the mapper
chain via `pool_resolve` → `storage_read` → `cache_find`. For a 3-entry
chain, that's 3 hash lookups + 3 bucket locks per call.

### Proposed State
A hash table built at mount by walking the mapper chain once. `mapper_resolve`
is a table lookup — O(1). The table is updated on `mapper_insert` (commit,
soft-delete) and during GC (entries dropped).

### Data Structure
```c
typedef struct {
    uint32_t fromEpoch;
    uint32_t toEpoch;
    bool     traversalApply;
} MapperEntry;

typedef struct {
    MapperEntry* entries;    // dynamic array
    int          count;
    int          capacity;
    bool         dirty;      // needs rebuild from chain
} MapperTable;
```

### Operations
- `mapper_table_init`: walk on-disk chain, populate entries[]
- `mapper_table_resolve(epoch)`: linear scan or hash. Given max ~10 entries,
  linear scan is fine. O(N) where N ≤ 10.
- `mapper_table_insert(from, to, traversalApply)`: append to entries[].
  Also CAS-prepend to on-disk chain.
- `mapper_table_rebuild`: called at mount and after GC. Walks chain, rebuilds.

### Acceptance
- [ ] After mount: `mapper_table_resolve` returns same result as chain walk
- [ ] After commit: new mapping in table AND on disk
- [ ] After GC: entries for deleted/committed epochs removed from both
- [ ] `pool_resolve` never called from mapper functions in read path

---

## Workload 13.2 — File Node Table

### What
Replace `pool_resolve(FileNode)` lookups with an in-memory hash table
`{nodeId → VirtualPtr}`.

### Current State
Every `vfs_write`, `vfs_read`, `vfs_file_size`, `vfs_file_mtime` starts by
calling `pool_resolve(file_nodeId)` to find the FileNode's VirtualPtr.
But `vfs_create` already returns a VirtualPtr (mislabeled as nodeId).
The benchmark stores VirtualPtrs directly and avoids this lookup.

### Proposed State
A hash table mapping `nodeId → VirtualPtr` for all file and directory nodes.
Built at mount by walking the tree. Updated on create (insert new node),
delete (doesn't remove — the tombstone preserves history).

### Data Structure
```c
typedef struct {
    uint32_t nodeId;
    int64_t  vptr;        // VirtualPtr to DirNode or FileNode
} NodeEntry;

typedef struct {
    NodeEntry* entries;   // sorted by nodeId for binary search, or hash table
    int        count;
    int        capacity;
} NodeTable;
```

### Operations
- `node_table_lookup(nodeId)`: return VirtualPtr, or 0 if not found
- `node_table_insert(nodeId, vptr)`: called from `vfs_create` and `vfs_mkdir`
- `node_table_rebuild`: walk entire tree at mount, populate

### Acceptance
- [ ] `vfs_write` does not call `pool_resolve` for FileNode resolution
- [ ] `node_table_lookup(0)` returns root VirtualPtr
- [ ] After creating N files, table has N+1 entries (root + N files)
- [ ] Rebuilt correctly on mount

---

## Workload 13.3 — Directory Index

### What
Replace `DirContent` chain walks with an in-memory per-directory hash table
`{name → VirtualPtr}`.

### Current State
`vfs_create` walks the DirContent chain to check for name collisions (O(N)).
`vfs_open_file` walks the chain to find a child by name (O(N)).
`vfs_readdir` walks the chain + deduplicates by childNodeId (O(N²)).

### Proposed State
A per-directory hash table mapping `(name) → (childNodeId, childPtr, isDir)`.
Built lazily on first directory access. Updated on create, delete, rename.
Invalidated by GC.

### Data Structure
```c
typedef struct {
    char*    name;
    uint32_t childNodeId;
    int64_t  childPtr;      // VirtualPtr to child's DirNode or FileNode
    bool     isDir;
    uint32_t bestEpoch;     // highest epoch ≤ live head for dedup
    bool     isDeleted;     // true if namePtr == 0 at best epoch
} DirEntry;

typedef struct {
    DirEntry* entries;      // dynamic array
    int       count;
    int       capacity;
    int64_t   dirVp;        // VirtualPtr of the DirNode this index belongs to
    bool      valid;        // false = needs rebuild
} DirIndex;
```

### Operations
- `dir_index_build(dirVp)`: walk DirContent chain, insert surviving entries
  (same dedup rules as `vfs_readdir`). O(entries).
- `dir_index_lookup(dirVp, name)`: linear scan or hash. O(entries).
- `dir_index_add(dirVp, name, childNodeId, childPtr, isDir, epoch)`: called
  from `vfs_create`/`vfs_mkdir` after CAS-prepend to parent's headPtr.
- `dir_index_invalidate(dirVp)`: called from `vfs_delete`, `vfs_rename`,
  and GC. Marks the index as needing rebuild.
- `dir_index_readdir(dirVp, entries, max)`: fills `entries[]` from the
  in-memory index — no pool page access.

### Acceptance
- [ ] `vfs_create` checks `dir_index_lookup` for name collision instead of
  walking DirContent chain
- [ ] `vfs_open_file` resolves via `dir_index_lookup`
- [ ] `vfs_readdir` returns results from in-memory index (no chain walk)
- [ ] Index invalidated correctly after delete, rename
- [ ] Index rebuilt correctly on mount

---

## Workload 13.4 — Refactor vfs_read to Use Runtime Structures

### What
Rewrite `vfs_read` to use the mapper table, node table, and directory index
exclusively. No `pool_resolve` calls in the read path.

### Current Hot Path (vfs_read)
1. `pool_resolve(FileNode)` — resolves file nodeId to VirtualPtr
2. `tree_resolve_page` — walks FileContent chain, resolves PageNode
   (uses in-memory segment array after first access)
3. Walk version chain — `pool_resolve` per VersionPage
4. `storage_read(dataPage)` — reads the actual data

### Proposed Hot Path
1. `node_table_lookup(file)` → FileNode VirtualPtr — O(1)
2. `tree_resolve_page` — unchanged (already in-memory after first access)
3. Fast path: `mapper_table` is empty → no epochs to remap → first
   VersionPage IS the answer (already implemented)
4. `storage_read(dataPage)` — unchanged

### Acceptance
- [ ] `vfs_read` makes zero `pool_resolve` calls in the common case
  (live head, no mapper entries, warm cache)
- [ ] All existing tree tests pass
- [ ] Performance: ops/sec improves by >2× vs current baseline

---

## Workload 13.5 — Refactor vfs_write to Sync Runtime Structures

### What
After every pool page mutation in `vfs_write`, `vfs_create`, `vfs_delete`,
`vfs_rename`, update the corresponding runtime structure.

### Mutations and Runtime Updates

| Mutation | Pool Page Update | Runtime Update |
|----------|-----------------|----------------|
| `vfs_create` | FileNode, DirContent, NameEntry | `node_table_insert`, `dir_index_add` |
| `vfs_write` (new epoch VersionPage) | VersionPage, FileSize | `touchedfile_add` (already done) |
| `vfs_delete` | DirContent tombstone | `dir_index_invalidate` |
| `vfs_rename` (same-dir) | DirContent namePtr update | `dir_index_add` + remove old |
| `vfs_rename` (cross-dir) | DirContent ×2 | `dir_index_add` + `dir_index_invalidate` |
| `vfs_snapshot` | (none — in-memory) | `mapper_table_insert` (on commit) |
| `vfs_commit` | MapperEntry | `mapper_table_insert` |
| `vfs_delete_snapshot` | MapperEntry | `mapper_table_insert` |
| `vfs_gc` | Rebuild all pool pages | Rebuild ALL runtime structures |

### Acceptance
- [ ] After `vfs_create`, directory index immediately shows new file
- [ ] After `vfs_delete`, directory index shows file removed
- [ ] After `vfs_rename`, directory index shows both old and new locations
- [ ] All existing tree tests pass unchanged

---

## File Organization

| File | Purpose |
|------|---------|
| `src/mapper_table.c` | MapperTable — replaces mapper chain walks |
| `src/node_table.c` | NodeTable — nodeId → VirtualPtr |
| `src/dir_index.c` | DirIndex — per-directory hash table |
| (modified) `src/tree.c` | Refactored read/write paths |
| (modified) `src/epoch.c` | Mapper table integration |
| (modified) `src/gc.c` | Runtime structure rebuild on GC |

## Staging Guidance

### Stage A — Foundation (no read-path changes)
- 13.1: MapperTable — replaces chain walks in mapper functions
- 13.2: NodeTable — replaces FileNode pool_resolve calls

### Stage B — Directory Index
- 13.3: DirIndex — replaces DirContent chain walks in create/open/readdir

### Stage C — Read Path Optimization
- 13.4: Refactor `vfs_read` to use all three runtime structures

### Stage D — Write Path Consistency
- 13.5: Refactor write operations to keep runtime structures in sync
