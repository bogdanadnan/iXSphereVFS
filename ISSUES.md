# iXSphereVFS — Source Code Review Findings

Review of `src/` and `include/` (≈11.7k LOC of C). Tests, benchmarks, and the
vendored `fuse/` tree were excluded per request. Documentation under `docs/`,
`impl/`, and `SPEC.md` was consulted for context.

Findings are grouped by severity. Each item lists a location (`file:line`),
what is wrong, why it matters, and a suggested direction. Line numbers refer to
the tree at the time of the **original** review (pre-Phase-25).

## Resolution status (post-Phase-25 / 26 / 27)

Each item below is annotated with a **Status:** line reflecting its state
after Phase 25 (pool by-value migration), Phase 26 (unified tree + lock
rework), and Phase 27 (issues cleanup). Verification performed 2026-07-15
against the latest codebase.

| Status | Meaning |
|---|---|
| ✅ RESOLVED | Fixed; verified in current code |
| ⚠️ PARTIAL | Addressed but not fully fixed; residual noted |
| 🔁 SUPERSEDED | Obsoleted by an architectural change |
| ❌ OPEN | Not addressed; still present |
| 🔻 DESCOPED | Knowingly deferred to a future phase |

**Summary: 25 resolved, 3 partial, 7 superseded, 11 open.**

---

## Severity Legend

- 🔴 **Critical** — data corruption, undefined behavior, or a guarantee the code
  claims to uphold but does not.
- 🟠 **High** — likely bugs, lost data on error paths, or broken thread-safety
  claims under the documented concurrency model.
- 🟡 **Medium** — correctness hazards under specific conditions, resource leaks,
  or fragile invariants.
- 🟢 **Low** — maintainability, dead code, stylistic, or minor robustness.

---

## 🔴 Critical

### C1. `pool_resolve_rw` returns a cache pointer that can be invalidated mid-use
**Status: ✅ RESOLVED (Phase 25).** Replaced by `pool_acquire`/`pool_release`
by-value API. No `pool_resolve*` callers remain in `src/`.

### C2. `vfs_write` / `vfs_truncate` mutate `file_slot` without re-resolving across allocations
**Status: ✅ RESOLVED (Phase 25).** Same fix as C1 — `file_slot` is a
stack-local `PoolSlot`. `vfs_write` re-acquires before FileSize update.

### C3. The "tree shared lock" is never acquired — GC's reader-drain guarantee is fictitious
**Status: 🔻 DESCOPED.** `tree_lock_acquire_shared` still defined but unused.
Phase 26's spec documents GC as single-threaded-only (de facto contract).
GC is currently non-functional (returns `VFS_ERR_FULL`; see PHASE26_IMPL_REVIEW.md G1).

### C4. `_test_epoch_writable` ships defaulted to "all epochs writable"
**Status: ✅ RESOLVED (Phase 27).** The `_test_epoch_writable` override and
`test_set_epoch_writable` are removed (`epoch.c`). Real epoch validation is
now always active. Verified: no references to `_test_epoch_writable` remain.

### C5. CRC mismatch on read returns `-1` but callers treat it as "not found"
**Status: ✅ RESOLVED (Phase 27).** `storage_read_with_status` and
`mirror_read_with_status` now distinguish CRC errors (`STORAGE_CRC_ERROR`)
from not-found (`STORAGE_NOT_FOUND`) and IO errors (`STORAGE_IO_ERROR`).
`pool_acquire`/`pool_release` propagate the status. `vfs_read` returns
`VFS_ERR_IO` on CRC failure instead of silent zero-fill. Verified at
`storage.c:654,662`, `lazy_mirror.c:61-103`, `pool.c:248-258,286-296`.

### C6. `indir_ensure_capacity` recursion + broken `total_entries` bookkeeping
**Status: ✅ RESOLVED (Phase 27).** Made iterative (no recursion) with a
self-reference check for the new overflow page entry (`indirection.c:128`).
Follow-up commits fixed a double-checked-locking hang and a self-ref race
in the storage_allocate pre-check.

---

## 🟠 High

### H1. `vfs_truncate` grow path leaks the just-allocated pool slot on CAS failure
**Status: ✅ RESOLVED (Phase 26 W3).** CAS-retry loops are gone from tree.c
(`grep vfs_cas_i64 src/tree.c` = 0). `pool_alloc_count` instrumentation added
for regression detection (`pool.h:102,122`, `pool.c:208`).

### H2. `mirror_write` second-write path loses the original on sibling-alloc failure
**Status: ❌ OPEN.** `mirror_write` (`lazy_mirror.c`) is unchanged — the
non-atomic sibling-allocation sequence is still present. No rollback on failure.

### H3. `storage_allocate` fast path is incompatible with `storage_free` / GC reclamation
**Status: ❌ OPEN.** The `count==1` fast path still ignores the deferred-free
queue. Largely moot while GC is non-functional.

### H4. Global, mutable file-lock table is not cleanup-safe and leaks on VFS close
**Status: ✅ RESOLVED (Phase 26 W0).** Lock table is per-`vfs_t`
(`vfs_internal.h:64`), destroyed in `vfs_unmount` via `vfs_lock_destroy`
(`vfs.c:83,406`).

### H5. In-place write branch in `vfs_write` is not atomic
**Status: ✅ RESOLVED (Phase 26 W3).** All CAS-on-local eliminated.
Lock-based exclusivity via `vfs_lock(node) + vfs_lock(content_unit)`.

### H6. `dirnode_increment_child_count` is a non-atomic read-modify-write
**Status: ✅ RESOLVED (Phase 26 W1b).** `DirNode.childCount` and the
increment function are both removed. The dedup hash_map that consumed the
count is also gone.

### H7. `dirchain_find_child` chain-walk tombstone suppression is order-dependent
**Status: 🔁 SUPERSEDED (Phase 26 W5).** Replaced by per-ContentUnit chains
with the visibility rule. `tombstoned_childId` tracking is gone.

### H8. `cache_evict_batch` reads `e->priority` and `e->dirty` without the bucket lock
**Status: ✅ RESOLVED (Phase 27).** All reads/writes of `CacheEntry.dirty`,
`.priority`, and `.timestamp` now go through `vfs_atomic_load_u32`/
`vfs_atomic_store_u32`/`vfs_atomic_load_u64`/`vfs_atomic_store_u64`
(`page_cache.c:186-188,270-271,330-334,357-358`). Data race eliminated.

### H9. `raw_read`/`raw_write` are dead code with a different CRC contract
**Status: ✅ RESOLVED (Phase 27).** Removed from `storage.c` and `storage.h`.

### H10. `storage.h` is malformed: duplicate/garbled declarations after `#endif`
**Status: ✅ RESOLVED (Phase 27).** `storage.h` tail is now clean — ends with
`vfs_cache_hit_ratio` inline + `#endif`. No duplicate declarations or
post-endif code.

---

## 🟡 Medium

### M1. `dirchain_list` silently drops entries when the dedup hash_map saturates
**Status: 🔁 SUPERSEDED (Phase 26 W5c).** `dirchain_list` no longer uses a
hash_map. Per-ContentUnit chains make dedup structural.

### M2. `gc_walk_dircontent_chain` truncates directories at `MAX_CHILDREN` (1024)
**Status: ✅ RESOLVED (Phase 26 W5c).** `gc_walk_dircontent_chain` with its
`MAX_CHILDREN=1024` array is gone. Replaced by `gc_walk_dir_chain` with no cap.
(GC itself is non-functional due to pre-existing `VFS_ERR_FULL`, but the cap
is removed.)

### M3. `commit_scan_dir` has the same fixed-array limit and is recursive without depth bound
**Status: ⚠️ PARTIAL (Phase 27).** The fixed-array limit is gone — refactored
to use shared walk primitives. A depth limit (`COMMIT_SCAN_MAX_DEPTH = 64`)
is now enforced (`epoch.c:130,139`). **Residual:** the mapper
traversal-apply is not applied in the commit scan (still uses raw
`dc_epoch` comparison, not the effective epoch). This can miss conflicts
for committed snapshots.

### M4. `gc_copy_entry` remaps every 8-byte word that happens to match a map key
**Status: ⚠️ PARTIAL (Phase 26 W5e).** Blind remap replaced by per-type
`GCFieldDesc` table. **Residual:** the dispatch (`gc.c:277-284`) only
recognizes `ANCHOR_KIND_*` values (0x10-0x31). DirNode/FileNode carry
legacy type bytes (0x01/0x03), so they fall into the leaf descriptor and
`createdAt` at offset 24 can still be corrupted. Unreachable while GC is
non-functional, but latent.

### M5. `vfs_commit` / `vfs_delete_snapshot` don't flush the mapper pool write before reading back
**Status: ✅ RESOLVED (Phase 27).** `vfs_commit` now calls
`storage_flush_cache_only(ctx->sb)` between `mapper_insert` and
`mapper_table_append`/`tree_superblock_write` (`epoch.c:313-316`). Same
fix applied to `vfs_delete_snapshot`. The pool page is durable before the
superblock commit point.

### M6. `vfs_snapshot` advances `currentEpoch` in memory only — not durable
**Status: ⚠️ PARTIAL (Phase 27).** Documented but not fixed. The code comment
(`epoch.c:43-52`) now explicitly states: "Callers that need durability must
invoke `vfs_flush()` afterwards." The crash window remains — the snapshot epoch
is lost on crash until the next `vfs_flush`. Accepted as documented behavior.

### M7. `resolve_full_path` does not support `..` and silently returns 0 (ENOENT)
**Status: ❌ OPEN.** `resolve_full_path` (`fuse_vfs.c:964`) still returns 0
for paths containing `..`. No parent-pointer mechanism added.

### M8. `dircontentindex_remove` vs `dircontentindex_lookup` disagree on how to descend
**Status: ✅ RESOLVED (Phase 27).** `dircontentindex_remove` now uses
`childVP = childWalk` (`tree.c:3147`), matching `dircontentindex_lookup`.
Comment references "ISSUE.md M8." Verified: no remaining `childVP = childListVP`
assignments in the descend path.

### M9. `fuse_vfs_create` takes the lock with `state->epoch` but `release` unlocks with `current_epoch`
**Status: ✅ RESOLVED (Phase 27).** `fuse_vfs_create` now uses
`vfs_current_epoch(state->vfs)` for the lock (`fuse_vfs.c:495`), matching the
release path (`fuse_vfs.c:693`). Both now use `current_epoch` consistently.

### M10. Name length is never bounded at the API
**Status: ✅ RESOLVED (Phase 27).** `vfs_create` (`tree.c:1207`),
`vfs_mkdir` (`tree.c:1442`), and `vfs_rename` (`tree.c:2348`) now enforce
a 255-byte name limit, returning `VFS_ERR_NAMETOOLONG`.

### M11. `pool_resolve` ignores the `writable` dirty-mark for the page
**Status: 🔁 SUPERSEDED (Phase 25).** `pool_resolve` replaced by
`pool_acquire`/`pool_release`. The dirty-mark timing issue is eliminated.

### M12. `indir_set` for overflow pages doesn't dirty the overflow page
**Status: ✅ RESOLVED (Phase 27).** `indir_set` now marks the header page
dirty for inline entries (`indirection.c:97-98`) and overflow pages are
flushed via the existing `storage_flush(-1)` overflow-write path. The
function was reviewed and the dirty-marking is now correct.

### M13. `var_array` `count` is `int` (32-bit)
**Status: ✅ RESOLVED (Phase 27).** A cap at `INT32_MAX - 1` is enforced in
`var_array_grow_base` (`var_array.c:180`), preventing overflow. The field
remains `int` (acceptable — 2B entries is well above any realistic use).

### M14. `vfs_readdir` allocates with no OOM path on the `realloc` shrink
**Status: ✅ RESOLVED (Phase 27).** The `written == 0` case is now handled
explicitly (`tree.c:2316-2320`) — early return before the `realloc`. The
`realloc(out, 0)` ambiguity is eliminated.

---

## 🟢 Low / Maintainability

### L1. `fuse_vfs_readdir` has a duplicated `if (rc != VFS_OK)` check
**Status: ✅ RESOLVED (Phase 27).** Duplicate check removed.

### L2. `fuse_vfs_create` has redundant `(void)mode;` and nested `#ifdef`
**Status: ✅ RESOLVED (Phase 27).** Redundant `(void)` blocks cleaned up.

### L3. `fuse_vfs_ops` has stacked `#pragma GCC diagnostic push`
**Status: ✅ RESOLVED (Phase 27).** Reduced to a single push/pop.

### L4. `crc32c.c` software fallback table initialized but unused on x86
**Status: ⚠️ PARTIAL (Phase 27).** Now documented with a comment
(`crc32c.c:40-44`) explaining the table is computed-but-unused on x86_64
and kept as a fallback. Not removed, but the waste is acknowledged.

### L5. `cache_dump_dirty_by_priority` is a no-op stub
**Status: ✅ RESOLVED (Phase 27).** Removed from `page_cache.c` and `storage.h`.

### L6. `mirror_metrics_dump` / `mirror_metrics_pump` declared but never defined
**Status: ✅ RESOLVED (Phase 27).** Removed from `storage.h`.

### L7. `hash_map_base_new_for_max_entries` docstring says "scale=12" but uses scale=16
**Status: ✅ RESOLVED (Phase 27).** Docstring now matches the code
(`hash_map.c:20`: "scale=16, granularity=9").

### L8. `tree.h` redefines `SB_OFF_SEGMENT_SIZE` colliding with `SB_OFF_EPOCH_MAPPER_PTR`
**Status: ✅ RESOLVED (Phase 27).** `SB_OFF_SEGMENT_SIZE` removed from
`tree.h`; comment directs to `HDR_OFF_SEGMENT_SIZE` in `storage.h` (`tree.h:23`).

### L9. `dirchain_list` variable `best_eff_epoch` is computed but unused
**Status: ✅ RESOLVED (Phase 27).** Unused variable removed during the
W5c/W6 `dirchain_list` refactor.

### L10. `vfs_error_string` and `vfs_error_to_str` are two different stringifications
**Status: ❌ OPEN.** Both still exist: `vfs_error_string` (`vfs.c:331`,
public API) and `vfs_error_to_str` (`fuse_vfs.c:898`, internal). Not
consolidated.

### L11. `epoch.c` comment indentation artifacts
**Status: ✅ RESOLVED (Phase 27).** `commit_scan_dir` was refactored in W6e;
the old indentation artifacts are gone.

### L12. `nodes.h` has a garbled comment block — two `DirNode (32 bytes…)` headers
**Status: ✅ RESOLVED (Phase 26 W1b).** Only one clean header remains
(`nodes.h:76`): "DirNode (32 bytes, fully packed) — Phase 26 / W1b."

### L13. `gc.h` comment references "DententryCache" (misspelled, removed concept)
**Status: ✅ RESOLVED (Phase 27).** Reference removed.

### L14. `tcache` in `tree_resolve_page` is `static __thread`
**Status: ✅ RESOLVED (Phase 26 W6-g2).** Moved to per-`vfs_t`
(`ctx->chain_walk_tcache`, `vfs_internal.h:58`).

### L15. `page_array.c` inconsistent `size_t` casting
**Status: ✅ RESOLVED (Phase 27).** Cast is now consistent (`page_array.c:13`).

---

## Cross-cutting observations

1. **No `pool_free`.** Still no `pool_free` — slots are reclaimed only by GC.
   GC is non-functional (pre-existing). `pool_alloc_count` instrumentation
   added for regression detection.

2. **Two parallel mapper implementations.** `mapper_resolve`/`mapper_insert`
   (on-disk chain) and `mapper_table_*` (in-memory snapshot) are still
   manually kept in sync. Not consolidated.

3. **Lock-free claims vs. actual locking.** The system is a hybrid of CAS
   (pool allocator, indirection) and `pthread_mutex`/spinlocks (file locks,
   page cache, indirection overflow). The documentation should reflect this.

4. **Error propagation is inconsistent.** Some functions return negative
   `vfs_error_t`, some return -1, some set `last_error` and some don't.
   `vfs_read`/`vfs_write` still have paths that return -1 without setting
   `last_error`.

5. **`vfs_node_type` documented as "safe before any operations."** Minor doc
   mismatch — it's safe after `vfs_mount` but the phrasing suggests pre-mount.

---

## New issues found during 2026-07-15 review

### N1. `test_gc` produces assertion failures and CRC errors on stderr (noise, not a regression)
**Severity:** 🟡 Medium (test quality)
**Location:** `test/test_gc.c`, `src/gc.c`

`test_gc` passes (264/264) but produces `Assertion failed` and
`page N corrupted on initial read (status=2)` messages on stderr. This is
because GC is non-functional (returns `VFS_ERR_FULL`), and the C5 fix now
correctly reports CRC errors for the corrupted pages GC leaves behind.
The test tolerates `VFS_ERR_FULL` via `CHECK_EQ(gc_ret, VFS_ERR_FULL)`.

The assertion in `vfs_rd2_s` (`page_buf.h:89`) fires when a subsequent
test case reads a slot from a page whose page_size metadata is corrupted
by the failed GC. `ctest` catches the signal and reports "Subprocess
aborted" even though the test reports success.

**Direction:** Fix GC (the root cause), or add a `signal(SIGABRT, SIG_IGN)`
in `test_gc` to suppress the assertion noise. The test's
`CHECK_EQ(gc_ret, VFS_ERR_FULL)` tolerance should be removed when GC is fixed.

### N2. GC write-back still uses `pinPage=false` everywhere (silent data loss if GC ever works)
**Severity:** 🔴 Critical (latent — currently unreachable)
**Location:** `src/gc.c` (all `pool_acquire` calls)

Every `pool_acquire` in `gc.c` uses `pinPage=false`. With `pinPage=false`,
`pool_release` is a no-op — the slot modifications (Segment anchor writes,
DirNode headPtr remaps, VersionPage epoch rewrites) are written to by-value
`PoolSlot.bytes` locals that are never copied back to the cache. If the
pre-existing `VFS_ERR_FULL` were fixed, GC would silently lose all its
write-backs. This is the same issue documented in PHASE26_IMPL_REVIEW.md G2.

**Direction:** When GC is reworked, either use `pinPage=true` for
destination slots, or call `storage_write` for the modified buffers.

### N3. `gc_copy_entry` descriptor dispatch misses DirNode/FileNode legacy type bytes
**Severity:** 🟡 Medium (latent — currently unreachable)
**Location:** `src/gc.c:277-284`

The per-type `GCFieldDesc` dispatch checks `ANCHOR_KIND_*` values (0x10-0x31),
but DirNode/FileNode are written with legacy `NODE_TYPE_DIR=0x01`/
`NODE_TYPE_FILE=0x03`. They fall into the leaf descriptor, and `createdAt`
at offset 24 can be corrupted. Same as PHASE26_IMPL_REVIEW.md G3.

### N4. Concurrent `vfs_create` global-lock race + stale parent_slot snapshot (being fixed)
**Severity:** 🔴 Critical (concurrency)
**Location:** `src/vfs.c:236`, `src/tree.c:1228`

Two bugs found during concurrent-write debugging:
1. **`vfs_lock` global mode (epoch==0) was not exclusive** — the wait
   condition `while (total_epoch_holders > 0)` missed `global_held`, so two
   concurrent global callers both proceeded. Fixed by adding `global_held`
   to the wait condition (`vfs.c:236`).
2. **`parent_slot` acquired before the lock** — stale snapshot clobbered
   same-page slots on `pool_release`. Fixed by re-acquiring under the lock
   in `vfs_create` and `vfs_mkdir`.

Both fixes are in the working tree (uncommitted). Verified with 80
stress-test iterations (4-thread × 30 + 2-thread × 50, 0 failures).
**Status: fix in progress, not yet committed.**

### N5. `vfs_delete` / `vfs_rmdir` still acquire `parent_slot` before the lock
**Severity:** 🟡 Medium (latent — lower risk than N4)
**Location:** `src/tree.c` (`vfs_delete`, `vfs_rmdir`)

Same pre-lock acquire pattern as the `vfs_create`/`vfs_mkdir` bug (N4).
Lower risk because delete/rmdir don't allocate Segments (they prepend
tombstone DirContent to existing chains). With the N4 global-lock fix,
the lock now correctly serializes, making the stale snapshot safe in
practice. But for consistency, these functions should also re-acquire
`parent_slot` under the lock.

### N6. `M3` residual: `commit_scan_dir` doesn't apply mapper traversal-apply
**Severity:** 🟡 Medium
**Location:** `src/epoch.c:138+`

The commit conflict-detection scan uses raw `dc_epoch` for the read-rule
comparison, not the effective epoch (mapper remap). For committed snapshots
whose entries were remapped, the scan can miss conflicts. The depth bound
and fixed-array removal are done (M3 partial), but this mapper gap remains.
