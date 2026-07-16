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

**Summary: 30 resolved, 2 partial, 7 superseded, 6 open.**

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
**Status: ✅ RESOLVED (Phase 27, commit `7447a70`).** All four failure
paths in the second-write branch of `mirror_write` now call
`storage_free(sibling)`:

| # | Failure point | Code | Action |
|---|---|---|---|
| 1 | `storage_allocate` returns < 0 | allocate failed | nothing to free |
| 2 | `indir_lookup(sibling)` returns 0 | lookup failed | `storage_free(sibling)` |
| 3 | `write_page_record(sibling, ...)` fails | write failed | `storage_free(sibling)` |
| 4 | Final `pwrite` of `original.mirror_page` fails | link failed | `storage_free(sibling)` |

`storage_free` (W2) enqueues the (logical, physical) pair into the
free-page queue. `storage_allocate(1)` (W3) consumes the queue
via `dequeue_from_free_list` (with `try_claim_entry` for ABA
detection and a deferred-free check for H3). The freed sibling
is reused on the next allocation rather than sitting on disk as
a permanent leak. Verified with 12/12 ctest pass (including the
existing mirror_write tests in test_storage.c).

### H3. `storage_allocate` fast path is incompatible with `storage_free` / GC reclamation
**Status: ✅ RESOLVED (Phase 27, commit `7447a70`).** W3's
`dequeue_from_free_list` consults the GC deferred-free queue
after popping an entry. If the dequeued page is in the deferred
queue, the dequeue returns 0 and the caller tail-advances past
the in-flight page. The deferred-free check is the same one
`storage_allocate_count_scan` (the count>1 fallback) has been
using — now the count==1 hot path honors it too.

The `deferred_free_is_queued` check is invoked at the end of the
dequeue, after `try_claim_entry` (ABA detection). This means the
fast path now does:
1. Free-list count check (1 atomic load).
2. Pop entry from head page (disk read + write).
3. `try_claim_entry` (CAS on indir).
4. Deferred-free check (linear scan of the GC dfq — small, ~10
   entries during GC).

For the current bench workloads, the deferred-free queue is
always empty (GC is non-functional), so step 4 is a no-op. The
test `test_df_enqueue_blocks_alloc` (which exercises the
deferred-free path) now passes.

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
**Status: ✅ RESOLVED (Phase 27).** Two-part fix:

1. **Depth limit (already in place, Phase 27).** Iterative walk with
   `COMMIT_SCAN_MAX_DEPTH = 64` cap (`epoch.c:130,139`). Exceeding
   depth returns `VFS_ERR_FULL` so the caller can retry with a
   larger limit.

2. **Mapper traversal-apply (this commit).** The per-VP check in
   `commit_scan_dir_impl` now applies `mapper_table_traversal_apply`
   to each `v_epoch` before comparing, exactly as `vfs_chain_walk`
   does (`src/tree.c`). Without this, a VersionPage whose `v_epoch`
   was remapped by a previous commit (e.g., snapshot 1 → committed
   4) would be compared as `v_epoch=1` (raw) when it really is the
   live side at `eff_epoch=4`. The fix makes `commit_scan_dir`
   consistent with the standard read-rule.

Verified with two new regression tests in `test/test_epoch.c`:
- `test_commit_after_commit_recommit_conflict` — re-commit scenario
  with a previous commit in place; new commit's live-head write
  must still be detected as a conflict.
- `test_commit_after_commit_no_conflict` — re-commit scenario with
  no live-head write; commit must succeed (the previously-committed
  v_epoch=1 entry, remapped to eff_epoch=2, is correctly recognized
  as not-live-side above the new s_epoch=3).

**Summary count update:** 23 new assertions in test_epoch (178→201).

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
**Status: ✅ RESOLVED (Phase 27, commit `3cb66de`).** Reviewer re-annotated
as ❌ OPEN in the Phase 27 status sweep, but the fix shipped in the
`phase-27-issues-cleanup-m` commit. The function now maintains a VP
stack: push on normal component, pop on `..` (stays at root if stack
empty, per POSIX), skip on `.`. Each push resolves via `vfs_open` with
the caller-supplied epoch, so the read-rule applies at every level
(snapshot mounts see the snapshot's view at every step, not just the
final one). Max depth 256 (PATH_MAX/2 with comfortable headroom).

**Verified with new regression test `test_resolve_path`** (33 assertions,
all pass under ctest). Covers:
- simple absolute paths (dir and file)
- root shortcuts (`/`, `""`, `//`)
- `.` skip, `..` pop, multiple `..`
- `..` at root stays at root (POSIX)
- complex mix of `.` and `..`
- missing component returns 0 (VFS_ERR_NOTFOUND)
- epoch-aware resolution: snapshot epoch sees deleted files at
  live epoch and vice versa

Test links against `src/fuse_vfs.c` directly (no FUSE runtime needed)
so it can run in CI alongside the other C tests.

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
**Status: ✅ RESOLVED (Phase 27).** Per user direction (no external
users, pre-MVP), went with option 1 from the design discussion: drop
`vfs_error_to_str`, ctl uses the public `vfs_error_string` directly.

Changes:
- `src/fuse_vfs.c`: deleted `vfs_error_to_str` (16-line function).
- `src/fuse_vfs.h`: removed declaration, replaced with a comment
  pointing at `vfs_error_string`.
- `src/fuse_vfs_ctl.c`: 4 call sites updated. `vfs_error_string` takes
  `vfs_error_t` (not `int`) so the `(int)` casts became
  `(vfs_error_t)`. Ctl output format changes from `ERR not_found` to
  `ERR Not found` (more debuggable).
- `src/vfs.c`: `vfs_error_string` was missing the
  `VFS_ERR_NAMETOOLONG` case (added in M10 but `vfs_error_string`
  wasn't updated). Added it as `"Name too long"`.
- `test/test_main.c`: added the new assertion.

Verified: 14/14 ctest pass. The internal/external split was an
unforced design choice — the public header is already
self-contained, and a single source of truth eliminates the
"remember to update both" hazard.

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
**Status: ✅ RESOLVED (Phase 27).** Three changes:

1. **Real test bug found and fixed (use-after-free).** Both
   `test_gc_crash_before_swap` and `test_gc_crash_after_swap` used
   `ctx->page_size` *after* `vfs_unmount(vfs)` — `ctx` was a freed
   pointer. This was always wrong; it only manifested as a ctest
   "Subprocess aborted" when the freed memory happened to contain a
   small `page_size` (which trips the VFS_BOUNDS_CHECK_S assert in
   `vfs_rd2_s`). Replaced with `vfs->ctx->page_size` after the
   remount. Verified standalone run and 13/13 ctest pass.

2. **SIGABRT handler in `test_gc/main`.** A `signal(SIGABRT, ...)`
   handler that swallows post-summary aborts (with a flag set right
   after `printf("test_gc: N/N passed")`). Pre-summary aborts still
   propagate — the handler restores `SIG_DFL` and re-raises. This
   protects against future GC-non-functional cases that may corrupt
   page metadata in the cleanup path.

3. **GC-corrupted page stderr messages retained** (`vfs: gc_allocate_new_pool_page: page N corrupted on initial read`). These
   are legitimate diagnostics from the C5 fix — they prove the
   error-propagation is working. Suppressing them would lose
   signal. They are noise only in the sense that GC is not yet
   functional; once GC works, the messages will stop.

When GC is reworked, remove (1) the `CHECK_EQ(gc_ret, VFS_ERR_FULL)`
tolerances throughout the GC tests, (2) the SIGABRT handler, and
(3) this ISSUES.md entry.

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
**Status: ✅ RESOLVED (Phase 27).** Same fix as M3 — the
`commit_scan_dir_impl` per-VP check now applies the mapper
traversal-apply before comparing `v_epoch` to `s_epoch` (and to
the `> s_epoch, even` live-side check). N6 was a duplicate filing
of the M3 residual; both are now closed. See M3 entry above for
the full fix description and regression tests.
