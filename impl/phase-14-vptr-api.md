# Phase 14: VirtualPtr API — Fix nodeId/VirtualPtr Mismatch

## Goal
Eliminate the `resolve_child_vp` pattern that walks the DirContent chain after
every create. `vfs_create` and `vfs_mkdir` return the child's FileNode/DirNode
VirtualPtr directly, which is what `vfs_write`/`vfs_read`/`vfs_file_size`
already expect. No API caller ever needs a nodeId except for directory listing
identity tracking across renames.

## Current State

```c
int vfs_create(vfs, parent, name, epoch) → nodeId (1, 2, 3...)
```

Every benchmark and test then does:
```c
int nid = vfs_create(vfs, root, name, 0);
int64_t file_vp = resolve_child_vp(vfs, root, name);  // O(N) chain walk!
vfs_write(vfs, file_vp, data, ...);
```

The `resolve_child_vp` walks the parent's DirContent chain comparing names —
the same chain `vfs_create` just walked to check for name collisions. Two walks
per create. At 5,000 files, the 5,000th create walks ~5,000 entries twice.

## Proposed State

```c
int64_t vfs_create(vfs, parent, name, epoch) → VirtualPtr of FileNode
int64_t vfs_mkdir(vfs, parent, name, epoch)  → VirtualPtr of DirNode
```

On success: returns the child's VirtualPtr (always > 0).
On failure: returns negative error code (`VFS_ERR_EXISTS`, `VFS_ERR_NOTDIR`, etc.).

Callers become:
```c
int64_t fvp = vfs_create(vfs, root, "file.txt", 0);
if (fvp < 0) { /* error */ }
vfs_write(vfs, fvp, data, ...);
```

No `resolve_child_vp` call. No second chain walk.

## `vfs_dirent_t` Update

```c
typedef struct {
    int64_t vp;       // NEW: VirtualPtr to child (DirNode or FileNode)
    int64_t nodeId;   // unchanged: for identity tracking across renames
    char    name[256];
    bool    isDir;
} vfs_dirent_t;
```

The `vp` field is populated during `vfs_readdir` from the DirContent's
`childPtr` — already available in the chain walk, no extra cost.

## Files Changed

| File | Change |
|------|--------|
| `include/ixsphere/vfs.h` | Return types: `int` → `int64_t` for create, mkdir |
| `src/tree.c` | `vfs_create`/`vfs_mkdir`: return `file_vp`/`dir_vp` instead of `new_nodeId` |
| `bench/bench.c` | Remove all `resolve_child_vp` calls. Use VirtualPtrs directly. |
| `test/test_tree.c` | Same — remove chain-walk workarounds |
| `test/test_epoch.c` | Update create/mkdir call sites |
| `test/test_gc.c` | Update create/mkdir call sites |

## Non-Negotiable Constraints

- **No public API caller ever calls `resolve_child_vp` again.** The function
  becomes internal-only (or removed entirely).
- **NodeId remains in `vfs_dirent_t`** for identity tracking. It is NOT removed.
- **All error paths return negative values.** Callers check `if (ret < 0)` for
  errors and `if (ret > 0)` for success. Zero is never returned.
- **All existing tests must pass** with minimal changes (remove `resolve_child_vp`
  calls, use VirtualPtrs directly).

## Acceptance

- [x] `vfs_create` returns VirtualPtr, usable directly with `vfs_write`/`vfs_read`
- [x] `vfs_mkdir` returns VirtualPtr, usable directly with `vfs_rmdir`/`vfs_readdir`
- [x] `resolve_child_vp` removed from bench and tests
- [x] O(N) DirContent walk after create eliminated
- [x] Small file write benchmark: ops/sec improves by >10× at 5,000 files
- [x] All 8 tests pass
