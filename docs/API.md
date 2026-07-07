# iXSphereVFS API Reference

## Instance Management

### `vfs_mount`

```c
vfs_t* vfs_mount(const char* path, int64_t page_size);
```

Open or create a VFS backing file. If the file does not exist, a new VFS is
created with the given `page_size` (typically 8192). If the file exists, it is
opened and validated (XVFS magic check). Returns a `vfs_t*` handle or `NULL` on
failure. Check `vfs_last_error()` for the specific error.

### `vfs_unmount`

```c
void vfs_unmount(vfs_t* vfs);
```

Close the VFS handle. Writes the superblock, destroys in-memory structures
(mapper table, page array cache, dentry cache), and closes the backing file.
**Does not flush dirty data pages** — call `vfs_flush` first if data must survive.

### `vfs_flush`

```c
int vfs_flush(vfs_t* vfs);
```

Flush all dirty pages to disk in priority order (data → pool → superblock).
Returns `VFS_OK` on success. Call before `vfs_unmount` to ensure data
persistence.

### `vfs_last_error`

```c
vfs_error_t vfs_last_error(vfs_t* vfs);
```

Return the error code from the last failed operation, or `VFS_OK` if the last
operation succeeded. Use `vfs_error_string(err)` for a human-readable message.

---

## File Operations

### `vfs_create`

```c
int64_t vfs_create(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch);
```

Create a file named `name` under the directory `parent`. `parent` is a VirtualPtr
to a DirNode (from `vfs_mkdir` return or `vfs->ctx->rootNodeOffset` for root).
Returns the child's VirtualPtr on success (> 0), or a negative error code:
- `VFS_ERR_EXISTS`: file already exists in this epoch
- `VFS_ERR_NOTDIR`: parent is not a directory
- `VFS_ERR_FULL`: pool allocation failed

### `vfs_delete`

```c
int vfs_delete(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch);
```

Delete a file from its parent directory. The file is tombstoned (a DirContent
entry with `namePtr = 0` marks the deletion at this epoch). The underlying
data is freed by GC. Returns `VFS_OK` or error.

### `vfs_rename`

```c
int vfs_rename(vfs_t* vfs, int64_t src_parent, const char* src,
               int64_t dst_parent, const char* dst, int64_t epoch);
```

Rename a file or directory. Returns `VFS_OK` or error.

### `vfs_open`

```c
int64_t vfs_open(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch);
```

Find a file by name in a directory. Returns the child's **VirtualPtr** on
success, or a negative error code.  The returned VirtualPtr can be used
directly with `vfs_read`, `vfs_write`, `vfs_file_size`, etc.

### `vfs_read`

```c
int vfs_read(vfs_t* vfs, int64_t file, void* buf, int64_t offset,
             int64_t count, int64_t epoch);
```

Read up to `count` bytes from `file` at `offset`. `file` is a VirtualPtr to a
FileNode (from `vfs_create` or `vfs_readdir`). `epoch` selects the version:
0 = current base, odd = snapshot. Returns bytes read, or negative error.
**CRC corruption**: if the data page CRC is invalid (single-copy write loss),
returns the byte count but fills the buffer with zeros.

### `vfs_write`

```c
int vfs_write(vfs_t* vfs, int64_t file, const void* data, int64_t offset,
              int64_t count, int64_t epoch);
```

Write up to `count` bytes to `file` at `offset`. Grows the file if necessary.
**Known limitation**: writes at epoch 0 after a snapshot do in-place updates
to the same VersionPage (no COW for same-offset writes after snapshot).

### `vfs_file_size` / `vfs_file_mtime` / `vfs_file_ctime`

```c
int64_t vfs_file_size(vfs_t* vfs, int64_t file, int64_t epoch);
int64_t vfs_file_mtime(vfs_t* vfs, int64_t file, int64_t epoch);
int64_t vfs_file_ctime(vfs_t* vfs, int64_t file);
```

Query file metadata. `file` is a VirtualPtr to a FileNode. `epoch` selects
the version. `vfs_file_ctime` is immutable after creation.

---

## Directory Operations

### `vfs_mkdir`

```c
int64_t vfs_mkdir(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch);
```

Create a directory. Returns the child's VirtualPtr on success.

### `vfs_rmdir`

```c
int vfs_rmdir(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch);
```

Remove an empty directory. Returns `VFS_ERR_NOTEMPTY` if the directory has
children. Delete all children first.

### `vfs_readdir`

```c
int vfs_readdir(vfs_t* vfs, int64_t dir, vfs_dirent_t* entries,
                int max, int64_t epoch);
```

List directory contents. Returns the number of entries written (up to `max`).
Each entry contains:
- `vp`: VirtualPtr of the child — usable directly with `vfs_read`/`vfs_write`
- `nodeId`: stable identifier for the child
- `name`: entry name (up to 256 bytes)
- `isDir`: true if directory, false if file

**Note**: `max` is limited to 1024 entries (DENTRY_CACHE_MAX).

---

## Locking

### `vfs_lock` / `vfs_unlock`

```c
int vfs_lock(vfs_t* vfs, int64_t file, int64_t epoch);
int vfs_unlock(vfs_t* vfs, int64_t file, int64_t epoch);
```

Acquire/release a per-file lock for snapshot isolation. `vfs_create`
automatically acquires a lock on the new nodeId. Applications should lock
files before writing and unlock after.

---

## Snapshot & Commit

### `vfs_snapshot`

```c
int64_t vfs_snapshot(vfs_t* vfs);
```

Create a read-only snapshot of the current base state. Returns the snapshot
epoch (always odd: 1, 3, 5...), or negative error. The base epoch advances
by 2 (`currentEpoch += 2`). Read snapshot data by passing the snapshot epoch
to `vfs_read`.

### `vfs_commit`

```c
int vfs_commit(vfs_t* vfs, int64_t snapshot_epoch);
```

Merge a snapshot back into the base epoch. Inserts a mapper entry
(`snapshot_epoch → base_epoch` with traversalApply). After commit,
the snapshot's data is visible at the base epoch. Writes the superblock.

### `vfs_delete_snapshot`

```c
int vfs_delete_snapshot(vfs_t* vfs, int64_t snapshot_epoch);
```

Soft-delete a snapshot. The snapshot's data becomes inaccessible. Recoverable
until GC runs.

---

## Garbage Collection

### `vfs_gc`

```c
int vfs_gc(vfs_t* vfs);
```

Shadow-compact the tree. Walks all live nodes, copies entries to fresh pool
pages, atomically swaps the superblock, and frees dead data pages. Acquires
an exclusive tree lock — blocks all other operations. Returns `VFS_OK` or
`VFS_ERR_FULL` if pool allocation fails (old state preserved).

---

## Error Handling

All functions returning `int` or `int64_t` return negative `vfs_error_t` values
on failure:

| Code | Name | Description |
|------|------|-------------|
| 0 | `VFS_OK` | Success |
| -1 | `VFS_ERR_IO` | I/O error or invalid argument |
| -2 | `VFS_ERR_NOTFOUND` | File or directory not found |
| -3 | `VFS_ERR_EXISTS` | File already exists in this epoch |
| -4 | `VFS_ERR_NOTDIR` | Path component is not a directory |
| -5 | `VFS_ERR_NOTEMPTY` | Directory not empty |
| -6 | `VFS_ERR_CONFLICT` | Snapshot commit conflict |
| -7 | `VFS_ERR_FULL` | Pool or storage exhausted |
| -8 | `VFS_ERR_NOMEM` | Out of memory |
| -9 | `VFS_ERR_EPOCH` | Invalid epoch |

Use `vfs_last_error(vfs)` to get the last error, and `vfs_error_string(err)`
for a human-readable string.

---

## Thread Safety

- **Page cache**: spin-locked hash buckets. Multiple readers/writers can
  operate on different pages concurrently.
- **Pool allocation**: lock-free CAS on poolState. Safe across threads.
- **Storage allocation**: CAS on physical_tail + indirection entries.
- **Directory operations**: CAS on DirContent headPtr. Concurrent creates
  in the same directory serialize on this CAS — throughput drops with threads.
- **GC**: exclusive tree lock blocks all other operations.
- **Snapshot/commit**: epoch counter is atomic.

Safe for multi-threaded use as long as different threads operate on different
files/directories. Concurrent creates in the same root directory cause CAS
contention — see [CONTENTION.md](CONTENTION.md).

---

## Known Limitations

1. **No COW after snapshot**: Writes at epoch 0 after a snapshot do in-place
   updates to the same VersionPage (no copy-on-write for same-offset writes).
   Use different offsets or commit the snapshot before further writes.
2. **vfs_unmount does not flush**: Call `vfs_flush` before `vfs_unmount` to
   ensure data persistence. Data pages in the write-back cache are lost on
   unmount without flush.
3. **readdir limit**: `vfs_readdir` is capped at 1024 entries (DENTRY_CACHE_MAX).
4. **Single-copy writes**: The first write to a page creates a single on-disk
   copy. If the process crashes before the second write (which allocates a
   mirror sibling), the data may be lost. Use `vfs_flush` frequently.
5. **GC pool exhaustion**: `vfs_gc` returns `VFS_ERR_FULL` if it cannot
   allocate new pool pages for compaction. The old state is preserved.
6. **Epoch limit**: `int64_t` epoch counter overflows in ~146 billion years
   at 2 snapshots/second — practically unbounded.
