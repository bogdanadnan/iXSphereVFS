# Phase 12: FUSE Integration

## Goal
Mount an iXSphereVFS instance as a regular filesystem via FUSE. Every POSIX
filesystem operation maps to an existing VFS API call. Custom ioctl commands
expose snapshot, commit, soft-delete, and GC through the mount point.

## Dependencies

| Dependency | Phase | Purpose |
|------------|-------|---------|
| Full VFS API | Phase 8 | All filesystem operations |
| libfuse 3.x | External | FUSE callbacks, mount, event loop |
| pthread | OS | libfuse links against pthread |

## File Organization

| File | Purpose |
|------|---------|
| `src/fuse_vfs.c` | FUSE operations + mount entry point |
| `src/fuse_ioctl.c` | Custom ioctl commands (snapshot, commit, GC) |
| `tools/vfsctl.c` | CLI tool for calling ioctls on the mount point |
| `test/test_fuse.sh` | Integration tests via shell commands |

## Non-Negotiable Constraints

- **libfuse 3.x only.** Use the high-level API (`fuse_operations` struct, `fuse_main_real`).
- **Single-threaded FUSE by default.** Multi-threaded (`-o default_permissions,allow_other`) optional.
- **Epoch selection via mount option.** `-o epoch=N` selects the read epoch. Default -1 (live head).
- **No VFS code changes.** This phase is purely a shim layer.
- **VirtualPtr handles in FUSE `fi->fh`.** `vfs_create`/`vfs_open` return VirtualPtrs which fit in `uint64_t`.
  No nodeId/VirtualPtr confusion at the FUSE level.
- **Init/destroy callbacks required.** `init` calls `vfs_mount`, `destroy` calls `vfs_unmount`.
  The VFS file path is passed as private data via `fuse_main_real`.

---

## Workload 12.1 — FUSE Callback Mapping

### What
Implement `fuse_operations` callbacks, each calling the corresponding VFS
function. Path resolution walks DirContent chains component by component
starting from root (nodeId 0).

### Path Resolution

FUSE passes full paths. Split and resolve each component using `vfs_open`,
tracking VirtualPtrs for each level. The root directory VirtualPtr is
`vfs->ctx->rootNodeOffset`.

```c
int64_t resolve_full_path(vfs_t* vfs, const char* path, int64_t epoch) {
    if (strcmp(path, "/") == 0) return vfs->ctx->rootNodeOffset;
    int64_t parent = vfs->ctx->rootNodeOffset;
    char* copy = strdup(path);
    char* tok = strtok(copy, "/");
    int64_t current = 0;
    while (tok) {
        current = vfs_open(vfs, parent, tok, epoch);
        if (current <= 0) break;  // error or not found
        parent = current;         // current is a VirtualPtr
        tok = strtok(NULL, "/");
    }
    free(copy);
    return current > 0 ? current : 0;
}
```

### Callback Mapping

| FUSE callback | VFS call | Notes |
|---------------|----------|-------|
| `init` | `vfs_mount(path, page_size)` | VFS file path from private_data |
| `destroy` | `vfs_unmount(vfs)` | |
| `getattr` | `vfs_file_size` + `vfs_file_mtime` + `vfs_file_ctime` | |
| `readdir` | `vfs_readdir` | Root: parent=root_vp |
| `open` | `vfs_open(parent_vp, name, epoch)` | Returns VirtualPtr stored in `fi->fh` |
| `read` | `vfs_read(vp, buf, offset, count, epoch)` | `vp` from `fi->fh` |
| `write` | `vfs_write(vp, data, offset, count, epoch)` | |
| `create` | `vfs_create(parent_vp, name, epoch)` | Returns VirtualPtr stored in `fi->fh` |
| `mkdir` | `vfs_mkdir(parent_vp, name, epoch)` | |
| `unlink` | `vfs_delete(parent_vp, name, epoch)` | |
| `rmdir` | `vfs_rmdir(parent_vp, name, epoch)` | |
| `rename` | `vfs_rename(src_vp, src_name, dst_vp, dst_name, epoch)` | |
| `truncate` | `vfs_write` (zero-fill to extend) | |
| `statfs` | StorageBackend `total_pages`, `page_size` | |
| `flush` | `vfs_flush` | Called on `close()` |
| `ioctl` | Custom routing (Workload 12.2) | |

### Acceptance
- [ ] `mkdir /mnt/vfs && ./vfs_fuse test.vfs /mnt/vfs` mounts successfully
- [ ] `ls /mnt/vfs` shows root directory (empty on fresh mount)
- [ ] `echo "hello" > /mnt/vfs/test.txt` creates file
- [ ] `cat /mnt/vfs/test.txt` returns "hello"
- [ ] `rm /mnt/vfs/test.txt` deletes file
- [ ] `mkdir /mnt/vfs/sub && echo "x" > /mnt/vfs/sub/file.txt` works across subdirectories
- [ ] `fusermount -u /mnt/vfs` unmounts cleanly

---

## Workload 12.2 — Custom ioctl Commands

### What
Expose snapshot, commit, soft-delete, and GC via FUSE ioctl. A companion CLI
tool `vfsctl` sends these ioctls to the mount point.

### ioctl Command Definitions

```c
#define VFS_IOC_MAGIC   0x56
#define VFS_IOC_SNAPSHOT       _IO(VFS_IOC_MAGIC, 1)
#define VFS_IOC_COMMIT         _IOW(VFS_IOC_MAGIC, 2, int64_t)
#define VFS_IOC_DELETE_SNAP    _IOW(VFS_IOC_MAGIC, 3, int64_t)
#define VFS_IOC_GC             _IO(VFS_IOC_MAGIC, 4)
```

### FUSE ioctl Callback

```c
int vfs_fuse_ioctl(const char* path, int cmd, void* arg,
                   struct fuse_file_info* fi, unsigned int flags, void* data) {
    (void)path; (void)fi; (void)flags;
    switch (cmd) {
        case VFS_IOC_SNAPSHOT: {
            int64_t ep = vfs_snapshot(vfs);
            if (ep < 0) return -EIO;
            *(int64_t*)data = ep;
            return 0;
        }
        case VFS_IOC_COMMIT: {
            int64_t ep = *(int64_t*)data;
            int r = vfs_commit(vfs, ep);
            return r == VFS_OK ? 0 : (r == VFS_ERR_CONFLICT ? -EBUSY : -EIO);
        }
        case VFS_IOC_DELETE_SNAP: {
            int64_t ep = *(int64_t*)data;
            int r = vfs_delete_snapshot(vfs, ep);
            return r == VFS_OK ? 0 : -EIO;
        }
        case VFS_IOC_GC: {
            int r = vfs_gc(vfs);
            return r == VFS_OK ? 0 : -EIO;
        }
    }
    return -ENOTTY;
}
```

### vfsctl CLI Tool

```
vfsctl snapshot          → ioctl(VFS_IOC_SNAPSHOT)    → prints snapshot epoch
vfsctl commit N          → ioctl(VFS_IOC_COMMIT, N)   → prints "ok" or "conflict"
vfsctl delete-snapshot N → ioctl(VFS_IOC_DELETE_SNAP, N)
vfsctl gc                → ioctl(VFS_IOC_GC)
```

### Acceptance
- [ ] `vfsctl snapshot` returns odd epoch
- [ ] `vfsctl commit 1` succeeds on clean snapshot
- [ ] `vfsctl delete-snapshot 3` soft-deletes
- [ ] `vfsctl gc` completes without error
- [ ] After commit: snapshot data visible via mount point read at correct epoch

---

## Workload 12.3 — Mount Options

### What
Parse FUSE mount options passed via `-o key=value`.

### Supported Options

| Option | Default | Meaning |
|--------|---------|---------|
| `epoch=N` | -1 | Read epoch for this mount. -1 = live head |
| `page_size=N` | 8192 | Passed to `vfs_mount` on mount |
| `readonly` | off | Mount read-only — all writes rejected |
| `allow_other` | off | Allow other users to access the mount |

### Acceptance
- [ ] `-o epoch=3` mounts at snapshot epoch 3; reads return data from that epoch
- [ ] `-o readonly` rejects writes with `EROFS`
- [ ] `-o page_size=16384` creates a 16KB page_size file

---

## Workload 12.4 — Integration Tests

### What
Shell-script tests that exercise the FUSE mount through standard POSIX commands.

### Test Cases

```
test_fuse_create_read_delete:
    ./vfs_fuse test.vfs /tmp/mnt &
    sleep 1
    echo "data" > /tmp/mnt/file.txt
    [ "$(cat /tmp/mnt/file.txt)" = "data" ]
    rm /tmp/mnt/file.txt
    [ ! -f /tmp/mnt/file.txt ]
    fusermount -u /tmp/mnt

test_fuse_snapshot:
    ./vfs_fuse test.vfs /tmp/mnt -o epoch=-1 &
    sleep 1
    echo "base" > /tmp/mnt/f.txt
    SNAP=$(vfsctl snapshot)
    echo "modified" > /tmp/mnt/f.txt
    fusermount -u /tmp/mnt
    
    ./vfs_fuse test.vfs /tmp/mnt2 -o epoch=$SNAP &
    sleep 1
    [ "$(cat /tmp/mnt2/f.txt)" = "base" ]  # snapshot still sees original
    fusermount -u /tmp/mnt2
```

### Acceptance
- [ ] All shell tests pass
- [ ] Data written via FUSE survives remount (no FUSE, raw VFS reopen)
- [ ] Snapshot isolation works through FUSE mounts at different epochs
