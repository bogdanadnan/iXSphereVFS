# Phase 28 Bin Job: File Deletion — Implementation Review

Review of the file-deletion bin job implementation (commits `ed18f35`..
`897cfcf`, 6 workloads W1-W6). Tests: **16/16 pass** (ctest, 478s including
test_fuzz 326s). Build: clean.

## Verdict

The implementation is **structurally complete** — all spec requirements are
present and wired correctly. The producer push, trigger handler, work handler,
dispatch, index update, and `pool_free` are all implemented. Tests pass.

However, **three correctness bugs** will cause the bin job to fail its primary
purpose (reclaiming dead pages) under real workloads. Two are high-severity
(mirror leak, data-page classification failure); one is medium (CAS on stack-
local copy instead of shared cache payload). All three are invisible in
single-threaded smoke tests. They must be fixed before delete-heavy workloads.

---

## ✅ What's correct

### W1: Producer push — correct
`tree.c:1910-1911` pushes `BIN_TRIGGER_FILE_DELETED` (type=1) with
`found_childPtr` as context and `dc_vp` as context2. The `BIN_TRIGGER_NOOP`
placeholder is fully replaced. The two remaining NOOP pushes
(`tree.c:2883` vfs_rename, `tree.c:3735` vfs_truncate) are correctly out
of scope.

### W2: Dispatch — correct
`gc.c:1489-1490` routes `BIN_TRIGGER_FILE_DELETED` to
`gc_handle_file_deleted`. `gc.c:1503-1504` routes `BIN_WORK_FREE_PAGES` to
`gc_handle_free_pages`. Threshold split at `gc.c:1513`.

### W5: Index update — correct
`dircontentindex_remove` called at `gc_bin_file_deleted.c:251` after the
drop. Name hash read pre-drop at `:201`. One CAS per drop (not full rebuild).

### W6: `pool_free` — correct (exceeds spec)
`pool.c:341` implements a real CAS-based slot free (not a stub). Operates on
the actual cache payload via `storage_read_with_status`, reads
`(freeCount, firstFreeSlot)`, writes the slot's next-free pointer, then
CAS-bumps `poolState` with bounded retry. Thread-safe. `gc_handle_file_deleted`
uses it for the tombstone and create DirContent slots (`:218`, `:229`).

### Trigger handler shape — all spec steps present
`gc_bin_file_deleted.c:85-267`: file-slot check, `collect_active_snapshots`,
`classify_data_pages`, `check_file_referenced`, push `BIN_WORK_FREE_PAGES`,
inline `drop_dir_entries`, `dircontentindex_remove`. All steps in the spec's
§3.3 algorithm are present.

---

## 🔴 High-severity bugs

### B1. Mirror sibling is never freed — free-before-read ordering error

**Location:** `gc_bin_free_pages.c:59-70`

```c
storage_free(ctx->sb, (int64_t)logical);   // zeroes indir[logical]
...
int64_t mirror = read_mirror_page(ctx->sb, (int64_t)logical);  // indir now 0!
if (mirror >= 0) storage_free(ctx->sb, mirror);
```

`storage_free` calls `indir_set(logical, 0)` which zeroes the indirection
entry. `read_mirror_page` (`gc_bin_free_pages.c:90`) starts with
`indir_lookup(sb, logical)` — which now returns 0 because the indir was just
zeroed. So `read_mirror_page` returns -1, and the mirror is never freed.

The mirror pages leak on **every** deletion where a mirror exists (pages
written twice or more — common for any file that was modified after initial
creation).

**Root cause:** The spec's own pseudocode (§4.1 and §6.2) has the same
free-then-read ordering error. The spec §6.3 even states the contradiction:
"Once the logical page is freed (and the indirection entry is 0), no reader
can resolve the logical page."

**Fix:** Read the mirror BEFORE freeing the logical page:
```c
int64_t mirror = read_mirror_page(ctx->sb, (int64_t)logical);  // read first
storage_free(ctx->sb, (int64_t)logical);                        // then free
if (mirror >= 0) storage_free(ctx->sb, mirror);                 // then free mirror
```

The unused `free_logical_with_mirror` helper at `gc_bin_file_deleted.c:981-987`
already has the correct ordering — use it.

### B2. `classify_data_pages` omits the file-visibility-at-R check — data pages never classified dead on head delete

**Location:** `gc_bin_file_deleted.c:368-390`

The spec (§3.4) requires, for each reference point R:
```c
if (!file_visible_at(R, file_vp, tombstone_vp)) continue;  // CHECK FILE VISIBILITY
if (read_rule_picks(R, E_VP, file_vp)) { live = 1; break; }
```

The implementation only checks the VersionPage epoch against R (exact match,
or even-below). It **never consults the parent dir** to confirm the file is
actually reachable at R. Consequence: for a head delete with no active
snapshots (the common case), every VersionPage at an even epoch < H is
marked `live` — so no data pages are classified dead, `BIN_WORK_FREE_PAGES`
is never pushed, and **the file's data pages leak permanently.**

The file-visibility check is the key step that distinguishes "this
VersionPage is visible at R" (epoch-based) from "this file is visible at R"
(dir-chain-based). Without it, a deleted file's data pages are always
classified as live because the VersionPage chain still exists in the pool.

**Fix:** For each reference point R, first check `file_visible_at(R)` (walk
the parent dir's SlotNode chain, apply the read rule, check if the visible
entry has `namePtr != 0`). If the file is NOT visible at R, skip R — all
VersionPages at R are dead for this file.

---

## 🟡 Medium

### B3. CAS in `drop_dir_entries` operates on a stack-local copy — not concurrent-safe

**Location:** `gc_bin_file_deleted.c:812-815, 822-825, 869-872, 878-881`

```c
PoolSlot s = {0};
pool_acquire(&ctx->pool, slot_vp, true, &s);          // copies 32B into s.bytes
int64_t* head_ptr_field = (int64_t*)(s.bytes + ANCHOR_OFF_HEADPTR);
vfs_cas_i64(head_ptr_field, tombstone_vp, tom_next);  // CAS on STACK buffer!
pool_release(&ctx->pool, &s);                          // memcpy back, non-atomic
```

`pool_acquire` hands the caller a private stack copy (`memcpy` from the cache
payload). The CAS therefore always succeeds on the caller's local copy — it
provides zero cross-thread synchronization. The actual publish is a plain
`memcpy` in `pool_release`, which is last-writer-wins. Under a concurrent
prepend, the drop can clobber the new entry.

**Note:** This is the same "CAS-on-local" hazard that Phase 26 W3-W4
eliminated from the tree operations (the `vfs_cas_i64` count in `tree.c` went
to zero). The bin job reintroduced it because `drop_dir_entries` is a new
function that wasn't present during the Phase 26 migration.

**Fix:** Use `storage_read_with_status` to get a pointer to the live cache
payload, then CAS directly on that pointer (same pattern as
`pool_free` at `pool.c:368,398` and `dequeue_from_free_list` at
`storage.c:574`). Alternatively, take the `vfs_lock` on the parent dir
node VP for the duration of the drop (the same lock `vfs_create` and
`vfs_delete` use), which serializes the CAS naturally.

### B4. Phase A re-check in `drop_dir_entries` is described but not implemented

**Location:** `gc_bin_file_deleted.c:709` (comment) vs `:710+` (implementation)

Spec §3.7 Phase A says the drop should re-check `file_referenced` before
modifying the chain (to catch concurrent `vfs_create` re-adding a live entry).
The comment at `:709` describes this, but the function goes straight to the
chain walk without the re-check. If a concurrent `vfs_create` re-creates the
file between the analysis's `check_file_referenced` and the inline
`drop_dir_entries`, the drop will remove the old create+tombstone — which is
correct (the new create is a different DirContent at a new slot). But if the
concurrent operation modifies the SAME create entry (unlikely but possible
with rename), the drop could remove a still-needed entry.

**Fix:** Implement the Phase A re-check, or document that the concurrent-
re-create case is safe because the new create is at a different VP.

---

## 🟢 Low

### L1. Batch + chain slots still leak (TODO-12 partial)
Only the tombstone and create DirContent slots are freed via `pool_free`
(`gc_bin_file_deleted.c:218, 229`). The work handler does not free batch
slots (`gc_bin_free_pages.c:74-77`). `classify_data_pages` walks but never
frees FileNode/FileContent/PageNode/VersionPage/NameSlot slots. TODO-12 is
only partially closed.

### L2. `find_parent_dir` doesn't check node type
`gc_bin_file_deleted.c:427-500` recurses into any non-tombstone child as if
it were a directory without checking node type. Functionally guarded by
`pool_acquire` failure skip, but inefficient on large trees.

### L3. Dead code: `free_logical_with_mirror` helper
`gc_bin_file_deleted.c:981-987` has the correct mirror-free ordering but is
never called. It should replace the buggy inline code in
`gc_bin_free_pages.c`.

---

## Summary

The implementation is structurally complete — all spec requirements are
present, wired, and tested (16/16 pass). The `pool_free` implementation
exceeds the spec (real CAS-based free instead of a stub). The test suite
covers the spec's test plan.

However, **two high-severity bugs** (B1: mirror leak, B2: data-page
classification failure) mean the bin job **does not actually reclaim the
pages it's supposed to reclaim** under real workloads. B2 is particularly
severe: a head delete with no snapshots (the simplest, most common case)
results in zero data pages being freed. Both bugs are invisible in the
current tests because the tests check the GC thread's lifecycle and the
Bin's push/pop mechanics, not the actual page-free results.

**Recommendation:** Fix B1 and B2 before any further bin-job specs. B1 is a
2-line fix (swap the order of `read_mirror_page` and `storage_free`). B2
requires adding the `file_visible_at(R)` check to the classify loop. B3
should be fixed for correctness under concurrency (use the `vfs_lock` on the
parent dir, matching `vfs_create`/`vfs_delete`'s pattern). After these fixes,
add tests that verify actual page reclamation (e.g., check
`storage_allocate(1)` reuses freed pages after GC).
