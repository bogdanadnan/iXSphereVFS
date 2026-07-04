# Phase 13: In-Memory Lazy Tree

## Goal
Build a unified in-memory tree that mirrors the on-disk pool structure.
Every node (DirNode, FileNode) caches its children and metadata in memory.
On first access, the tree is populated by walking pool chains. On mutation,
both the pool page and the in-memory cache are updated. After mount, the
entire read path uses only the in-memory tree — zero `pool_resolve` calls.

## Principle
There is exactly ONE way to walk the tree. Every operation that needs
directory contents, file metadata, or node lookups uses the same shared
implementation. No operation has its own hand-rolled chain walk.

## Data Structure

```c
typedef struct VfsNode {
    uint32_t nodeId;
    int64_t  vp;           // VirtualPtr to the pool entry
    uint16_t type;         // NODE_TYPE_DIR or NODE_TYPE_FILE

    /* Directory children — populated lazily */
    struct {
        DirChild* children;   // dynamic array, sorted by name
        int       count;
        int       capacity;
        bool      loaded;     // false = not yet loaded from pool
    } dir;

    /* File metadata — loaded with the FileNode */
    struct {
        int64_t  sizePtr;     // VirtualPtr to first FileSize
        int64_t  createdAt;
        int64_t* pageArray;   // in-memory segment array (Phase 5)
    } file;
} VfsNode;

typedef struct {
    char*     name;
    uint32_t  childNodeId;
    int64_t   childVp;       // VirtualPtr to child node
    bool      isDir;
    uint32_t  epoch;         // highest epoch ≤ live head for this entry
    bool      deleted;       // namePtr == 0 at this epoch
} DirChild;
```

## Operations

### `vfs_node_get(nodeId) → VfsNode*`
Return the in-memory node. If not yet loaded, allocate and populate:
- For DirNode: walk DirContent chain, populate `dir.children[]`
  (deduplicated by childNodeId, highest epoch rules applied)
- For FileNode: read sizePtr, createdAt from pool entry
- Mark `loaded = true`

### `vfs_node_lookup(parent, name) → DirChild*`
Binary search `parent->dir.children[]` by name. If `parent->dir.loaded`
is false, lazy-load first. Returns NULL if not found.

### `vfs_node_list(parent, entries, max) → count`
Copy `parent->dir.children[]` (excluding deleted entries) into `entries[]`.

### `vfs_node_add(parent, name, childNodeId, childVp, isDir, epoch)`
Binary-search-insert into `parent->dir.children[]`. Also writes the
corresponding DirContent entry to the pool (CAS-prepend to headPtr).

### `vfs_node_remove(parent, name, epoch)`
Mark the child as `deleted = true`. Also CAS-prepends a tombstone
DirContent to the pool.

### `vfs_node_rename(parent, oldName, newName, epoch)`
Update the DirChild entry in-place. Also updates the pool.

### `vfs_node_invalidate(parent)`
Called after GC or when the pool chain is rebuilt. Sets `loaded = false`.
Next access lazy-loads fresh data.

## What Gets Replaced

| Current Code | Replaced By | Lines Removed |
|-------------|-------------|---------------|
| `vfs_create`: walk chain for name collision | `vfs_node_lookup` | ~20 |
| `vfs_delete`: walk chain for target | `vfs_node_lookup` | ~15 |
| `vfs_open_file`: walk chain for name match | `vfs_node_lookup` | ~20 |
| `vfs_readdir`: walk chain + dedup | `vfs_node_list` | ~25 |
| `vfs_rename`: walk chain ×2 (src+dst) | `vfs_node_lookup` ×2 | ~30 |
| `vfs_rmdir`: walk chain for child | `vfs_node_lookup` | ~10 |
| Epoch mapper chain walk | `MapperTable` (still separate, ~5 entries) | ~10 |
| **Total hand-rolled chain walks eliminated** | | **~130 lines** |

## Staging

### Stage A — Lazy Tree Core (no read-path changes yet)
- `VfsNode` struct, `vfs_node_get`, `vfs_node_lookup`, `vfs_node_list`, `vfs_node_add/remove/rename`
- MapperTable as a simple hash (max 10 entries, linear scan is fine)
- Tests: create node, add children, lookup, verify pool writes match

### Stage B — Refactor Read Path
- `vfs_read`: `vfs_node_get(file)` → use in-memory pageArray
- `vfs_open_file`: `vfs_node_lookup(parent, name)`
- `vfs_readdir`: `vfs_node_list(parent)`
- `vfs_file_size/mtime/ctime`: read from VfsNode.file fields
- `vfs_epoch_is_writable`: MapperTable lookup
- Zero `pool_resolve` calls in the read path

### Stage C — Refactor Write Path
- `vfs_create`: `vfs_node_add` (writes pool + updates cache)
- `vfs_delete`: `vfs_node_remove` (writes tombstone + marks deleted)
- `vfs_rename`: `vfs_node_rename` (in-place update + pool write)
- `vfs_write` (COW path): unchanged (already minimal pool access)

### Stage D — GC Integration
- `vfs_gc` rebuilds pool pages → invalidates all `VfsNode` entries → lazy reload on next access
