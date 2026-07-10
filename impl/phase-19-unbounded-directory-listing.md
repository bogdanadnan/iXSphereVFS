# Phase 19: Unbounded Directory Listing

## Goal

Replace the fixed-size dedup array in `dirchain_list` with a heap-backed, unbounded VarArray. The 1024-entry hard cap disappears, the ~36KB stack frame shrinks to ~256B, and the dedup algorithm simplifies to "first-hit-wins" based on the chain-ordering invariant.

This phase **also deletes the dead `dentry_cache` infrastructure** that was planned but never wired up — see the "Dead code removal" section.

## Background

A directory's `DirContent` chain is prepended on every mutation, so the chain is in **descending epoch order** (newest record at front, oldest at tail). Combined with the read rule (`SPEC.md` §7.2), the highest-epoch applicable record for each childNodeId appears **first** in the chain walk.

The current `dirchain_list` (`src/tree.c:1204-1288`) tracks all applicable records per childNodeId in **5 parallel arrays of size 1024**:

```c
int64_t best_child[1024];
int64_t best_childPtr[1024];
int64_t best_eff_epoch[1024];
int     best_name_set[1024];
int64_t best_namePtr[1024];
```

```c
for (i = 0; i < best_count; i++) {
    if (best_child[i] == ce_child) { found = i; break; }
}
if (found >= 0) {
    if (eff_epoch > best_eff_epoch[found]) {  // defensive, never fires
        /* overwrite */
    }
} else {
    /* append */
}
```

## Problems with the current code

1. **Hard cap.** `while (walk_vp != 0 && best_count < DENTRY_CACHE_MAX)` — directories with >1024 unique children are silently truncated.
2. **Stack blowup.** `dirchain_list`: 5 × 8KB = ~36KB on stack per call. Recursive GC and deep traversal can overflow.
3. **Unnecessary epoch comparison.** Because the chain is descending, the first applicable hit for a childNodeId is by definition the highest-epoch applicable record. The `eff_epoch > best_eff_epoch[found]` branch is defensive code that never fires.

## Dead code removal

The codebase has dead-code infrastructure around `dentry_cache`:

- `src/dentry_cache.h` and `src/dentry_cache.c` declare/define `DentryEntry`, `DentryCache`, `DENTRY_CACHE_MAX`, `dentry_cache_build`, `dentry_cache_is_valid`, `dentry_cache_invalidate`.
- `include/ixsphere/vfs_internal.h` declares `DentryCache readdir_cache` as a field on `TreeContext`.
- `src/tree.c` calls `dentry_cache_invalidate(&ctx->readdir_cache)` 6 times (at every create/delete/rename/rmdir).
- **No code calls `dentry_cache_build`.** The cache is invalidated on every mutation but never populated. The struct is allocated on every `TreeContext` (280KB) and never used.

This phase deletes all of it:
- Removes `src/dentry_cache.h` and `src/dentry_cache.c`.
- Removes the `#include "dentry_cache.h"` from `vfs_internal.h`.
- Removes the `readdir_cache` field from `TreeContext`.
- Removes all 6 `dentry_cache_invalidate` calls from `tree.c`.
- Removes `src/dentry_cache.c` from `CMakeLists.txt`.

`DENTRY_CACHE_MAX` is removed entirely. The cap that was 1024 in the dedup array goes away (replaced by VarArray, no upper bound).

## Read rule (verified correct)

The read rule from `SPEC.md` §7.2 has 4 cases (5, 6, 7, 8). The current implementation in `tree.c:1237-1238` and `dentry_cache.c:42-43` is:

```c
int applies = (eff_epoch == read_epoch) ||
              (eff_epoch < read_epoch && eff_epoch % 2 == 0);
if (!applies) { walk_vp = ce_next; continue; }
```

This expression correctly handles all 4 cases:
- Case 5 (`== R'`): `applies = true`, take it.
- Case 6 (`> R'`): both conditions fail, `applies = false`, skip.
- Case 7 (`< R' && even`): second condition true, `applies = true`, take it.
- Case 8 (`< R' && odd`): second condition fails (odd), `applies = false`, skip.

**The read rule is already correct.** Verified by `test_snapshot_write_readrule` and `test_rename_across_snapshots` in `test/test_epoch.c`. No changes needed.

The earlier concern that "case 6 is missing" was a misreading of the code — case 6 is handled implicitly by the boolean expression failing. We will NOT change the read rule in this phase.

## Design

### Dedup entry struct

```c
typedef struct {
    int64_t childNodeId;     /* unique per-directory identifier */
    int64_t childPtr;        /* VirtualPtr to child FileNode or DirNode */
    int     name_set;        /* 1 = live entry, 0 = tombstone */
    int64_t namePtr;         /* VirtualPtr to cached NameEntry (0 if tombstone) */
} DirchainDedupEntry;
```

`eff_epoch` is **not** tracked per-entry. The chain ordering guarantees the first applicable hit is the correct one. We verify this is safe via the existing read-rule tests.

Layout: 4 fields, 32 bytes. 256 entries per VarArray chunk = 8KB chunk. Same as the existing `vfs_dirent_t` size (280 bytes / 64 entries ≈ 4.4 bytes per entry; 32 bytes per dedup entry is much smaller).

### VarArray usage

The codebase pattern is to use the typed macros, not the base functions:

```c
#include "var_array.h"

VarArray(DirchainDedupEntry) dedup = var_array_new(DirchainDedupEntry);

DirchainDedupEntry e = { .childNodeId = ce_child, .childPtr = ce_childPtr,
                          .name_set = (ce_namePtr != 0), .namePtr = ce_namePtr };
int idx = var_array_append(dedup, e);

DirchainDedupEntry* found = var_array_lookup(dedup, idx);
if (found) printf("child=%lld namePtr=%lld\n",
                  (long long)found->childNodeId, (long long)found->namePtr);

// Update in place (not used in our algorithm — append-only)
e.name_set = 0;
var_array_update(dedup, idx, e);

var_array_delete(dedup);
```

The macros wrap the base functions (`var_array_new_base`, `var_array_grow_base`, `var_array_resolve_base`, `var_array_delete_base`) with type safety. They use GCC's statement-expression extension (`({ ... })`) for in-line composition.

### New algorithm for `dirchain_list`

```c
int dirchain_list(TreeContext* ctx, int64_t dir_vp, int64_t epoch,
                  vfs_dirent_t* entries, int max) {
    /* ... resolve dir_slot, read_epoch, headPtr ... */

    /* Per-call dedup: heap-backed, unbounded. */
    VarArray(DirchainDedupEntry) dedup = var_array_new(DirchainDedupEntry);

    int64_t walk_vp = headPtr;
    while (walk_vp != 0) {
        uint8_t* dc_slot = pool_resolve_ro(&ctx->pool, walk_vp);
        if (!dc_slot) break;
        uint32_t ce_child, ce_epoch;
        int64_t ce_childPtr, ce_namePtr, ce_next;
        nodes_read_dircontent(dc_slot, &ce_child, &ce_epoch, &ce_childPtr,
                              &ce_namePtr, &ce_next, ctx->page_size);

        int64_t eff_epoch = (int64_t)ce_epoch;
        if (mapper_table_traversal_apply(&ctx->mapper_table, (int64_t)ce_epoch))
            eff_epoch = mapper_table_resolve(&ctx->mapper_table, (int64_t)ce_epoch);

        int applies = (eff_epoch == read_epoch) ||
                      (eff_epoch < read_epoch && eff_epoch % 2 == 0);
        if (!applies) { walk_vp = ce_next; continue; }

        /* Linear scan for "already seen" — O(N) per scan, O(N²) total.
           The dedup array's count field is read on every iteration; the
           var_array append is reflected there by var_array_append's
           grow_base call. */
        int found = 0;
        for (int i = 0; i < dedup->count; i++) {
            DirchainDedupEntry* e = var_array_lookup(dedup, i);
            if (e && e->childNodeId == (int64_t)ce_child) { found = 1; break; }
        }
        if (found) { walk_vp = ce_next; continue; }

        DirchainDedupEntry entry = {
            .childNodeId = (int64_t)ce_child,
            .childPtr    = ce_childPtr,
            .name_set    = (ce_namePtr != 0),
            .namePtr     = ce_namePtr,
        };
        (void)var_array_append(dedup, entry);
        walk_vp = ce_next;
    }

    /* Build output, skipping tombstones.  Limit by `max` (FUSE-side cap
       is 64; full dedup may be larger). */
    int written = 0;
    for (int i = 0; i < dedup->count && written < max; i++) {
        DirchainDedupEntry* e = var_array_lookup(dedup, i);
        if (!e || !e->name_set) continue;

        entries[written].vp     = e->childPtr;
        entries[written].nodeId = e->childNodeId;
        entries[written].name[0] = '\0';
        entries[written].isDir = false;

        uint8_t* child_slot = pool_resolve_ro(&ctx->pool, e->childPtr);
        if (child_slot) {
            int16_t ctype = vfs_rd2_s(child_slot, DIRNODE_OFF_TYPE,
                                       ctx->page_size);
            entries[written].isDir = (ctype == (int16_t)NODE_TYPE_DIR);
        }
        if (e->namePtr != 0)
            nodes_read_name(&ctx->pool, e->namePtr,
                            entries[written].name,
                            (int)sizeof(entries[written].name));
        written++;
    }

    var_array_delete(dedup);
    return written;
}
```

The linear scan for "already seen" is **O(N²)** total. For typical directories (<100 children) this is <10K ops and runs in microseconds. A future phase could add a hash table for O(N) lookup.

## Files

| File | Change |
|------|--------|
| `src/tree.h` | Declare `DirchainDedupEntry` struct. |
| `src/tree.c` | `dirchain_list`: replace 5 parallel arrays with `VarArray(DirchainDedupEntry)`, drop `eff_epoch` tracking, use typed `var_array_new`/`var_array_append`/`var_array_lookup`/`var_array_delete` macros. Remove all 6 `dentry_cache_invalidate` calls. |
| `src/dentry_cache.h` | **Delete entire file.** |
| `src/dentry_cache.c` | **Delete entire file.** |
| `include/ixsphere/vfs_internal.h` | Remove `#include "dentry_cache.h"`. Remove `DentryCache readdir_cache` field from `TreeContext`. |
| `CMakeLists.txt` | Remove `src/dentry_cache.c` from the source list. |

## Performance budget

| Metric | Current (capped) | After phase 19 |
|---|---|---|
| First readdir of dir with N children | O(min(N², 1024²)) | O(N²) |
| First readdir of dir with >1024 children | silently truncated | O(N²) full list |
| Stack frame of `dirchain_list` | ~36KB | ~256B |
| Per-call malloc | 0 | 1 (8KB per 256 entries) |
| Per-call free | 0 | 1 |
| `TreeContext` size | ~280KB (DentryCache field) | 0 (field removed) |

For N=6500 (the bench_ditto extraction): N² = 42M ops, ~0.4s. This is comparable to the current code's performance for N≤1024. The dedup is per-call with no caching — repeated readdirs of the same directory each do the full chain walk. If profiling shows this is a bottleneck, a future phase can introduce a per-directory cache.

## Concurrency

`dirchain_list` is called from a single FUSE callback at a time per mount (FUSE user-space callbacks are serialized per mount). Per-call alloc/free is safe — no concurrent access to the dedup VarArray.

If we want per-thread reuse later (to avoid the malloc/free per readdir), we can use a `_Thread_local VarArrayBase*` reset to count=0 on each entry. Deferred.

## API surface

No new public API. Internal change to dedup storage. `DentryCache`, `DentryEntry`, `dentry_cache_build`, `dentry_cache_is_valid`, `dentry_cache_invalidate`, and `DENTRY_CACHE_MAX` are all **removed**.

## Acceptance

- [ ] `dirchain_list` no longer truncates at 1024 children.
- [ ] `dentry_cache.h` and `dentry_cache.c` deleted.
- [ ] All `dentry_cache_invalidate` calls removed from `tree.c`.
- [ ] `readdir_cache` field removed from `TreeContext`.
- [ ] `DENTRY_CACHE_MAX` no longer exists.
- [ ] `src/dentry_cache.c` removed from `CMakeLists.txt`.
- [ ] Stack frame of `dirchain_list` reduced from ~36KB to ~256B.
- [ ] `DirchainDedupEntry` struct has 4 fields (no `eff_epoch`).
- [ ] All 144+ epoch tests still pass.
- [ ] `test_tree`, `test_crash`, `test_fuzz`, `test_gc`, `test_mapper`, `test_nodes`, `test_pool`, `test_storage`, `test_var_array` all pass.
- [ ] 500 FUSE test scenarios: no regression.
- [ ] `bench_ditto.py` baseline numbers don't degrade.

## Out of scope (explicit non-goals)

1. **FUSE 64-cap** in `fuse_vfs.c:297-298`. This is a separate bottleneck for the 500-scenario test failures and the bench_ditto missing-files bug. The 1024-cap removal here doesn't fix the 64-cap.
2. **Hash-based "seen" set** for O(N) instead of O(N²). For typical directories (<100 children) the O(N²) is fine. A future phase could add a hash table if profiling shows O(N²) is a real bottleneck.
3. **Per-thread scratch VarArray** for allocation reuse. Per-call malloc is simple and correct. A future phase could optimize if needed.
4. **Read rule changes.** The current implementation is correct. Verified by the existing read-rule tests.
5. **Re-introducing a directory readdir cache.** The deleted `dentry_cache` infrastructure could be brought back later with cleaner semantics (per-directory cache, better invalidation strategy). Out of scope for this phase.

## Implementation order

1. Declare `DirchainDedupEntry` in `src/tree.h`.
2. Update `dirchain_list` in `src/tree.c` to use the new struct + VarArray (typed macros).
3. Remove all 6 `dentry_cache_invalidate` calls from `tree.c`.
4. Remove `#include "dentry_cache.h"` and the `readdir_cache` field from `include/ixsphere/vfs_internal.h`.
5. Delete `src/dentry_cache.h` and `src/dentry_cache.c`.
6. Remove `src/dentry_cache.c` from `CMakeLists.txt`.
7. Build and run all unit tests. Verify 144/144 epoch, 958/958 tree, etc.
8. Re-run `bench_ditto.py` to verify the 5412 only-in-host count doesn't regress.
9. Re-run 500-scenario FUSE tests to verify no regression.

## Risks

- **Memory leaks.** The new code does `var_array_new(DirchainDedupEntry)` at function entry and `var_array_delete(dedup)` before return. If a return is added later that bypasses the cleanup, we leak. To minimize risk, structure the function so the dedup var_array is created early (after input validation) and freed in a single cleanup path at the end. A `goto cleanup` pattern is idiomatic for this.
- **Per-call malloc overhead.** For a benchmark that does many readdirs, the malloc adds up. For the FUSE readdir path (called once per ls), the overhead is negligible. A future phase could add per-thread scratch space.
- **Dropping `eff_epoch` from dedup output.** This is a code-correctness change, not a perf change. The existing tests must pass to verify. If any test fails, we know the simplification is wrong.
- **`var_array_lookup` may return NULL.** The lookup macro returns NULL if the slot was never written (count > idx) or if the resolve_base walks an empty slot. In our algorithm, we always append-then-lookup, so the slot is always written. But the macro's NULL return is defensive — we should check for NULL to avoid crashes from missed CAS races during grow.
- **`var_array_append` returns the index, not the entry pointer.** If we need both, we use `var_array_lookup(dedup, idx)` to get the pointer. We don't store the pointer from `var_array_append` because the macro doesn't return one.
