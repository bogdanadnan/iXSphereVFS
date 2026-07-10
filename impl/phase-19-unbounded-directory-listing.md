# Phase 19: Unbounded Directory Listing

## Goal

Replace the fixed-size dedup arrays in `dirchain_list` (and the equivalent logic in `dentry_cache_build`) with a heap-backed, unbounded VarArray. The 1024-entry hard cap disappears, the ~36KB stack frame shrinks to ~256B, and the dedup algorithm simplifies to "first-hit-wins" based on the chain-ordering invariant.

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

The current `dentry_cache_build` (`src/dentry_cache.c:6-114`) tracks **4 parallel arrays of size 1024** (no namePtr — the function does a second chain walk to look up names):

```c
int64_t best_child[1024];
int64_t best_childPtr[1024];
int64_t best_effective_epoch[1024];
int     best_name_set[1024];
```

Both have a 1024-entry cap. Both walk the chain to dedup by childNodeId. The dedup loop in both:

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
2. **Stack blowup.** `dirchain_list`: 5 × 8KB = ~36KB on stack per call. `dentry_cache_build`: 4 × 8KB + 4 × 4KB = ~36KB. Recursive GC and deep traversal can overflow.
3. **Unnecessary epoch comparison.** Because the chain is descending, the first applicable hit for a childNodeId is by definition the highest-epoch applicable record. The `eff_epoch > best_eff_epoch[found]` branch is defensive code that never fires.
4. **Dead code.** `dentry_cache_build` is **never called** anywhere in the codebase (verified by grep). It's an unimplemented hook. The invalidation calls in `tree.c` reference `ctx->readdir_cache` (a `DentryCache` field) but nothing populates it. We must decide: wire it up, or delete the dead code.
5. **Second chain walk in `dentry_cache_build`.** Lines 84-107 do a full second walk of the chain to look up the name string for each entry. This is O(N) per entry, so the total is O(N²) for the second pass. We can eliminate it by tracking `namePtr` in the first pass.

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

The linear scan for "already seen" is **O(N²)** total. For typical directories (<100 children) this is <10K ops and runs in microseconds. The dentry cache amortizes the cost after the first readdir. This is **the same complexity as the current code** — no regression. A future phase could add a hash table for O(N) lookup.

### Updated algorithm for `dentry_cache_build`

Two changes vs the current implementation:
1. Replace the 4 parallel arrays with a VarArray of `DirchainDedupEntry` (drops the 1024 cap).
2. **Track `namePtr` in the first pass** (eliminates the second chain walk — saves O(N²) for the second pass).

```c
int dentry_cache_build(Pool* pool, MapperTable* mapper_table,
                       int64_t root_vp, int64_t epoch, DentryCache* arr) {
    /* ... resolve dir_slot, read_epoch, headPtr ... */

    arr->last_headPtr_page = VFS_VPTR_PAGE(headPtr);
    arr->count = 0;
    int64_t read_epoch = mapper_table_resolve(mapper_table, epoch);

    VarArray(DirchainDedupEntry) dedup = var_array_new(DirchainDedupEntry);

    int64_t walk_vp = headPtr;
    while (walk_vp != 0) {
        /* ... read ce_child, ce_epoch, ce_childPtr, ce_namePtr, ce_next ... */
        /* ... compute eff_epoch, apply read rule ... */

        /* Linear scan for "already seen" (O(N²) total, same as before) */
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

    /* Populate DentryCache from dedup.  No second chain walk — namePtr
       is already in the dedup entry.  arr->count < DENTRY_CACHE_MAX
       remains as a sanity check (the in-memory cache is fixed-size; the
       dedup is unbounded, so if a directory has more than 1024 entries
       the cache will only hold the first 1024). */
    for (int i = 0; i < dedup->count && arr->count < DENTRY_CACHE_MAX; i++) {
        DirchainDedupEntry* e = var_array_lookup(dedup, i);
        if (!e || !e->name_set) continue;

        DentryEntry* out = &arr->entries[arr->count++];
        out->childNodeId = e->childNodeId;
        out->childPtr = e->childPtr;
        out->isDir = false;
        out->name[0] = '\0';

        uint8_t* child_slot = pool_resolve_ro(pool, e->childPtr);
        if (child_slot) {
            int16_t type = vfs_rd2_s(child_slot, DIRNODE_OFF_TYPE,
                                     pool->sb->page_size);
            out->isDir = (type == (int16_t)NODE_TYPE_DIR);
        }
        if (e->namePtr != 0)
            nodes_read_name(pool, e->namePtr, out->name,
                            (int)sizeof(out->name));
    }

    var_array_delete(dedup);
    arr->valid = true;
    return VFS_OK;
}
```

### Dead code decision

`dentry_cache_build` is currently dead code — declared, defined, but never called. Only the invalidation hook (`dentry_cache_invalidate`) is wired up. The `DentryCache readdir_cache` field on `TreeContext` is invalidated by every directory mutation but never populated.

This phase keeps the dead code (option 3: defer wiring). Rationale:

- The cap removal is the user-visible win and is independent of whether `dentry_cache_build` is called.
- Wiring up the cache requires more design decisions (per-directory vs single shared cache, what happens when headPtr changes, when to invalidate beyond the headPtr-page check). These are out of scope for this phase.
- The struct layout is small (~280KB on TreeContext) and removing it is a separate refactor.

A follow-up phase could wire up the cache. For now, we make `dentry_cache_build` correct and bounded only by `DentryCache.entries[DENTRY_CACHE_MAX]` (the in-memory cache size, not the dedup VarArray's unbounded capacity).

## Files

| File | Change |
|------|--------|
| `src/tree.h` | Declare `DirchainDedupEntry` struct (shared between `tree.c` and `dentry_cache.c`). |
| `src/tree.c` | `dirchain_list`: replace 5 parallel arrays with `VarArray(DirchainDedupEntry)`, drop `eff_epoch` tracking, use typed `var_array_new`/`var_array_append`/`var_array_lookup`/`var_array_delete` macros. |
| `src/dentry_cache.c` | `dentry_cache_build`: replace 4 parallel arrays with `VarArray(DirchainDedupEntry)`, **track `namePtr` in first pass** to eliminate 2nd chain walk. |
| `src/dentry_cache.h` | `DENTRY_CACHE_MAX` keeps its role as the in-memory `DentryCache.entries` cap (sanity check). |

## Performance budget

| Metric | Current (capped) | After phase 19 |
|---|---|---|
| First readdir of dir with N children | O(min(N², 1024²)) | O(N²) |
| First readdir of dir with >1024 children | silently truncated | O(N²) full list |
| Stack frame of `dirchain_list` | ~36KB | ~256B |
| Per-call malloc | 0 | 1 (8KB per 256 entries) |
| Per-call free | 0 | 1 |

For N=6500 (the bench_ditto extraction): N² = 42M ops, ~0.4s. The dentry cache amortizes this after the first readdir, so subsequent reads are O(1). This is comparable to the current code's performance for N≤1024.

## Concurrency

`dirchain_list` is called from a single FUSE callback at a time per mount (FUSE user-space callbacks are serialized per mount). Per-call alloc/free is safe — no concurrent access to the dedup VarArray.

If we want per-thread reuse later (to avoid the malloc/free per readdir), we can use a `_Thread_local VarArrayBase*` reset to count=0 on each entry. Deferred.

## API surface

No new public API. Internal change to dedup storage. `DentryCache` is removed (option 2) or kept but with different population code (option 1).

## Acceptance

- [ ] `dirchain_list` no longer truncates at 1024 children.
- [ ] `dentry_cache_build` no longer truncates at 1024 children (or is deleted entirely).
- [ ] Stack frame of `dirchain_list` reduced from ~36KB to ~256B (just the VarArray handle).
- [ ] `DirchainDedupEntry` struct has 4 fields (no `eff_epoch`).
- [ ] `DENTRY_CACHE_MAX` removed from `dentry_cache.h` (or repurposed).
- [ ] All 144+ epoch tests still pass (the new tests we added for read-rule verification).
- [ ] `test_tree`, `test_crash`, `test_fuzz`, `test_gc`, `test_mapper`, `test_nodes`, `test_pool`, `test_storage`, `test_var_array` all pass.
- [ ] 500 FUSE test scenarios: no regression (same 72 fails, same 428 passes as before — the FUSE 64-cap and other issues are out of scope here).
- [ ] `bench_ditto.py` baseline numbers don't degrade (we expect they improve or stay the same since the 1024 cap is what was silently truncating).

## Out of scope (explicit non-goals)

1. **FUSE 64-cap** in `fuse_vfs.c:297-298`. This is a separate bottleneck for the 500-scenario test failures and the bench_ditto missing-files bug. The 1024-cap removal here doesn't fix the 64-cap.
2. **Hash-based "seen" set** for O(N) instead of O(N²). For typical directories (<100 children) the O(N²) is fine. The dentry cache amortizes the cost. A future phase could add a hash table if profiling shows O(N²) is a real bottleneck.
3. **Per-thread scratch VarArray** for allocation reuse. Per-call malloc is simple and correct. A future phase could optimize if needed.
4. **Read rule changes.** The current implementation is correct. Verified by the existing read-rule tests.
5. **`dentry_cache_build` cache wiring.** Per the dead-code decision, we delete the cache entirely. A future phase could reintroduce it with cleaner semantics.

## Implementation order

1. Declare `DirchainDedupEntry` in `src/tree.h`.
2. Update `dirchain_list` in `src/tree.c` to use the new struct + VarArray (typed macros).
3. Update `dentry_cache_build` in `src/dentry_cache.c` to use the new struct + VarArray + namePtr tracking (eliminates the 2nd chain walk).
4. Build and run all unit tests. Verify 144/144 epoch, 958/958 tree, etc.
5. Re-run `bench_ditto.py` to verify the 5412 only-in-host count doesn't regress (and ideally improves).
6. Re-run 500-scenario FUSE tests to verify no regression.

## Risks

- **Memory leaks.** The new code does `var_array_new(DirchainDedupEntry)` at function entry and `var_array_delete(dedup)` before return. If a return is added later that bypasses the cleanup, we leak. To minimize risk, structure the function so the dedup var_array is created early (after input validation) and freed in a single cleanup path at the end. A `goto cleanup` pattern is idiomatic for this.
- **Per-call malloc overhead.** For a benchmark that does many readdirs, the malloc adds up. For the FUSE readdir path (called once per ls), the overhead is negligible. A future phase could add per-thread scratch space.
- **Dropping `eff_epoch` from dedup output.** This is a code-correctness change, not a perf change. The existing tests must pass to verify. If any test fails, we know the simplification is wrong.
- **`var_array_lookup` may return NULL.** The lookup macro returns NULL if the slot was never written (count > idx) or if the resolve_base walks an empty slot. In our algorithm, we always append-then-lookup, so the slot is always written. But the macro's NULL return is defensive — we should check for NULL to avoid crashes from missed CAS races during grow.
- **`var_array_append` returns the index, not the entry pointer.** If we need both, we use `var_array_lookup(dedup, idx)` to get the pointer. We don't store the pointer from `var_array_append` because the macro doesn't return one.
