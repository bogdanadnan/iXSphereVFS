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

### New algorithm for `dirchain_list`

```
walk chain head-to-tail (descending epoch):
    read ce_child, ce_epoch, ce_childPtr, ce_namePtr, ce_next
    eff_epoch = mapper_table_resolve(ce_epoch)   # with traversalApply
    if !read_rule_applies(eff_epoch, read_epoch):
        continue
    linear scan dedup[] for childNodeId == ce_child
    if found:
        continue   # already have a higher-epoch applicable record
    var_array_append(dedup, DirchainDedupEntry{ce_child, ce_childPtr, namePtr!=0, namePtr})
    # No eff_epoch tracking. Chain order guarantees correctness.

output (limited by `max`):
    for entry in dedup:
        if !entry.name_set: continue   # skip tombstones
        entries[written++] = populate_vfs_dirent(entry)
    var_array_delete(dedup)
    return written
```

The linear scan for "already seen" is **O(N²)** total. For typical directories (<100 children) this is <10K ops and runs in microseconds. The dentry cache amortizes the cost after the first readdir. This is **the same complexity as the current code** — no regression. A future phase could add a hash table for O(N) lookup.

### Updated algorithm for `dentry_cache_build`

Two changes vs the current implementation:
1. Replace the 4 parallel arrays with a VarArray of `DirchainDedupEntry` (drops the 1024 cap).
2. **Track `namePtr` in the first pass** (eliminates the second chain walk — saves O(N²) for the second pass).
3. Track `eff_epoch` only enough to match the highest-epoch record (for the second-pass name lookup). With chain order, this is just the first applicable hit per childNodeId — same logic as `dirchain_list`.

```
walk chain head-to-tail:
    read ce_child, ce_epoch, ce_childPtr, ce_namePtr, ce_next
    eff_epoch = mapper_table_resolve(ce_epoch)
    if !read_rule_applies(eff_epoch, read_epoch): continue
    linear scan dedup[] for childNodeId == ce_child
    if found: continue
    var_array_append(dedup, {ce_child, ce_childPtr, namePtr!=0, ce_namePtr})

populate DentryCache from dedup (limit DENTRY_CACHE_MAX remains as a sanity check, not a hard cap):
    for entry in dedup:
        if !entry.name_set: continue
        DentryEntry* out = &arr->entries[arr->count++]
        out->childNodeId = entry.childNodeId
        out->childPtr = entry.childPtr
        nodes_read_name(&ctx->pool, entry.namePtr, out->name, sizeof(out->name))
        # Determine isDir by reading child's type field
```

`DentryCache` itself stays as-is (it's the in-memory array that lives across readdir calls for the same directory). The change is in the population function. The 1024 cap on `DentryCache.entries` becomes a sanity check (`arr->count < DENTRY_CACHE_MAX` becomes `< 1024 * 1024` or is removed entirely).

### Dead code decision

`dentry_cache_build` is currently dead code. Three options:

1. **Wire it up** — call it from `dirchain_list` (or the FUSE callback) to actually use the cache. Risk: a second chain walk, but with namePtr tracked the per-entry cost is just one `pool_resolve_ro`. Could speed up repeated readdirs.
2. **Delete it** — remove the dead code, simplify the codebase. The first-readdir cost goes back to a fresh chain walk each time, but the cap is removed anyway.
3. **Wire it up later** — phase 19 fixes the cap but doesn't wire the cache. A follow-up phase adds the cache hook.

**Recommendation: option 2 (delete).** The cache adds complexity, the dentry_cache struct lives on `TreeContext` (single shared cache, not per-directory), so it's not even clear how it would be useful. The cap removal is the main user-visible benefit. If we want caching later, a follow-up phase can introduce it cleanly.

If the user prefers option 1, the implementation is straightforward: add a call to `dentry_cache_build` in `dirchain_list` (under the right conditions) and use `dentry_cache_is_valid` to short-circuit. This is a separate concern from the cap removal.

## Files

| File | Change |
|------|--------|
| `src/tree.h` | Declare `DirchainDedupEntry` struct (shared between `tree.c` and `dentry_cache.c`). |
| `src/tree.c` | `dirchain_list`: replace 5 parallel arrays with `VarArray(DirchainDedupEntry)`, drop `eff_epoch` tracking, var_array allocated per-call, freed before return. |
| `src/dentry_cache.c` | `dentry_cache_build`: replace 4 parallel arrays with `VarArray(DirchainDedupEntry)`, track `namePtr` to eliminate 2nd chain walk, drop `eff_epoch` tracking. |
| `src/dentry_cache.h` | Remove or repurpose `DENTRY_CACHE_MAX`. If we delete the dead code entirely, also remove `DentryCache` and `DentryEntry` and the `readdir_cache` field on `TreeContext`. **Decision: option 2 — delete the dead code.** |
| `include/ixsphere/vfs_internal.h` | If option 2: remove `DentryCache readdir_cache` from `TreeContext`. |
| `src/tree.c` | Remove all `dentry_cache_invalidate(&ctx->readdir_cache)` calls. |

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
2. Update `dirchain_list` in `src/tree.c` to use the new struct + VarArray.
3. Update `dentry_cache_build` in `src/dentry_cache.c` to use the new struct + VarArray + namePtr tracking.
4. Build and run all unit tests. Verify 144/144 epoch, 958/958 tree, etc.
5. Delete the dead `dentry_cache` code (option 2) — this includes `DentryCache`, `DentryEntry`, the `readdir_cache` field, the invalidate calls, and the `dentry_cache.{h,c}` files.
6. Re-run `bench_ditto.py` to verify the 5412 only-in-host count doesn't regress (and ideally improves).
7. Re-run 500-scenario FUSE tests to verify no regression.

## Risks

- **Memory leaks.** The new code does `var_array_new_base` at function entry and `var_array_delete_base` before return. If a return is added later that bypasses the cleanup, we leak. Add a comment at the entry and entry-point checks.
- **Per-call malloc overhead.** For a benchmark that does many readdirs, the malloc adds up. For the FUSE readdir path (called once per ls), the overhead is negligible.
- **Dropping `eff_epoch` from dedup output.** This is a code-correctness change, not a perf change. The existing tests must pass to verify. If any test fails, we know the simplification is wrong.
