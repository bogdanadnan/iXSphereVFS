# Phase 8: Filesystem API

## Goal
Public C API matching SPEC.md §12.

## Workloads

### 8.1 Instance Management
- `vfs_open(path)` — create/open backing file, mount, return `vfs_t*`
- `vfs_close(vfs)` — flush, free resources
- `vfs_flush(vfs)` — `Flush(-1)` via StorageBackend
- Tests: open new, close, reopen existing, verify persistent data

### 8.2 File Operations
- `vfs_create(parent, name, epoch)` → nodeId
- `vfs_open_file(parent, name, epoch)` → nodeId (resolve existing)
- `vfs_read(file, buf, offset, count, epoch)` → bytes read
- `vfs_write(file, data, offset, count, epoch)` → bytes written
- `vfs_file_size(file, epoch)` → size
- `vfs_file_mtime(file, epoch)` → modification timestamp
- `vfs_file_ctime(file)` → creation timestamp
- Tests: CRUD cycle, multi-page read/write, cross-epoch isolation

### 8.3 Directory Operations
- `vfs_mkdir(parent, name, epoch)`
- `vfs_rmdir(parent, name, epoch)` — fails if not empty
- `vfs_readdir(dir, entries, max, epoch)` → fills `vfs_dirent_t[]`
- Tests: mkdir, create file in dir, readdir, delete, rmdir

### 8.4 Locking
- `vfs_lock(file, epoch)` — global if epoch=0, per-epoch otherwise
- `vfs_unlock(file, epoch)`
- Lock compatibility: global waits for epoch locks, epochs block during global
- Tests: concurrent lock/unlock, global vs epoch blocking, no deadlocks

### 8.5 Snapshot & Commit
- `vfs_snapshot()` → new odd epoch
- `vfs_commit(snapshot_epoch)` → success or VFS_ERR_CONFLICT
- `vfs_delete_snapshot(epoch)` → soft-delete
- Tests: snapshot→write→commit, snapshot→conflict→abort, delete→verify invisible

### 8.6 GC
- `vfs_gc()` → blocking, returns when complete
- Tests: GC after delete, verify space reclaimed

### 8.7 Error Handling
- All functions return negative on error, set `vfs_last_error(vfs)`
- `vfs_error_string(err)` → human-readable
- Tests: trigger each error code, verify message
