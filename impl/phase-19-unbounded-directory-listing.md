# Phase 19: Unbounded Directory Listing

## Goal

Replace the fixed-size dedup arrays in `dirchain_list` and `dentry_cache_build` with a heap-backed, unbounded VarArray, and simplify the dedup algorithm to "first-hit-wins" based on the chain-ordering invariant.

## Background

A directory's `DirContent` chain is prepended on every mutation, so the chain is in **descending epoch order at the head** (newest record at front, oldest at tail). Combined with the spec's read rule (`SPEC.md` §7.2), the highest-epoch applicable record for each childNodeId appears **first** in the chain walk.

The current code (commit `25f685d`, `src/tree.c:1216-1286` and `src/dentry_cache.c:18-67`) tracks all applicable records per childNodeId in 5 parallel arrays of size `DENTRY_CACHE_MAX=1024`:

```c
int64_t best_child[DENTRY_CACHE_MAX];
int64_t best_childPtr[DENTRY_CACHE_MAX];
int64_t best_eff_epoch[DENTRY_CACHE_MAX];
int     best_name_set[DENTRY_CACHE_MAX];
int64_t best_namePtr[DENTRY_CACHE_MAX];
```

The loop:

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

Three problems:

1. **Hard cap.** `while (walk_vp != 0 && best_count < DENTRY_CACHE_MAX)` — directories with >1024 unique children are silently truncated.
2. **Stack blowup.** 5 × 8KB = ~36KB on stack per call. Recursive GC and deep traversal can overflow.
3. **Unnecessary epoch comparison.** Because the chain is descending, the first applicable hit for a childNodeId is by definition the highest-epoch applicable record. The `eff_epoch > best_eff_epoch[found]` branch is defensive code that never fires. Removing it simplifies the algorithm and the data structure.

## Design

### Dedup entry

```c
typedef struct {
    int64_t childNodeId;
    int64_t childPtr;       /* VirtualPtr to child FileNode or DirNode */
    int     name_set;        /* 1 = live entry, 0 = tombstone */
    int64_t namePtr;        /* VirtualPtr to cached NameEntry (0 if tombstone) */
} DirchainDedupEntry;
```

`eff_epoch` is no longer tracked per-entry. The chain ordering guarantees that the first applicable hit is the correct one.

### Algorithm

```
walk chain head-to-tail (descending epoch):
    read ce_child, ce_epoch, ce_childPtr, ce_namePtr, ce_next
    eff_epoch = mapper.resolve(ce_epoch)   # with traversalApply
    if !read_rule_applies(eff_epoch, read_epoch):
        continue
    if seen.contains(ce_child):
        continue    # already have a higher-epoch applicable record
    seen.add(ce_child)
    append DirchainDedupEntry{ce_child, ce_childPtr, namePtr!=0, namePtr} to dedup

output:
    for entry in dedup:
        if !entry.name_set: continue    # skip tombstones
        entries[written++] = entry
```

The "seen" set is a simple O(N²) linear scan over the dedup array (current behavior). For typical directories (<100 children) this is <10K ops and runs in microseconds.

### Read rule (correct, per `SPEC.md` §7.2)

```
applies = (eff_epoch == read_epoch) ||
         (eff_epoch < read_epoch && eff_epoch % 2 == 0)
```

**This must also skip `eff_epoch > read_epoch` records.** The current code (`src/tree.c:1237-1238`) does NOT explicitly skip future-epoch records. The chain ordering invariant (descending by epoch) means future-epoch records appear first in the walk, and the current read rule marks them as `applies` (since they're not `< read_epoch` either). The first hit for the wrong child is then captured as the "best" record, even though the read is at a past epoch.

We need to add: `if (eff_epoch > read_epoch) continue` before the `applies` check.

This is a separate, more concerning correctness bug — see `phase-19-read-rule.md` (TBD).

## Files

| File | Change |
|------|--------|
| `src/tree.c` | `dirchain_list`: replace 5 parallel arrays with `VarArray(DirchainDedupEntry)`, drop `eff_epoch` tracking, fix read rule (case 6: skip future epochs). |
| `src/dentry_cache.c` | `dentry_cache_build`: same simplification. |
| `src/dentry_cache.h` | Remove or repurpose `DENTRY_CACHE_MAX` (no longer used as a hard cap). |
| `src/tree.h` | Declare `DirchainDedupEntry` struct (shared between `tree.c` and `dentry_cache.c`). |
| `test/scenarios/` | Re-run all 500 scenarios. Expect: no regression on tests that pass; some previously-passing `count_mismatch` tests should still pass since the FUSE-side 64 cap is the dominant cause. |

## API surface

No new public API. Internal change to dedup storage.

## Acceptance

- [ ] `dirchain_list` no longer truncates at 1024 children.
- [ ] `dentry_cache_build` no longer truncates at 1024 children.
- [ ] Stack frame size of `dirchain_list` reduced from ~36KB to ~256B (just the VarArray handle).
- [ ] `DirchainDedupEntry` struct has 4 fields (no `eff_epoch`).
- [ ] `DENTRY_CACHE_MAX` removed from `dentry_cache.h` (or marked as deprecated for compile-time sanity checks only).
- [ ] All 500 FUSE test scenarios still pass (or improve).
- [ ] Unit tests in `test/test_tree.c` still pass.

## Non-Goals

- **Hash-based "seen" set** for O(N) instead of O(N²). Deferred — current linear scan is sufficient for typical directory sizes.
- **Per-thread scratch VarArray** for allocation reuse. Deferred — per-call alloc is simple and correct.
- **FUSE-side 64-entry cap** in `fuse_vfs.c:297-298`. Separate change.

## Out of scope for this phase

1. **FUSE 64-cap** (`fuse_vfs.c`). Deferred to a separate change. The current phase removes the in-VFS 1024 cap; the FUSE cap is a different bottleneck.
2. **Hash dedup** (O(N) instead of O(N²)). The user acknowledged this is a known characteristic of the existing code; the current change is purely a capacity fix.
3. **Read-rule correctness** (case 6: skip future-epoch records). This is a separate concern tracked in `phase-19-read-rule.md` (TBD).
