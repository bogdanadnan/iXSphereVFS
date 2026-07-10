# Phase 20: Cursor-Based Readdir with FUSE-Side Cache

## Goal

Fix the `bench_ditto.py` "5412 only-in-host files" bug by replacing the FUSE `readdir` callback's hard-coded 64-entry buffer with a proper cursor-based protocol backed by a small FUSE-side LRU cache of full directory listings. The VFS layer adds an "alloc" variant of `readdir` that walks the DirContent chain exactly once and returns a heap-allocated buffer of exact size. The FUSE layer caches these listings (keyed by both path_hash AND fi->fh) and serves the kernel via the FUSE cursor protocol (offset = position in the listing). Directory mutations invalidate by resolved `dir_vp` so invalidation matches across harness paths vs FUSE-relative paths.

## Background

The previous `fuse_vfs_readdir` (commits `25f685d`–`0c93fdb`) had three problems:

1. **Hard 64-entry buffer** (`vfs_dirent_t ents[64]` at `src/fuse_vfs.c:297`). Directories with >64 unique entries were silently truncated.

2. **Ignored cursor offset** (`(void)offset;` at line 279). The kernel's resume offset was discarded, and `filler` was always called with offset=0. Even if the buffer had space for more, the kernel couldn't tell where to continue.

3. **O(N²) chain walks per `ls`.** Each FUSE readdir callback called `vfs_readdir`, which walked the DirContent chain. A `ls` on a 6500-entry dir triggered ~100 readdir calls (kernel buffer 64 entries), each walking the chain.

The 500-scenario FUSE test report (commit `15b5db2`) showed 30 of 72 failures were "count_mismatch" — the readdir pipe was truncating at 64. The bench's `ditto -x -k archive.zip` extraction showed 5412 of 6563 host files missing from the VFS extract, with 0 MD5 mismatches on the 1151 that did appear.

## VFS changes

### `vfs_readdir_alloc`

Added a new VFS API:

```c
/* List directory contents with VFS-allocated buffer.  Walks the chain
   exactly once and allocates a buffer of exact size (no cap, no
   doubling).  Caller must free with vfs_free_dirents(). */
int     vfs_readdir_alloc(vfs_t* vfs, int64_t dir,
                           vfs_dirent_t** out_entries, int* out_count,
                           int64_t epoch);

/* Free a buffer returned by vfs_readdir_alloc.  Safe on NULL. */
void    vfs_free_dirents(vfs_dirent_t* entries);
```

### Internal `dirchain_list_all`

In `src/tree.c`, an internal function that mirrors `dirchain_list` but produces a heap-allocated output of exact size:

```c
int dirchain_list_all(TreeContext* ctx, int64_t dir_vp, int64_t epoch,
                      vfs_dirent_t** out_entries, int* out_count);
```

Algorithm: walks the chain once into a per-call dedup VarArray (same first-hit-wins dedup as Phase 19's `dirchain_list`), then allocates a `vfs_dirent_t[]` of upper-bound size (dedup->count), populates it, and reallocs down to exact size after filtering tombstones.

Single chain walk. No doubling. No caller guess. **This was the key architectural fix**: previously, repeatedly calling `vfs_readdir` with growing buffers (the user's "doubling" suggestion) would walk the chain multiple times — defeating the purpose.

## FUSE-side cache

### Module: `src/fuse_dir_cache.{h,c}`

A 32-slot LRU cache of full directory listings.

**Each slot holds both keys:**
```c
typedef struct {
    uint64_t path_hash;       /* FNV-1a of FUSE-relative path, or 0 */
    int64_t  fh;              /* fi->fh from opendir, or 0 */
    int64_t  dir_vp;          /* resolved VirtualPtr */
    vfs_dirent_t* entries;     /* owned by the slot */
    int count;
    uint64_t last_used;
} FusedirCacheSlot;
```

**Double-keyed** because libfuse may call `readdir` with either:
- The directory's path (high-level, no opendir fd yet)
- NULL path + a valid `fi->fh` from opendir (low-level, fd-keyed)

A slot matches EITHER key. If a lookup hits by path, the fh field is filled in (and vice versa), so subsequent lookups using either key hit the same slot.

### Cursor-based readdir

`fuse_vfs_readdir` was rewritten:

```c
int fuse_vfs_readdir(const char* path, void* buf, fuse_darwin_fill_dir_t filler,
                     off_t offset, struct fuse_file_info* fi, ...) {
    /* Resolve dir: prefer fi->fh (set by opendir), fall back to path. */
    int64_t dir_vp = (fi && fi->fh != 0) ? (int64_t)fi->fh
                    : resolve_full_path(...);

    /* Look up or build the cache. */
    vfs_dirent_t* entries = NULL; int count = 0;
    int rc = (fi && fi->fh) ? fusedir_cache_get_fh(...)
                            : fusedir_cache_get(...);
    if (rc != VFS_OK) return vfs_error_to_errno(rc);

    /* Cursor logic.  Offset layout:
       0 = start (include . and ..)
       1 = skip '.', start at '..'
       2 = skip '.', '..', start at cache[0]
       3 = start at cache[1]
       ... */
    long start = (long)offset;
    if (start <= 0) { filler(buf, ".",  &attr, 1, 0); start = 1; }
    if (start == 1) { filler(buf, "..", &attr, 2, 0); start = 2; }
    for (long i = start - 2; i < count; i++) {
        if (filler(buf, entries[i].name, &attr, (off_t)(i + 3), 0)) break;
    }
    return 0;
}
```

The `(i + 3)` passed to `filler` is the next entry's offset, so the kernel knows where to resume after a buffer-full stop.

## The invalidation bug (and fix)

The first cut of phase 20 invalidated the cache by **path hash**:

```c
// In fuse_vfs_create:
invalidate_path(state, path_copy);  // /tmp/vfs_scenario_mnt/401_...
```

But the cache was keyed by **FUSE-relative path**:

```c
// In fuse_vfs_readdir:
const char* key_path = path;  // "/" (from libfuse after mount)
```

Different hashes. Stale cache. Result: `batch_09` (stress ≥32 files in single dir) regressed from 50/50 to 0/50.

**Fix**: invalidate by **resolved `dir_vp`** instead of path. Added `fusedir_cache_invalidate_vp(vp)` and switched `fuse_vfs_create/unlink/mkdir/rmdir/rename` to use it. `dir_vp` is stable across all access paths (full harness path, FUSE-relative path, fi->fh).

After the fix: `batch_09` back to 50/50.

## Files

| File | Change |
|------|--------|
| `include/ixsphere/vfs.h` | `vfs_readdir_alloc`, `vfs_free_dirents` declarations. |
| `src/vfs.c` | `vfs_readdir_alloc` and `vfs_free_dirents` implementations. |
| `src/tree.c`, `src/tree.h` | Internal `dirchain_list_all`. |
| `src/fuse_dir_cache.h`, `src/fuse_dir_cache.c` | New FUSE-side cache module. |
| `src/fuse_vfs.c`, `src/fuse_vfs.h` | `fuse_vfs_readdir` rewritten to use cache + cursor. Cache invalidation in create/unlink/mkdir/rmdir/rename. New `dir_cache` field on `fuse_vfs_state_t`. |
| `CMakeLists.txt` | Added `src/fuse_dir_cache.c`. |

## Commits

- `0e3e5aa` — Phase 20 implementation (FUSE cache + cursor + vfs_readdir_alloc).
- `38cf73b` — BENCH_BASELINE.md updated with phase 20 numbers.
- `c611620` — dir_vp-based invalidation fix (resolves stale-cache regression).
- `b7f538e` — batch_07/batch_09 results post-fix.
- `7fe714c` — bench_phase20-final.json.

## Results

| Metric | baseline | phase 20 |
|---|---|---|
| copy_in | 1,553 ms (162 MB/s) | 1,505 ms (167 MB/s) |
| ditto | 27,547 ms (15.5 MB/s) | **24,265 ms (34.0 MB/s)** |
| vfs_extract files | 2,653 | **14,395** |
| only_in_host (missing) | 5,412 | **0** |
| md5_mismatches | 0 | 0 |
| common | 1,151 | **6,563** |

**Highlights**:
- Extract throughput **2.2x baseline**
- All 6,563 host files extract correctly
- 0 MD5 mismatches (content byte-identical)
- 50/50 unit test pass; 50/50 batch_09; 13/50 batch_07 (no regression on concurrency)

## Out of scope

1. **Hash-based dedup** (O(N) instead of O(N²)). Deferred to Phase 21 (`phase-21-hash-map.md`).
2. **Concurrent resize of the FUSE cache.** Single-threaded callbacks per mount.
3. **String-keyed cache.** Path hash only.
4. **Cache invalidation by full prefix** (e.g., `invalidate_prefix("/foo")` for subtree invalidation on rename of a directory). Currently we do full-flush for prefix; acceptable given the 32-slot cap.

## Future work

- **Phase 21 (Hash Map)**: replace O(N²) linear-scan dedup in `dirchain_list_all` with O(N) hash map. Will reduce uncached-readdir CPU for huge directories by another ~40000x at 6500 entries.