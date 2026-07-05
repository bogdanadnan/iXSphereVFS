# Phase 8: Filesystem API

## Goal
Expose the full VFS functionality through a clean, POSIX-like C API. Every
function accepts an optional epoch parameter (-1 = current live head). The
API covers instance management, file and directory operations, locking,
snapshot lifecycle, and garbage collection.

## Non-Negotiable Constraints

- **All functions return predictable error codes.** Every failure path must
  set `vfs->last_error` before returning.
- **Thread safety.** The `vfs_t` handle is shared across threads. All
  internal synchronization uses the mechanisms from earlier phases. No
  additional global locks.
- **Epoch parameter semantics.** -1 means "current live head." Any other
  negative value is an error. Odd positive = snapshot. Even positive = may
  be a live-head epoch (validated by the Epoch System).
- **Node handles are int64_t.** They contain the nodeId in the lower 32 bits.
  Upper 32 bits are always 0. The `vfs_dirent_t.nodeId` is also int64_t.
- **Pthread only.** The codebase uses pthread for all threading (mutexes,
  condition variables, thread creation). C11 `<threads.h>` is not used.
  This is a deliberate choice: pthread is universally available; C11 threads
  are not supported on macOS and have inconsistent MSVC availability.

## File Organization

Phase 8 does not introduce a single `api.c` file. Instead, each function lives
in the module that owns its implementation:

| Function | Source File | Phase | Notes |
|----------|-------------|-------|-------|
| `vfs_mount`, `vfs_unmount` | `src/vfs.c` | 5+8 | Instance lifecycle |
| `vfs_flush` | `src/vfs.c` | 8 | **NEW — not yet implemented** |
| `vfs_last_error` | `src/vfs.c` | 8 | **NEW — not yet implemented** |
| `vfs_lock`, `vfs_unlock` | `src/vfs.c` | 8 | **NEW — not yet implemented** |
| `vfs_create`, `vfs_delete`, `vfs_mkdir`, `vfs_rmdir`, `vfs_rename` | `src/tree.c` | 5 | Already implemented |
| `vfs_write`, `vfs_read` | `src/tree.c` | 5 | Already implemented |
| `vfs_mount`, `vfs_file_size/mtime/ctime` | `src/tree.c` | 5 | Already implemented |
| `vfs_readdir` | `src/tree.c` | 5 | Already implemented |
| `vfs_snapshot`, `vfs_commit`, `vfs_delete_snapshot` | `src/epoch.c` | 6 | Already implemented |
| `vfs_gc` | `src/gc.c` | 7 | Already implemented |

Public declarations live in `include/ixsphere/`:
- `include/ixsphere/vfs.h` — ALL public API declarations, grouped by functionality
- `include/ixsphere/vfs_internal.h` — internal structures (TreeContext, vfs_t)

Internal headers live in `src/` alongside their `.c` files:
- `src/nodes.h` — moved from `include/` for consistency
- `src/tree.h`, `src/pool.h`, `src/gc.h`, etc.

Final include convention:
```c
#include <ixsphere/vfs.h>          // public API
#include "nodes.h"                 // internal (src/ is on include path)
```

## Header Reorganization Tasks

### Task A — Move `include/nodes.h` → `src/nodes.h`
- Update all `#include "nodes.h"` to use the src path (already on include path)
- Remove `include/nodes.h`

### Task B — Move `include/ixsphere_vfs.h` → `include/ixsphere/vfs.h`
- Create `include/ixsphere/` directory
- Rename the file
- Update CMakeLists.txt public include path to `include`
- Update all internal `#include "ixsphere_vfs.h"` → `#include "vfs.h"` or
  `#include <ixsphere/vfs.h>`

### Task C — Merge `include/tree_api.h` into `include/ixsphere/vfs.h`
Move all function declarations into vfs.h, grouped by category:

```c
// Instance management
vfs_t*  vfs_mount(const char* path, int64_t page_size);
void    vfs_unmount(vfs_t* vfs);
int     vfs_flush(vfs_t* vfs);
vfs_error_t vfs_last_error(vfs_t* vfs);

// File operations
int     vfs_create(vfs_t*, int64_t parent, const char* name, int64_t epoch);
int     vfs_delete(vfs_t*, int64_t parent, const char* name, int64_t epoch);
int     vfs_rename(vfs_t*, int64_t src_p, const char* src, int64_t dst_p, const char* dst, int64_t epoch);
int64_t vfs_mount(vfs_t*, int64_t parent, const char* name, int64_t epoch);
int     vfs_read(vfs_t*, int64_t file, void* buf, int64_t offset, int64_t count, int64_t epoch);
int     vfs_write(vfs_t*, int64_t file, const void* data, int64_t offset, int64_t count, int64_t epoch);
int64_t vfs_file_size(vfs_t*, int64_t file, int64_t epoch);
int64_t vfs_file_mtime(vfs_t*, int64_t file, int64_t epoch);
int64_t vfs_file_ctime(vfs_t*, int64_t file);

// Directory operations
int     vfs_mkdir(vfs_t*, int64_t parent, const char* name, int64_t epoch);
int     vfs_rmdir(vfs_t*, int64_t parent, const char* name, int64_t epoch);
int     vfs_readdir(vfs_t*, int64_t dir, vfs_dirent_t* entries, int max, int64_t epoch);
typedef struct { int64_t nodeId; char name[256]; bool isDir; } vfs_dirent_t;

// Locking
int     vfs_lock(vfs_t*, int64_t file, int64_t epoch);
int     vfs_unlock(vfs_t*, int64_t file, int64_t epoch);

// Snapshots & commits
int64_t vfs_snapshot(vfs_t*);
int     vfs_commit(vfs_t*, int64_t snapshot_epoch);
int     vfs_delete_snapshot(vfs_t*, int64_t snapshot_epoch);

// GC
int     vfs_gc(vfs_t*);
```

### Task D — Move `include/vfs_internal.h` → `include/ixsphere/vfs_internal.h`
- Same namespace as public header
- Update all internal `#include "vfs_internal.h"` references

### Task E — Remove `include/tree_api.h`
- All declarations now in `include/ixsphere/vfs.h`
- Remove the file and update all `#include "tree_api.h"` references

### Task F — Update CMakeLists.txt
- Public include dir: `include` (so `#include <ixsphere/vfs.h>` works)
- Add `src` to internal include path (already done)

---

## Review Iteration 1 — Code vs Spec (2026-07-02)

### Implementation Status

| Deliverable | Status | Location |
|-------------|--------|----------|
| `vfs_flush` | ✅ Implemented | `src/vfs.c:247` — thin wrapper around `storage_flush` |
| `vfs_lock` | ✅ Implemented | `src/vfs.c:85` — hash table, 256 buckets, recursive, two-phase |
| `vfs_unlock` | ✅ Implemented | `src/vfs.c:142` — depth tracking, recursive unlock |
| `vfs_last_error` | ✅ Implemented | `src/vfs.c:253` — reads `ctx->last_error` |
| `vfs_mount` | ✅ Already existed | `src/vfs.c:194` — StorageBackend + TreeContext init |
| `vfs_unmount` | ✅ Already existed | `src/vfs.c:233` — flush + free + close |
| `vfs_error_string` | ✅ Already existed | `src/vfs.c:178` — 10 codes including VFS_ERR_EPOCH |
| Header `nodes.h` → `src/` | ✅ Complete | `src/nodes.h` |
| Header `ixsphere/vfs.h` | ✅ Complete | All 20 API functions, grouped by category |
| Header `ixsphere/vfs_internal.h` | ✅ Complete | TreeContext, vfs_t wrapper |
| `tree_api.h` removed | ✅ Complete | All declarations merged into `vfs.h` |
| CMakeLists.txt updated | ✅ Complete | `include` and `src` on path |

### Public API Surface (include/ixsphere/vfs.h)

20 functions, one clean header:
- **Lifecycle**: vfs_mount, vfs_unmount, vfs_flush, vfs_last_error
- **File**: vfs_create, vfs_delete, vfs_rename, vfs_mount, vfs_read, vfs_write, vfs_file_size/mtime/ctime
- **Directory**: vfs_mkdir, vfs_rmdir, vfs_readdir
- **Locking**: vfs_lock, vfs_unlock
- **Snapshots**: vfs_snapshot, vfs_commit, vfs_delete_snapshot
- **GC**: vfs_gc
- **Utility**: vfs_crc32c, vfs_error_string, vfs_dirent_t, vfs_error_t

### Locking Implementation Notes

Two-phase locking protocol for per-epoch locks:
1. Acquire global lock (epoch=0) for this file
2. Acquire per-epoch mutex
3. Release global lock

This ensures a global lock holder blocks per-epoch lock attempts. Recursive
locking supported (same thread, same epoch). Hash table with 256 buckets,
reference-counted entries.

### Phase 8 Gate: ✅ COMPLETE

All deliverables implemented, headers clean, API consistent. No blocking issues.

## Dependencies
All previous phases must be complete. This phase is a thin wrapper.

## Debt from Previous Phases

| Item | Phase | Description | Resolution |
|------|-------|-------------|------------|
| Subdirectory commit conflict detection | 6 | `vfs_commit` only scans root DirContent chain — modified files in subdirectories are not checked for conflicts. Commit may silently succeed when it should fail with VFS_ERR_CONFLICT. | Implement recursive tree walk in `vfs_commit` that follows `childPtr` into subdirectory DirContent chains, collecting all file nodeIds at all directory levels. |

---

## Workload 8.1 — Instance Management

### What
Implement `vfs_flush`, `vfs_last_error`, and complete the `vfs_unmount` cleanup.
`vfs_mount` is already implemented in `src/vfs.c` (wired to Phase 5 bootstrap).

### New Implementation Required

### `vfs_t* vfs_mount(const char* path, int64_t page_size)`
Already implemented in `src/vfs.c`. Creates or mounts StorageBackend,
bootstraps tree context (superblock + root directory). Wire the TreeContext's
`last_error` field for error reporting.

### `void vfs_unmount(vfs_t* vfs)`
Already implemented. Ensure it calls `vfs_flush` before `storage_close`,
then frees TreeContext, pool resources, mapper, and the handle itself.

### `int vfs_flush(vfs_t* vfs)`
**NEW.** Thin wrapper:
```
1. storage_flush(vfs->ctx->sb, -1)
2. Return VFS_OK on success, VFS_ERR_IO on failure.
```

### `vfs_error_t vfs_last_error(vfs_t* vfs)`
**NEW.** Add `last_error` field to TreeContext. Return it here. Do NOT clear it.

### Acceptance
- [ ] `vfs_mount("new.vfs", 8192)` → creates file, bootstrap, returns valid handle
- [ ] `vfs_unmount(handle)` → flush, free, Valgrind clean
- [ ] `vfs_flush` → data survives kill -9 + remount

---

## Workload 8.2 — File Operations

### `int64_t vfs_create(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch)`

```
1. Resolve epoch: if epoch == -1, epoch = superblock->currentEpoch.
2. Call tree_create(parent, name, epoch) from Phase 5.
3. On success: return new nodeId. On failure: set last_error, return -1.
```

### `int64_t vfs_mount(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch)`

```
1. Resolve epoch.
2. Walk parent DirNode's headPtr chain at epoch.
   Find DirContent with matching name.
3. If found: return childNodeId.
   If not found: last_error = VFS_ERR_NOTFOUND, return -1.
```

### `int vfs_read(vfs_t* vfs, int64_t file, void* buf, int64_t offset, int64_t count, int64_t epoch)`

```
1. Resolve epoch.
2. Call tree_read from Phase 5 Workload 5.4.
3. Return bytes read (may be < count at EOF), or -1 on error.
```

### `int vfs_write(vfs_t* vfs, int64_t file, const void* data, int64_t offset, int64_t count, int64_t epoch)`

```
1. Resolve epoch.
2. Call tree_write from Phase 5 Workload 5.3.
3. Return bytes written, or -1 on error.
```

### `int vfs_delete(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch)`

```
1. Resolve epoch.
2. Call tree_delete from Phase 5 Workload 5.2.
3. Return VFS_OK or error.
```

### `int64_t vfs_file_size(vfs_t* vfs, int64_t file, int64_t epoch)`

```
1. Resolve epoch.
2. Call tree_file_size from Phase 5 Workload 5.6.
```

### `int64_t vfs_file_mtime(vfs_t* vfs, int64_t file, int64_t epoch)`

```
1. Resolve epoch.
2. Call tree_file_mtime.
```

### `int64_t vfs_file_ctime(vfs_t* vfs, int64_t file)`

```
1. No epoch needed. Call tree_file_ctime (reads createdAt directly from FileNode).
```

### Acceptance
- [ ] Full CRUD cycle: create → write → read → delete → verify not found
- [ ] Read at old epoch returns data from that epoch
- [ ] Write at snapshot epoch doesn't affect live-head readers
- [ ] File size updates on write; size at old epoch returns old size
- [ ] mtime changes on write; ctime fixed at creation

---

## Workload 8.3 — Directory Operations

### `int vfs_mkdir(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch)`

```
1. Resolve epoch.
2. Call tree_mkdir from Phase 5 Workload 5.5.
```

### `int vfs_rmdir(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch)`

```
1. Resolve epoch.
2. Call tree_rmdir. Validate directory is empty at this epoch. Fail with
   VFS_ERR_NOTEMPTY if children exist.
```

### `int vfs_readdir(vfs_t* vfs, int64_t dir, vfs_dirent_t* entries, int max, int64_t epoch)`

```
1. Resolve epoch.
2. Call tree_readdir.
3. Fill entries[] up to max:
   entries[i].nodeId = childNodeId
   entries[i].isDir  = true if child is DirNode
   strncpy(entries[i].name, name, 255)
4. Return number written.
```

### `int vfs_rename(vfs_t* vfs, int64_t src_parent, const char* src_name, int64_t dst_parent, const char* dst_name, int64_t epoch)`

```
1. Resolve epoch.
2. Call tree_rename from Phase 5.
```

### `vfs_dirent_t` Definition

```c
typedef struct {
    int64_t nodeId;
    char    name[256];
    bool    isDir;
} vfs_dirent_t;
```

### Acceptance
- [ ] mkdir "a", mkdir "a/b", create "a/b/c.txt" → readdir "a/b" returns 1 entry
  with name "c.txt", isDir=false, valid nodeId
- [ ] rmdir on non-empty → VFS_ERR_NOTEMPTY
- [ ] Delete file in dir → rmdir → succeeds
- [ ] Rename same-dir: old name gone, new name present, nodeId unchanged
- [ ] Rename cross-dir: source loses entry, destination gains it
- [ ] readdir at old epoch shows state as of that epoch

---

## Workload 8.4 — Locking

### What
Explicit per-file locking API with two modes: global (epoch=0) serializes all
epochs; per-epoch (specific epoch) allows cross-epoch concurrency.

### Data Structure

```c
typedef struct {
    pthread_mutex_t mutex;
    int64_t         nodeId;
    int64_t         epoch;      // 0 = global lock
    int             refcount;   // for recursive locking within same thread
} FileLock;

// Hash table: key = (nodeId << 32) | (epoch & 0xFFFFFFFF)
// NULL epoch (0) is the global lock — all other epoch locks must respect it.
```

### `int vfs_lock(vfs_t* vfs, int64_t file, int64_t epoch)`

```
1. If epoch == 0:
     // Global lock request
     // 1. Set "global_pending" flag for this file
     // 2. Wait for all active per-epoch locks on this file to drain (refcount → 0)
     // 3. Acquire global mutex
2. Else:
     // Per-epoch lock request
     // 1. Check if global lock is held or pending for this file → if yes, block
     // 2. Acquire per-epoch mutex
3. Return VFS_OK.
```

### `int vfs_unlock(vfs_t* vfs, int64_t file, int64_t epoch)`

```
1. Release the mutex for (file, epoch).
2. If this was the global lock: clear "global_pending" flag.
3. Return VFS_OK.
```

### Internal Write Locking
All write operations (vfs_write, vfs_create, vfs_delete, vfs_mkdir, vfs_rmdir,
vfs_rename) automatically acquire the per-epoch lock for the write's epoch
BEFORE performing the operation. This is transparent to the caller. The
explicit `vfs_lock`/`vfs_unlock` API is for callers who need to hold the 
lock across multiple operations.

### Acceptance
- [ ] Lock/unlock same file: no error
- [ ] Two threads lock same file at same epoch: second blocks until first unlocks
- [ ] Two threads lock same file at different epochs: both proceed concurrently
- [ ] Global lock (epoch=0) blocks per-epoch locks
- [ ] Global lock waits for per-epoch locks to drain before acquiring
- [ ] Unlocking non-locked file returns error
- [ ] Internal write acquires and releases the per-epoch lock automatically

---

## Workload 8.5 — Snapshot, Commit, and GC

### `int64_t vfs_snapshot(vfs_t* vfs)`

```
1. Return epoch_snapshot(vfs) from Phase 6 Workload 6.2.
```

### `int vfs_commit(vfs_t* vfs, int64_t snapshot_epoch)`

```
1. Call epoch_commit(vfs, snapshot_epoch) from Phase 6 Workload 6.3.
2. If success: flush superblock (storage_write + storage_flush for the
   superblock page) to persist the new mapper state.
3. Return VFS_OK or VFS_ERR_CONFLICT.
```

### `int vfs_delete_snapshot(vfs_t* vfs, int64_t snapshot_epoch)`

```
1. Call epoch_soft_delete(vfs, snapshot_epoch) from Phase 6 Workload 6.4.
2. Return VFS_OK.
```

### `int vfs_gc(vfs_t* vfs)`

```
1. Call gc_run(vfs) from Phase 7.
2. This is a blocking call — all other operations wait until it completes.
3. Return VFS_OK.
```

### Acceptance
- [ ] Snapshot returns odd epoch; multiple snapshots return distinct epochs
- [ ] Commit clean snapshot → succeeds; read at snapshot epoch returns committed data
- [ ] Commit with conflict → VFS_ERR_CONFLICT
- [ ] Soft-delete → snapshot data invisible
- [ ] GC after soft-delete → space reclaimed

---

## Workload 8.6 — Error Handling

### `vfs_error_t vfs_last_error(vfs_t* vfs)`

Returns `vfs->last_error`. Does NOT clear it.

### `const char* vfs_error_string(vfs_error_t err)`

Returns string literal for the error code. Already implemented in Phase 1.

### Error Setting Convention

Every API function that can fail must:
1. Set `vfs->last_error = ERROR_CODE` on the first failure encountered.
2. Return the error indicator (-1, NULL, or error code).
3. On success: do NOT reset `last_error`. Leave it unchanged.

### Error Codes

| Code | Value | Meaning |
|------|-------|---------|
| VFS_OK | 0 | Success |
| VFS_ERR_IO | -1 | I/O error, frozen epoch, corrupt data |
| VFS_ERR_NOTFOUND | -2 | File or directory not found |
| VFS_ERR_EXISTS | -3 | Already exists (create when file exists) |
| VFS_ERR_NOTDIR | -4 | Expected directory, got file |
| VFS_ERR_NOTEMPTY | -5 | Directory not empty (rmdir) |
| VFS_ERR_CONFLICT | -6 | Commit conflict |
| VFS_ERR_FULL | -7 | No space left |
| VFS_ERR_NOMEM | -8 | Out of memory |

### Acceptance
- [ ] `vfs_mount` on missing file → returns -1, last_error = VFS_ERR_NOTFOUND
- [ ] `vfs_write` to frozen epoch → returns -1, last_error = VFS_ERR_IO
- [ ] `vfs_last_error` after successful operation returns previous error (not cleared)
- [ ] `vfs_error_string` returns non-NULL for every defined code

---

## Final Phase 8 Checklist

- [ ] All API functions callable and return documented results
- [ ] Epoch isolation: snapshot writes don't affect live-head reads
- [ ] Locking: per-epoch concurrent, global serializes, compatibility rules enforced
- [ ] Error codes set correctly for all failure paths
- [ ] Multi-threaded stress test: 4 threads, each doing create/write/read/delete
  on different files, 10 seconds, no crashes, no leaks, no double-frees
