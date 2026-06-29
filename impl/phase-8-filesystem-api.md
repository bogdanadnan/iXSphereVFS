# Phase 8: Filesystem API

## Goal
Expose the full VFS functionality through a clean, POSIX-like C API. Every
function accepts an optional epoch parameter for snapshot isolation. The API
covers instance management, file and directory operations, locking, snapshot
lifecycle, and garbage collection.

---

## Workload 8.1 — Instance Management

**What:** Functions to open, close, and flush a VFS instance.

**Why:** Every VFS consumer starts by opening a backing file. The open call
handles both creation and mounting of existing instances.

**How:**
- `vfs_t* vfs_open(const char* path)`: if the file does not exist, create it,
  bootstrap the StorageBackend header and bitmap (Phase 2), allocate the
  superblock, and initialize the root directory (Phase 5). If the file exists,
  validate the XVFS magic, load the bitmap, read the superblock, and walk
  the epoch mapper chain. Return a handle to the VFS instance, or NULL on
  error.
- `void vfs_close(vfs_t* vfs)`: flush all dirty pages, free the page cache,
  free all in-memory structures, close the backing file descriptor.
- `int vfs_flush(vfs_t* vfs)`: call `Flush(-1)` on the StorageBackend to
  write all dirty pages and fsync. Returns `VFS_OK` on success, `VFS_ERR_IO`
  on failure.
- The `vfs_t` handle is opaque to the caller. It contains the StorageBackend
  state, the superblock in-memory copy, the page cache, the mapper dictionary,
  the file lock hash table, and the pool list head.

**Acceptance:**
  - `vfs_open` with a new path creates a valid instance; `vfs_close` cleans up.
  - `vfs_open` with an existing path mounts the previous state; data written
    before close is visible after reopen.
  - `vfs_flush` followed by process kill and remount preserves all committed
    data.
  - `vfs_open` with a non-VFS file fails (magic validation).

---

## Workload 8.2 — File Operations

**What:** Create, open, read, write, delete, and stat files. All accept an
optional epoch parameter (pass -1 for current live head).

**Why:** These are the primary data operations. The API must be simple enough
for a developer to use without understanding the internal epoch and version
chain mechanics.

**How:**
- `int64_t vfs_create(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch)`:
  create a file under `parent` directory. Returns the new file's nodeId, or
  -1 on error.
- `int64_t vfs_open_file(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch)`:
  resolve a path component to a file nodeId. Returns -1 if not found. This
  is `openat`-style — it resolves one name component relative to a parent
  directory. Multi-component paths are the caller's responsibility.
- `int vfs_read(vfs_t* vfs, int64_t file, void* buf, int64_t offset, int64_t count, int64_t epoch)`:
  read up to `count` bytes from `offset`. Returns the number of bytes read
  (may be less at EOF), or -1 on error. Uses the read rule with epoch mapper.
- `int vfs_write(vfs_t* vfs, int64_t file, const void* data, int64_t offset, int64_t count, int64_t epoch)`:
  write `count` bytes at `offset`. Handles COW on first write to a page in the
  epoch, in-place on subsequent writes. Updates FileSize. Returns bytes written.
- `int vfs_delete(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch)`:
  tombstone the file. Returns `VFS_OK` or `VFS_ERR_NOTFOUND`.
- `int64_t vfs_file_size(vfs_t* vfs, int64_t file, int64_t epoch)`: return file
  size in bytes via the read rule on the `sizePtr` chain.
- `int64_t vfs_file_mtime(vfs_t* vfs, int64_t file, int64_t epoch)`: return
  modification timestamp via the read rule on the `sizePtr` chain.
- `int64_t vfs_file_ctime(vfs_t* vfs, int64_t file)`: return creation timestamp
  from the FileNode's `createdAt` field.
- All epoch parameters accept -1 meaning "current live head." The VFS resolves
  this to the actual `currentEpoch` from the superblock before performing the
  operation.

**Acceptance:**
  - Full CRUD cycle: create → write → read → delete → verify not found.
  - Read at an old epoch returns data as it existed at that epoch.
  - Write at a snapshot epoch does not affect live-head readers.
  - File size updates correctly as data is written.
  - mtime changes on write; ctime is fixed at creation.

---

## Workload 8.3 — Directory Operations

**What:** Create and remove directories, list directory contents.

**Why:** Directories organize the namespace. Listing must correctly deduplicate
entries across epochs and respect snapshot isolation.

**How:**
- `int vfs_mkdir(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch)`:
  create a directory. Returns `VFS_OK` or error.
- `int vfs_rmdir(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch)`:
  remove an empty directory. Returns `VFS_ERR_NOTEMPTY` if the directory
  contains visible children at the given epoch.
- `int vfs_readdir(vfs_t* vfs, int64_t dir, vfs_dirent_t* entries, int max, int64_t epoch)`:
  fill `entries` with up to `max` directory entries. Each entry is a
  `vfs_dirent_t` struct containing `nodeId` (int64_t), `name` (char[256]),
  and `isDir` (bool). Returns the number of entries written. The caller
  iterates by calling repeatedly until 0 is returned.
- `vfs_rename(vfs_t* vfs, int64_t src_parent, const char* src_name, int64_t dst_parent, const char* dst_name, int64_t epoch)`:
  rename a file or directory. Same-directory same-epoch: update `namePtr`
  in-place. Cross-directory or new epoch: create new DirContent at destination,
  tombstone at source.
- Listing uses the dentry cache (Phase 5) after the first call for a given
  directory and epoch.

**Acceptance:**
  - mkdir "a", mkdir "a/b", create "a/b/c.txt": readdir "a/b" returns one entry.
  - rmdir on non-empty directory fails.
  - Rename same-dir: listing shows new name, old name gone. NodeId unchanged.
  - Rename cross-dir: source directory loses entry, destination gains it.
  - Listing at an old epoch shows the state as of that epoch.

---

## Workload 8.4 — Locking

**What:** Explicit per-file locking with two modes: global (epoch=0) serializes
all epochs; per-epoch (specific epoch) allows cross-epoch concurrency.

**Why:** SQLite needs global locking to serialize all writes to a database file.
Snapshot-isolated workloads benefit from per-epoch locking where writers at
different epochs don't block each other. The API exposes both.

**How:**
- `int vfs_lock(vfs_t* vfs, int64_t file, int64_t epoch)`: acquire the lock.
  If `epoch` is 0, acquires the global lock — all other lock requests on this
  file block until released. If `epoch` is a specific value, acquires the
  per-epoch lock — only same-epoch writers block; cross-epoch writers proceed
  concurrently.
- `int vfs_unlock(vfs_t* vfs, int64_t file, int64_t epoch)`: release the lock.
- Internal write operations automatically acquire the per-epoch lock for the
  write's epoch. The explicit API is for callers who need to hold the lock
  across multiple operations (e.g., SQLite's multi-page transactions).
- Lock compatibility rules (enforced by the lock implementation):
  - Global lock is incompatible with all other locks on the same file. A
    global lock request waits for all active per-epoch locks to drain. New
    per-epoch requests block while a global lock is held or pending.
  - Per-epoch locks with different epochs are compatible — they don't block
    each other.
- The lock hash table is keyed by `(nodeId, epoch)`. Locks are created lazily
  and never removed. The hash table is thread-safe (mutex per bucket).

**Acceptance:**
  - Lock and unlock a file: no error.
  - Two threads locking the same file at the same epoch: second blocks until
    first unlocks.
  - Two threads locking the same file at different epochs: both proceed
    concurrently.
  - Global lock (epoch=0) blocks per-epoch locks. Per-epoch locks drain before
    global lock is granted.
  - Unlocking a file that was not locked returns an error.

---

## Workload 8.5 — Snapshot, Commit, and GC

**What:** Public API for the epoch lifecycle.

**Why:** These operations are exposed so applications can manage snapshots
and trigger garbage collection without understanding the internal epoch
machinery.

**How:**
- `int64_t vfs_snapshot(vfs_t* vfs)`: take a snapshot. Returns the new
  snapshot epoch (always odd), or -1 on error. The live head advances by 2.
- `int vfs_commit(vfs_t* vfs, int64_t snapshot_epoch)`: commit a snapshot.
  Returns `VFS_OK` on success, `VFS_ERR_CONFLICT` if a conflict is detected,
  or another error code.
- `int vfs_delete_snapshot(vfs_t* vfs, int64_t snapshot_epoch)`: soft-delete
  a snapshot. Returns `VFS_OK`. The snapshot's data becomes invisible; space
  is reclaimed by the next GC.
- `int vfs_gc(vfs_t* vfs)`: run garbage collection. This is a blocking call
  — all other operations wait until GC completes. Returns `VFS_OK` on success.

**Acceptance:**
  - Snapshot returns an odd epoch. Multiple snapshots return distinct epochs.
  - Commit of a clean snapshot succeeds. Commit with a conflict fails.
  - Soft-deleted snapshot data is invisible.
  - GC after soft-delete reclaims space (measurable via `vfs_file_size` or
    by observing that pool page count decreases).

---

## Workload 8.6 — Error Handling

**What:** A consistent error reporting mechanism. Every API function that can
fail sets a per-instance error code retrievable by the caller.

**Why:** Callers need to distinguish between "file not found," "I/O error,"
"out of space," and other failure modes without parsing error strings.

**How:**
- `vfs_error_t vfs_last_error(vfs_t* vfs)`: return the error code from the
  most recent operation on this instance.
- `const char* vfs_error_string(vfs_error_t err)`: return a human-readable
  string for a given error code.
- All API functions that return an error indicator (negative return, NULL
  pointer) MUST set `vfs->last_error` before returning. Functions that succeed
  leave `last_error` unchanged (it is NOT reset to `VFS_OK` on success).
- Error codes: `VFS_OK` (0), `VFS_ERR_IO` (-1), `VFS_ERR_NOTFOUND` (-2),
  `VFS_ERR_EXISTS` (-3), `VFS_ERR_NOTDIR` (-4), `VFS_ERR_NOTEMPTY` (-5),
  `VFS_ERR_CONFLICT` (-6), `VFS_ERR_FULL` (-7), `VFS_ERR_NOMEM` (-8).
- Thread safety: `last_error` is per-instance. In a multi-threaded program,
  each thread should call `vfs_last_error` immediately after the operation
  that failed, before any other thread performs an operation on the same
  instance.

**Acceptance:**
  - `vfs_open_file` on a non-existent file returns -1 and sets error to
    `VFS_ERR_NOTFOUND`.
  - `vfs_write` to a read-only epoch (frozen history) returns -1 and sets
    an appropriate error.
  - `vfs_last_error` after a successful operation returns whatever the
    previous error was (it is not cleared).
  - `vfs_error_string` returns a non-NULL string for every defined error code.

---

## Deliverables

| File | Purpose |
|------|---------|
| `src/api.c` | All public API function implementations |
| `src/api.h` | (update `include/ixsphere_vfs.h`) Full public API declarations |
| `test/test_api.c` | Integration tests exercising the full API surface |

## Success Criteria
- All API functions are callable and return documented results.
- Snapshot isolation: data written at snapshot epoch N is visible at epoch N
  but not at N-1 or N+1 before commit.
- Locking: concurrent writers at different epochs don't block; same-epoch
  writers serialize; global lock serializes everything.
- GC: after soft-delete + GC, space is reclaimed and data is gone.
- Error codes are set correctly for all failure paths tested.
- Multi-threaded stress test: 4 threads each doing create/write/read/delete
  on different files for 10 seconds, no crashes, no leaks.
