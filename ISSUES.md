# iXSphereVFS — Source Code Review Findings

Review of `src/` and `include/` (≈11.7k LOC of C). Tests, benchmarks, and the
vendored `fuse/` tree were excluded per request. Documentation under `docs/`,
`impl/`, and `SPEC.md` was consulted for context.

Findings are grouped by severity. Each item lists a location (`file:line`),
what is wrong, why it matters, and a suggested direction. Line numbers refer to
the tree at the time of review.

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
**Location:** `src/pool.c:200` (`pool_resolve`), used everywhere in `src/tree.c`.

`pool_resolve` calls `storage_read` (`src/storage.c:545`), which returns a
pointer **directly into the page-cache payload buffer**. Callers in `tree.c`
(`vfs_create`, `vfs_write`, `tree_resolve_page`, `dirchain_*`, …) hold these raw
pointers across `pool_alloc`, `storage_read`, `storage_write`, and CAS loops.

The cache evicts entries in `cache_evict_batch` (`src/page_cache.c:153`) whenever
`entry_count >= max_entries`, and eviction **frees the payload buffer**
(`page_cache.c:279`). The page-cache spinlocks are released between Pass 1
(collection) and Pass 2 (detach + free), and eviction is triggered *inside*
`cache_insert` — which is called by `storage_write`, which is called by
`pool_alloc`/`storage_read` that the same caller just invoked.

Concrete hazard: a thread does
```c
uint8_t* parent_slot = pool_resolve_rw(&ctx->pool, parent);  // pointer A
... checks parent_slot ...
int64_t dc_vp = pool_alloc(&ctx->pool);   // may evict the page backing A
nodes_write_dircontent(dc_slot, ..., old_head = vfs_rd8_s(parent_slot, ...)); // USE AFTER FREE
```
The `pool_alloc` → `storage_write` → `cache_insert` → `cache_evict_batch` path
can free the very page that `parent_slot` points into. The same pattern repeats
in `vfs_create` (`tree.c:645` then `:688`), `vfs_mkdir`, `vfs_delete`,
`vfs_rmdir`, `vfs_rename`, `vfs_write`, `vfs_truncate`, and `tree_resolve_page`
(which re-resolves `fc_slot` after allocations, but not always `file_slot`).

This is the single largest correctness risk in the codebase: under cache
pressure the pointers handed out by the pool are not stable for the duration
they are used. The comments in `pool.h:109-127` claim the dirty-mark happens
"before the pointer is returned," but nothing *pins* the page against eviction
while the caller holds the pointer.

**Direction:** either (a) return copies (32-byte slots) instead of pointers into
the cache, (b) implement page pinning/ref-counts so a held slot cannot be
evicted, or (c) re-resolve after every allocation. Option (a) is by far the
simplest and matches how 32-byte slot-oriented APIs are usually safe.

---

### C2. `vfs_write` / `vfs_truncate` mutate `file_slot` without re-resolving across allocations
**Location:** `src/tree.c:2240` (`vfs_write`), `tree.c:2140` (`vfs_truncate`).

`vfs_write` resolves `file_slot` once at `:2240`, then inside the page loop calls
`tree_resolve_page` (which does `pool_alloc`), `storage_allocate`, `pool_alloc`
for VersionPage/FileSize, and `storage_read`/`storage_write`. Any of these can
trigger cache eviction and free the page backing `file_slot`. The subsequent
`vfs_atomic_load_i64((const int64_t*)(file_slot + FILENODE_OFF_SIZEPTR))`
(`:2376`) and the FileSize CAS at `:2385` then touch freed memory.

Same shape in `vfs_truncate` (`:2140` resolve, `:2159` `pool_alloc` inside the
loop, then `:2172` CAS through `file_slot`).

This is the same root cause as C1, called out separately because the write path
is the hottest, most allocation-heavy path and the most likely to evict.

**Direction:** same as C1 — re-resolve `file_slot` after every allocation, or
stop returning cache-internal pointers from the pool.

---

### C3. The "tree shared lock" is never acquired — GC's reader-drain guarantee is fictitious
**Location:** `src/gc.c:17-67` (lock impl), `src/gc.h:26-36` (decls).

`tree_lock_acquire_shared` / `tree_lock_release_shared` are **defined but never
called** anywhere in `src/` (grep confirms only the definitions and the header
declaration exist). Every read/write entry point (`vfs_read`, `vfs_write`,
`vfs_create`, `vfs_open`, `dirchain_*`, `vfs_readdir`, …) enters the tree
**without** registering as a reader.

`vfs_gc` calls `tree_lock_acquire_exclusive` (`gc.c:1090`), which spins until
`readers == 0`. Since no path ever increments the reader count, GC proceeds
immediately while concurrent reads/writes are still in flight. The deferred-free
queue (`gc.c:1085-1109`) and the comment "waits for all readers to drain"
(`gc.c:1089`) describe a safety property the code does not enforce.

The consequence: GC frees pages (`storage_free` in `gc.c:1007`) and remaps
VirtualPtrs while other threads may still be resolving and dereferencing those
exact VirtualPtrs. Combined with C1, this is a use-after-free / dangling-pointer
window during any concurrent `vfs_gc`.

**Direction:** take the shared lock around every tree-walking operation, or
delete the RW-lock and document GC as stop-the-world (single-threaded only).

---

### C4. `_test_epoch_writable` ships defaulted to "all epochs writable"
**Location:** `src/epoch.c:9-18`.

```c
static int _test_epoch_writable = 1;   // default: all writable
...
bool vfs_epoch_is_writable(TreeContext* ctx, int64_t epoch) {
    if (_test_epoch_writable >= 0)
        return _test_epoch_writable != 0;   // ALWAYS TRUE in production
    ...
}
```

The real epoch validation (the function body after the override) is **only
reached when a test calls `test_set_epoch_writable(-1)`**. In any non-test
build, `vfs_create`/`vfs_write`/`vfs_delete`/`vfs_mkdir`/`vfs_truncate` accept
*any* epoch — including odd snapshots that have already been committed, odd
snapshots that have been soft-deleted, and even epochs that are not the live
head. The SPEC (§7.1) explicitly forbids all three.

There is no production call that flips the override to `-1`. So the library, as
shipped, does not enforce write-epoch rules at all. This silently breaks
snapshot isolation: a writer passing `epoch=<some odd snapshot>` will mutate
chains that readers at that snapshot (and the commit path) assume are immutable.

**Direction:** remove the override, or default it to `-1` and gate the override
behind `#ifndef NDEBUG` / a test-only TU. A global mutable switch on a security-
and correctness-critical invariant should not exist in release builds.

---

### C5. CRC mismatch on read returns `-1` but callers treat it as "not found"
**Location:** `src/storage.c:75` (`raw_read`), `src/lazy_mirror.c:54,67,97,108`.

`raw_read` returns `-1` for *both* "page never written" (`indir_lookup == 0`) and
"CRC validation failed" (`computed != ph.checksum`). `mirror_read` collapses
these again. Up the stack, `storage_read` (`storage.c:561`) turns any
`mirror_read != 0` into "return NULL," and callers like `vfs_read`
(`tree.c:2469-2477`) treat `storage_read == NULL` as "zero-fill the page."

Result: **silent data loss on disk corruption.** A page whose CRC fails is read
as if it were an unwritten (zero) page, with no error propagated to the caller
and no `last_error` set. For a storage layer that advertises "CRC32C validates
on read" (`README.md:112`), this is the wrong failure mode — it should surface
`VFS_ERR_IO`, not zeros.

**Direction:** distinguish "not allocated" from "checksum failed," and propagate
the latter as `VFS_ERR_IO` up to the caller.

---

### C6. `indir_ensure_capacity` recursion + broken `total_entries` bookkeeping
**Location:** `src/indirection.c:119-226`.

Two distinct problems in the overflow-page growth path:

1. **Recursive self-call on CAS failure** (`indirection.c:206-210`): when the
   "link previous overflow page's next" CAS fails, the function frees `buf` and
   calls `indir_ensure_capacity(sb, needed)` **recursively from scratch**. The
   recursion re-walks the whole loop with the original `needed`, can allocate a
   *different* new page, and on return the caller's state (the just-freed
   `buf`) is gone. Beyond being wasteful, the recursive call does not unwind the
   physical_tail advance or the total_pages bump already performed for this
   iteration — they become leaked "zombie" slots. Under contention this can
   recurse arbitrarily deep.

2. **`required < total_entries` uses `<` but the loop tests `>=`**
   (`indirection.c:128` vs `:131`): the early-out compares
   `sb->total_pages + needed < total_entries`, but `total_entries` only accounts
   for *currently allocated* overflow pages. After the loop body appends one
   overflow page, `total_entries += entries_per_overflow` (`:222`) — but the
   `while (required >= total_entries)` condition uses the freshly-grown count,
   while `required` is still `sb->total_pages + needed` computed from a
   `total_pages` that the body also bumps (`:172-175`). The interaction of these
   two monotonically-increasing values is subtle and easy to break; the net
   effect in edge cases is either over-allocation of overflow pages or an
   off-by-one where the caller's `needed` is not actually satisfied.

**Direction:** make the function iterative (loop-and-retry, not recurse), and
compute `required` once from a stable snapshot of `total_pages`.

---

## 🟠 High

### H1. `vfs_truncate` grow path leaks the just-allocated pool slot on CAS failure
**Location:** `src/tree.c:2156-2176` (shrink path) and `:2374-2391` (`vfs_write`).

The shrink path's CAS-retry loop re-`pool_alloc`s a *new* FileSize slot on every
retry without freeing (or reclaiming) the previous attempt's slot:
```c
while (1) {
    int64_t old_sizePtr = vfs_atomic_load_i64(...);
    int64_t fs_vp = pool_alloc(&ctx->pool);     // allocated EVERY iteration
    ...
    if (cas_res == old_sizePtr) break;          // only the winner is linked
}
```
Every CAS failure leaks one 32-byte pool slot permanently (no `pool_free`
exists). Under contention this can leak many slots per call. The identical
pattern is in `vfs_write` at `:2374-2391`. The pool is never reclaimed outside
GC, so these leaks accumulate until `vfs_gc`.

**Direction:** allocate once *outside* the CAS loop (the standard pattern used
correctly in `vfs_create` at `tree.c:703`), then retry only the CAS.

---

### H2. `mirror_write` second-write path loses the original on sibling-alloc failure
**Location:** `src/lazy_mirror.c:148-170`.

On the second write to a page (`mirror_page == -1`), the code allocates a
sibling, writes the new payload to the sibling, then links
`ph.mirror_page = sibling` into the **original** via a 4-byte pwrite
(`lazy_mirror.c:167-168`). If `storage_allocate` or `write_page_record` fails,
the function returns `-1` *after* the original's `mirror_page` may have been
left at `-1` — but the *new* payload was never durably written to the original
either (only the sibling got it, and the sibling is now orphaned). The caller
(`cache_flush_page`, `cache_flush_all`, `cache_evict_batch`) does **not** clear
`dirty` on `mirror_write` failure (`page_cache.c:409`, `:467`, `:276`), so the
page stays dirty — but the cache buffer and the on-disk state now diverge in a
way the next `mirror_read` may or may not paper over depending on generation
ordering.

There is no atomic "switch to mirrored mode" — it is a torn write across two
pages and a header field, with no rollback.

**Direction:** write the new generation to the *original* page first (advancing
its generation), then allocate + link the sibling. A crash leaves the original
current; no orphan.

---

### H3. `storage_allocate` fast path is incompatible with `storage_free` / GC reclamation
**Location:** `src/storage.c:370-417`.

The fast path assumes "the first free slot is exactly `sb->total_pages`" and
comments: *"this works because there are no holes in [2, sb->total_pages) during
the file-create/write hot path. ... until then [GC reclaims], this works."*

But `storage_free` (`storage.c:536`) *does* create holes (sets the indir entry
to 0), and GC calls it during `gc_shadow_compact` *before*
`deferred_free_confirm_and_release` finishes. The deferred-free queue is checked
only in `storage_allocate_count_scan` (the `count > 1` fallback at
`storage.c:439`), **not** in the `count == 1` fast path. So after GC frees pages
but before they are handed back, a concurrent `pool_alloc` on the fast path can
re-allocate `sb->total_pages` (which is fine) — but a *later* `storage_acquire`
of a specific page, or the count>1 scan, can hand out a page that GC just
decided to reclaim.

The two allocation paths have different free-space policies; the fast path
ignores the deferred queue entirely.

**Direction:** route both paths through one free-space oracle, or make the fast
path consult `deferred_free_is_queued`.

---

### H4. Global, mutable file-lock table is not cleanup-safe and leaks on VFS close
**Location:** `src/vfs.c:35-36`, `:90-192`.

`lock_table` is a global array of 256 buckets with a single global
`table_lock`. Entries are keyed by `nodeId` and refcounted. Problems:

1. **Process-global state shared across VFS instances.** Two `vfs_mount` calls
   in one process share the same lock table; locks taken for VFS-A's nodeId 5
   collide with VFS-B's nodeId 5. The SPEC/README say nothing about
   single-process multi-mount, but the API allows it.
2. **No teardown.** `vfs_unmount` does not release or drain outstanding locks.
   If a thread holds a lock at unmount, the `FileLockState` is never freed
   (refcount never reaches 0 from the unmount path). On next mount the entry is
   reused with a stale `global_owner`.
3. **`vfs_lock(vfs, new_nodeId, epoch)` in `vfs_create` (`tree.c:682`) locks on
   the *new file's* nodeId, but `vfs_open` / `vfs_read` never lock — so the
   per-file lock only guards `create` vs `release`, not concurrent `write` vs
   `write`.** Two threads `vfs_write`-ing the same `vp` concurrently will both
   pass the `vfs_lock` (write path locks on `file`, not `new_nodeId`, at
   `tree.c:2250` — but *only* after resolving `file_slot`; there is no lock in
   `vfs_read` at all). The write path's CAS loops tolerate concurrent writers,
   but the in-place-update branch (`tree.c:2298-2305`) does
   `storage_read → memcpy overlay → storage_write` with no CAS — the last
   writer wins, partial overlays can interleave.

**Direction:** make lock state per-`vfs_t`, hook cleanup into `vfs_unmount`,
and either lock in `vfs_read` or document that concurrent read/write of one fd
is caller-serialized.

---

### H5. In-place write branch in `vfs_write` is not atomic
**Location:** `src/tree.c:2298-2305`.

```c
if (found_in_place) {
    uint8_t* page_buf = storage_read(ctx->sb, data_page);   // read
    memcpy(page_buf + page_offset, src, page_count);        // overlay
    storage_write(ctx->sb, data_page, page_buf, 0);         // write
    break;
}
```
Two threads writing different offsets of the same page at the same epoch both
read the current page, overlay their slice, and write back — classic
read-modify-write race; whichever writes last erases the other's bytes. The
`vfs_lock` at `:2250` is per-file but (a) `vfs_read` doesn't take it, and (b)
FUSE `release` unlocks unconditionally, so the lock is not a reliable
serialization for overlapping writes from different opens.

**Direction:** serialize per-PageNode in-place updates (CAS the versionRoot or
hold a per-page lock), or document that overlapping writes to the same page are
undefined.

---

### H6. `dirnode_increment_child_count` is a non-atomic read-modify-write under a "caller holds parent lock" claim that is false
**Location:** `src/tree.c:610-622`.

The comment says *"the slot is page-locked by the caller (vfs_create/mkdir/
delete/rmdir/rename hold the parent lock for the duration of the mutation), so
concurrent writers to the same parent are serialized."*

But the "parent lock" it refers to is `vfs_lock(vfs, new_nodeId, epoch)` — which
locks on the **child's** nodeId, not the parent's. Two concurrent `vfs_create`
calls in the same parent directory create *different* children → different
nodeIds → independent locks → both run `dirnode_increment_child_count` on the
same DirNode slot concurrently. The `vfs_rd4_s`/`vfs_wr4_s` pair is a torn
read-modify-write; increments are lost.

`childCount` is documented as a "best-effort upper bound" (`tree.c:604-609`), so
lost increments don't corrupt the tree — but the dedup hash_map in
`dirchain_list` is sized from it (`tree.c:1221`), and undersizing causes
`hash_map_base_put` to return `-1` (silent entry drop, see M1).

**Direction:** use `vfs_atomic_add_i32` (already used elsewhere in the file), or
actually serialize on the parent.

---

### H7. `dirchain_find_child` chain-walk tombstone suppression is order-dependent and wrong for cross-name re-creates
**Location:** `src/tree.c:1974-2005` (chain fallback), mirrored at `:1888-1932`.

The walk tracks a single `tombstoned_childId` and suppresses *any* later live
entry with the same childId. Because the chain is head→tail (newest first), a
tombstone for childId X is seen before X's older live entry — correct for
"delete X." But consider: create "a" (childId 5), delete "a" (tombstone,
childId 5), create "a" *again* (new DirContent, childId 6, same name). The
chain is `[live-6, tomb-5, live-5]`. Walking for "a": see live-6 (name matches,
set best), see tomb-5 (set `tombstoned_childId=5`), see live-5 — childId 5 ==
tombstoned, so **suppressed**. That's fine for this case.

Now: create "a" (5), create "b" (7), delete "a" (tomb-5), create "a" (8).
Chain: `[live-8, tomb-5, live-7, live-5]`. Walk for "a": live-8 matches, tomb-5
sets the flag, live-7 is name "b" (no match, but `tombstoned_childId` is still
5, and 7 != 5 so not suppressed — fine), live-5 suppressed. OK.

The fragile case is **rename**. `vfs_rename` (same-dir, same-epoch,
`:1365-1417`) rewrites the DirContent's `namePtr` in place rather than
prepending a new entry. So a single DirContent entry for childId 5 originally
named "a" now points at name "b". If there was *also* an older live entry for
childId 5 named "a", the tombstone logic has no tombstone for 5 and both the
renamed entry and the stale entry are live-5 — the dedup keeps the first
(newest) and silently hides the older. Whether that's correct depends on
whether the rename produced a tombstone; same-dir same-epoch rename produces
**none** (`tree.c:1422` comment: "skip tombstone"). So the older "a"-named
live-5 entry stays reachable at the old name via the tree index
(`dircontentindex_remove` is called at `:1405`, which zeroes the *link*, not
the DirContent — and only for the matched walk VP).

This is subtle and likely the source of the "phase-25" bug-fix churn visible in
recent commits. The invariant "first applicable hit per childNodeId is the
highest-epoch record" (`tree.c:1189-1192`) is only true *because* of prepend
ordering — in-place rename breaks the prepend invariant.

**Direction:** add a regression test for same-dir rename followed by lookup of
both old and new name, and strongly consider making rename always prepend
(tombstone + new entry) rather than mutate-in-place.

---

### H8. `cache_evict_batch` reads `e->priority` and `e->dirty` without the bucket lock in Pass 1
**Location:** `src/page_cache.c:168-213`.

Pass 1 acquires each bucket lock to walk the chain, but it reads
`e->dirty`/`e->priority` into `EvictCandidate` and then **releases** the lock
before Pass 2. Pass 2 re-validates `*cands[k].pp != e` and `still_eligible`
under the lock — good — but the *collection* in Pass 1 can observe a transient
state where another thread is mid-`cache_insert` updating `e->priority` and
`e->dirty` (`page_cache.c:309-314`). The `timestamp` read is also racy against
`lru_promote`. Since `dirty`/`priority` are plain ints (not atomic), the read is
a data race under the C11 memory model.

In practice on x86 this is unlikely to corrupt, but it can collect a candidate
that just turned dirty+priority-1 (which must not be evicted). The Pass 2
re-validation mostly saves it, but the window between Pass 1 unlock and Pass 2
lock is where priority-1..3 pages can be wrongly detached if the re-validation
itself races on the same word.

**Direction:** make `dirty` and `priority` atomic, or hold the bucket lock
across collection-and-detach for the same bucket.

---

### H9. `raw_read`/`raw_write` are dead code with a different CRC contract than the live path
**Location:** `src/storage.c:61-109`.

`raw_read` and `raw_write` are declared in `storage.h:138-140` and defined, but
**never called** by anything in `src/` (grep confirms only the definitions and
the extern decls). The live read/write path goes through `mirror_read`/
`mirror_write` and the cache. `raw_write` has its own generation-bump and
mirror-init logic (`storage.c:97-98`) that diverges from `mirror_write`'s state
machine — if anyone ever wires these up, they'll get a third, inconsistent
mirror lifecycle. Dead code in a storage layer is a liability.

**Direction:** delete `raw_read`/`raw_write` and their declarations, or
re-route the live path through them with a clear comment.

---

### H10. `storage.h` is malformed: duplicate/garbled declarations after `#endif`
**Location:** `src/storage.h:217-221`.

```c
int64_t vfs_cache_max_entries(void);int64_t vfs_cache_max_entries(void);

#endif /* VFS_STORAGE_H */
void vfs_data_inc_total(void);
void vfs_data_inc_hits(void);
```

There is a duplicated declaration on one line (`vfs_cache_max_entries` twice,
including a definition `int64_t vfs_cache_max_entries(void) { return CACHE_DEFAULT_MAX; }`
at `storage.c:41` that ignores its argument version
`vfs_cache_get_max_entries` at `storage.c:21` — two functions, nearly identical
names, one takes `sb` and one doesn't, both exist). And **two function
declarations appear after `#endif`**, outside the include guard. They happen to
compile only because they're plain prototypes and some TU includes
`storage.h` after `<stdio.h>` etc., but they're structurally wrong: any
re-include of `storage.h` silently drops them, and they're invisible to the
guard's purpose.

**Direction:** move the prototypes above `#endif`, deduplicate
`vfs_cache_max_entries` vs `vfs_cache_get_max_entries`.

---

## 🟡 Medium

### M1. `dirchain_list` silently drops entries when the dedup hash_map saturates
**Location:** `src/tree.c:1249-1263`, `src/hash_map.c:189-228`.

`hash_map_base_put` returns `-1` when the table is full (no auto-grow, per the
Phase-23 comment at `hash_map.c:9`). `dirchain_list` calls it as
`(void)hash_map_put(seen, ...)` (`tree.c:1262`) — the return value is discarded.
If the map saturates (which depends on the `childCount`-based sizing at
`tree.c:1221` and on hash collisions), the put silently fails, the next entry
with the same key is *not* recognized as seen, and `var_array_append` adds a
duplicate. The output then contains duplicate dirents for the same child.

The sizing formula (`hash_map_base_new_for_max_entries`, `hash_map.c:101`)
targets 10% load factor, which should normally avoid saturation — but
`childCount` includes tombstones (`nodes.h:42-49`), and the formula's
`max_entries * 10` can be zero when `childCount == 0` (then scale=4, cap=16,
fine) but the *actual* live entry count can exceed the estimate if
`childCount` underflows (see H6) or if the chain has many entries the dedup
must process even if they don't all survive.

**Direction:** check the return value of `hash_map_put`; on `-1`, either grow
the map or fall back to a linear scan. At minimum, assert/return `VFS_ERR_IO`.

---

### M2. `gc_walk_dircontent_chain` truncates directories at `MAX_CHILDREN` (1024)
**Location:** `src/gc.c:592-637`.

The first pass collects unique childNodeIds into
`uint32_t child_ids[MAX_CHILDREN]` with `while (walk_vp != 0 && child_count <
MAX_CHILDREN)`. A directory with > 1024 unique children will have the 1025th+
entries ignored for survival-rule decisions — they won't be walked recursively
(`gc.c:705-740` third pass uses `child_has_kept` indexed by `cidx`, which is
-1 for untracked children, so they're skipped). Those children's subtrees are
never copied to the new pool → after GC they're gone.

`dirchain_list` was specifically rewritten (Phase 24) to remove the 1024 cap
for *reads*, but the GC path — which determines what *survives* — still has it.
This is a latent data-loss bug for any directory with > 1024 entries when GC
runs.

**Direction:** make `child_ids`/`child_best_epoch`/`child_has_survivor`
dynamically grown (VarArray or hash map, like `dirchain_list` does).

---

### M3. `commit_scan_dir` has the same fixed-array limit and is recursive without depth bound
**Location:** `src/epoch.c:52-132`.

Same `MAX_RMDIR_CHILDREN`/`MAX_CHILDREN` shape: `commit_scan_dir` recurses into
subdirectories (`epoch.c:81`) with no depth limit. A deeply nested directory
tree will blow the FUSE worker stack (which `tree.c:2187` notes is "small" on
macOS). And it doesn't apply the read-rule with mapper traversal-apply
(`epoch.c:70-71` uses raw `dc_epoch`, not the effective epoch), so commits of
snapshots whose entries were committed-remapped will miss conflicts.

**Direction:** convert to iterative with an explicit stack, and apply the mapper
traversal-apply consistently with `verchain_get`/`sizechain_get`.

---

### M4. `gc_copy_entry` remaps every 8-byte word that happens to match a map key
**Location:** `src/gc.c:199-225`.

The entry-copy helper scans all four 8-byte words in a 32-byte slot and remaps
any value that exists as a key in `gc_map`. The comment acknowledges this
(`gc.c:212-215`): "only values that exist as keys in gc_map will be remapped,
which correctly filters out non-pointer fields."

That is **not** correct. A non-pointer field (e.g., `createdAt` timestamp,
`fileSize`, `modifiedAt`, `epoch`, `childNodeId` zero-extended into the high
bytes) can *coincidentally* equal some VirtualPtr that was remapped. When that
happens, `gc_copy_entry` rewrites a data field as if it were a pointer,
corrupting it. The probability is low per-field, but there are 4 words × many
entries × every GC, and the corruption is silent.

Concretely: `FileSize.fileSize` at offset 12 (8 bytes) holds a byte count. If
that count, interpreted as an int64, equals any old_vp in the map (values
≥ 2<<16 = 131072, i.e., files larger than ~128 KB), it will be rewritten to the
new_vp. File sizes would be silently corrupted by GC.

**Direction:** give `gc_copy_entry` per-node-type field descriptors (which
offsets are VirtualPtrs) instead of blindly scanning all four words.

---

### M5. `vfs_commit` / `vfs_delete_snapshot` don't flush the mapper pool write before reading back
**Location:** `src/epoch.c:163-172`.

`mapper_insert` writes the new MapperEntry via `pool_alloc` + `pool_resolve_rw`
+ CAS. The pool page is marked dirty (priority POOL). Then
`mapper_table_append` updates the in-memory snapshot. But the on-disk pool page
is only flushed by `tree_superblock_write` → `storage_flush(-1)` at `:175`. If
the process crashes between `mapper_insert` (`:163`) and
`tree_superblock_write` (`:175`), the in-memory table says the snapshot is
committed but the pool page never made it to disk. On reopen, `mapper_table_init`
walks the on-disk chain — which doesn't have the new entry — so the snapshot
appears active again. Concurrent readers using the in-memory table during that
window also see a state that isn't durable.

**Direction:** flush pool pages before the superblock, or document the
crash window (the superblock is the commit point, but the mapper entry must
precede it on disk).

---

### M6. `vfs_snapshot` advances `currentEpoch` in memory only — not durable until next superblock write
**Location:** `src/epoch.c:37-45`.

`vfs_snapshot` does `vfs_atomic_add_i64(&ctx->currentEpoch, 2)` and returns. No
`tree_superblock_write`, no flush. A crash immediately after loses the snapshot
epoch entirely (on reopen, `currentEpoch` is read from the superblock, which
still has the old value). The SPEC (§12.4) describes snapshot as returning an
epoch "immediately usable"; durability is unspecified but a snapshot that
vanishes on crash is surprising.

**Direction:** either document that snapshots are in-memory until next flush
(and that writers must `vfs_flush` to make them durable), or write the
superblock in `vfs_snapshot`.

---

### M7. `resolve_full_path` does not support `..` and silently returns 0 (ENOENT) for any path containing it
**Location:** `src/fuse_vfs.c:963-967`.

```c
if (strcmp(token, "..") == 0) {
    free(path_copy);
    return 0;   // treated as "not found"
}
```
FUSE callers translate this to `-ENOENT`. Tools that resolve paths with `..`
(common in shell scripts, `find`, rsync) will get spurious "No such file"
errors. The VFS has no parent pointer, but the FUSE layer *does* track full
paths in the readdir cache and could resolve `..` against the path prefix.

**Direction:** resolve `..` lexically before walking, or track parent VPs.

---

### M8. `dircontentindex_remove` vs `dircontentindex_lookup` disagree on how to descend
**Location:** `src/tree.c:1789` (remove) vs `src/tree.c:1552` (lookup).

In `dircontentindex_lookup`, when a matching internal child is found, the code
sets `childVP = childWalk` (the child's own VP) — correct, per the "Phase 18
tree-correctness" comment at `tree.c:1638-1645`. In `dircontentindex_remove`,
the same branch sets `childVP = childListVP` (`tree.c:1789`) — i.e., the
child's *children-list head*, not the child itself. That's the exact bug the
lookup fix corrected. `remove` will descend one level too deep, fail to find
the leaf, and return `-1` — so rename's "zero the old link" call
(`tree.c:1405`) silently does nothing for names below the top level. Stale links
accumulate (they're filtered out by hash during lookup, so it's a space leak,
not a correctness bug — but it contradicts the function's own documentation).

**Direction:** change `tree.c:1789` to `childVP = childWalk`.

---

### M9. `fuse_vfs_create` takes the lock with `state->epoch` but `release` unlocks with `current_epoch`
**Location:** `src/fuse_vfs.c:504` (create locks with `state->epoch`),
`:702` (release unlocks with `vfs_current_epoch`).

`vfs_lock`/`vfs_unlock` key on `(nodeId, epoch)` — the global lock uses epoch=0,
per-epoch uses a counter. If `state->epoch` is odd (a snapshot mount) and
`current_epoch` is even, the lock is taken as a per-epoch lock but released as
a different per-epoch lock (different epoch value → different counter slot in
the same `FileLockState`? Actually no — `epoch_count` is a single int, not keyed
by epoch; but the release path checks `epoch == 0` for global vs else for
per-epoch). The asymmetry means a per-epoch lock taken at `state->epoch` is
released as "per-epoch" at a *different* epoch — the counter is decremented
correctly by accident, but `global_pending`/`global_held` logic could be
perturbed if any global lock is in flight.

More importantly, the FUSE write path (`fuse_vfs.c:451`) writes at
`vfs_current_epoch`, not `state->epoch` — so the lock epoch and the write epoch
disagree on snapshot mounts.

**Direction:** pick one epoch (current_epoch is correct for writes) and use it
consistently for lock/unlock across create/open/release/write.

---

### M10. Name length is never bounded at the API — `nodes_read_name` can overrun a 256-byte buffer silently
**Location:** `src/nodes.c:329-369`, callers in `tree.c` use `char entry_name[256]`.

`nodes_read_name` takes `max_len` and stops at `max_len - 1`, so it won't
overrun. But it also doesn't signal truncation — a name longer than 255 bytes is
silently chopped, and `strcmp` against the lookup name then fails, producing a
spurious NOTFOUND. The VFS lets `nodes_write_name` store arbitrarily long names
(no cap in `nodes.c:270`), but the FUSE dirent name field is `char name[256]`
(`vfs.h:63`) and every call site uses a 256-byte stack buffer. Names ≥ 255 bytes
are a correctness grey hole.

**Direction:** enforce a 255-byte name limit at `vfs_create`/`mkdir`/`rename`
(return `VFS_ERR_IO` or `ENAMETOOLONG`), since the on-disk format supports
longer but every reader assumes 255.

---

### M11. `pool_resolve` ignores the `writable` dirty-mark for the page that backs a read-only resolve used later for writing
**Location:** `src/pool.c:200-214`.

`pool_resolve(pool, vptr, writable=1)` marks the page dirty *after*
`storage_read` returns (`pool.c:210`). Between the `storage_read` and the
`cache_mark_dirty`, another thread can call `cache_evict_batch`, which in Pass 2
re-validates `still_eligible` using the dirty flag (`page_cache.c:252-253`).
If eviction reads `dirty == 0` (the mark hasn't happened yet), it will detach
and free the page; the `cache_mark_dirty` call at `pool.c:210` then operates on
a freed entry or a re-loaded clean page. This is the "pinning race" the comment
at `pool.c:172-177` claims to fix by re-`storage_read`-ing on CAS failure — but
that fix only covers `pool_alloc`'s CAS, not `pool_resolve`'s dirty-mark.

**Direction:** mark dirty under the same bucket lock as the read, or before
returning the pointer (with a barrier), and document the invariant.

---

### M12. `indir_set` for overflow pages doesn't dirty the overflow page for flushing
**Location:** `src/indirection.c:94-112`.

`indir_set` on an inline entry marks the header page dirty (`:100`), correct.
But `indir_set` on an overflow entry just does
`vfs_atomic_store_i64(&entries[1 + entry_idx], physical_offset)` (`:110`) — it
never marks the overflow page dirty in the cache. Overflow pages are flushed
separately in `storage_flush(-1)` by iterating `it->overflow_pages`
(`storage.c:595-602`) and mirror-writing them unconditionally, so the data does
reach disk on full flush. But a cache-only flush (`storage_flush_cache_only`,
`storage.c:637`) or an eviction path won't see these pages as dirty, and the
in-memory `indir_set` is not reflected until a full `storage_flush(-1)`.

**Direction:** track overflow-page dirtiness explicitly, or document that
overflow indirection is only durable via `storage_flush(-1)`.

---

### M13. `var_array` `count` is `int` (32-bit) but VirtualPtr math and indexes are `int64_t`
**Location:** `src/var_array.h:68` (`volatile int count`), `src/var_array.c:175`.

`var_array_grow_base` does `vfs_atomic_add_i32((int32_t*)&a->count, 1)`. The
`count` is `int`, so the array is capped at INT_MAX (~2.1B) entries. More
dangerously, `var_array_resolve_base` (`var_array.c:251`) checks
`if (idx >= a->count)` where `idx` is `int` — but `hash_map_slot_ptr`
(`hash_map.h:155`) checks `target >= a->count` with `target` as `int64_t`. The
mixed-sign comparisons are fine in practice but the 32-bit count silently caps
the hash map and the dedup VarArray at 2B entries, which is below what a
large-file VarArray-backed structure might want.

**Direction:** make `count` `int64_t` (it's already `volatile`; the atomics
have 64-bit variants).

---

### M14. `vfs_readdir` allocates `vfs_dirent_t` (≈280 bytes each) with no OOM path on the `realloc` shrink
**Location:** `src/tree.c:1316-1318`.

```c
if (written < total_count) {
    vfs_dirent_t* exact = realloc(out, written * sizeof(vfs_dirent_t));
    if (exact) out = exact;
}
```
If `realloc` fails (returns NULL), the original `out` is kept — but
`*out_count = written` is set to the smaller count. So the caller gets a buffer
sized for `total_count` entries with `written` valid — fine, just wastes memory.
But if `written == 0`, `realloc(out, 0)` is implementation-defined (may return
NULL and free `out`, or return a non-NULL pointer). On `written == 0` the
earlier branch at `:1271-1277` returns early, so this is probably unreachable —
but the `realloc(ptr, 0)` ambiguity is a footgun if that early return ever
moves.

**Direction:** guard `written == 0` explicitly, or skip the shrink entirely
(it's a micro-optimization).

---

## 🟢 Low / Maintainability

### L1. `fuse_vfs_readdir` has a duplicated `if (rc != VFS_OK)` check (`fuse_vfs.c:330-332`)
Likely copy-paste. Harmless (second is dead) but indicates the file needs a
pass.

### L2. `fuse_vfs_create` has a redundant `(void)mode; (void)flags;` block and a stray `#ifdef __APPLE__` inside an `#ifdef __APPLE__` (`fuse_vfs.c:457-467`)
The macOS branch is already inside `#ifdef __APPLE__`; the nested `#ifdef` is
dead nesting. The `(void)mode;` appears twice.

### L3. `fuse_vfs_ops` registration has two stacked `#pragma GCC diagnostic push` for the same warning (`fuse_vfs.c:838-842`, `:878-879`)
Two pushes, two pops, same `-Wincompatible-function-pointer-types`. The double
push indicates the function-pointer-type incompatibility is known and silenced
rather than fixed — the `fuse_operations` table assigns callbacks whose
signatures don't match libfuse's expected prototypes (macFUSE's
`fuse_darwin_attr` vs upstream `struct stat`). That's a macFUSE-specific
reality, but it should be a single push with a clear comment.

### L4. `crc32c.c` software fallback is not compiled when `VFS_ARCH_X86_64 && VFS_COMPILER_GCC`, but the table init still runs (`crc32c.c:34-37`)
On x86_64 GCC the constructor initializes `s_crc32c_table` which is then never
used (the HW path is taken unconditionally at `crc32c.c:141`). Wasted work at
load time and dead data. Minor.

### L5. `cache_dump_dirty_by_priority` is a no-op stub (`page_cache.c:481-484`)
Declared in `storage.h:183`, documented as "Dump per-priority dirty page
count," implemented as `(void)sb;`. Either remove or implement.

### L6. `mirror_metrics_dump` / `mirror_metrics_pump` declared in `storage.h:147-148` but never defined or called
`grep` finds no definitions in `src/`. Link error if any TU references them.

### L7. `hash_map_base_new_for_max_entries` docstring says "scale=12 default" but the code uses scale=4..20 (`hash_map.c:96-111`)
The top-of-file comment (`hash_map.c:20-30`) describes scale=12/granularity=8
defaults that no longer match `hash_map_base_new_for_max_entries`. The function
itself is correct; the stale comment predates Phase 25.

### L8. `tree.h:22` redefines `SB_OFF_SEGMENT_SIZE` to offset 16, colliding conceptually with `SB_OFF_CURRENT_EPOCH` (also 8? no — 16 is `SB_OFF_EPOCH_MAPPER_PTR`)
```c
#define SB_OFF_CURRENT_EPOCH    8
#define SB_OFF_EPOCH_MAPPER_PTR 16
...
#define SB_OFF_SEGMENT_SIZE     16  // in StorageBackend header page (page 0)
```
The comment says "in StorageBackend header page (page 0)" to disambiguate, but
the same identifier is defined twice with the same value for two different
pages. The superblock is page 1; `SB_OFF_SEGMENT_SIZE` is read from page 0
(`tree.c:98`). The naming gives no signal that they're in different pages.
Worth a rename (`HDR_OFF_SEGMENT_SIZE` already exists in `storage.h:126` — use
that).

### L9. `dirchain_list` variable `best_eff_epoch` / `eff_epoch` calculations are unused for decision-making (`tree.c:1238-1243`)
The `eff_epoch` is computed but only `applies` is tested; the
`first-hit-wins` design means the epoch value itself is irrelevant once
`applies` is true. The computation is wasted work on every entry. Minor.

### L10. `vfs_error_string` (vfs.h:39) and `vfs_error_to_str` (fuse_vfs.h:128) are two different stringifications of the same codes
`vfs_error_string` returns "I/O error"; `vfs_error_to_str` returns "IO". One is
public API, one is internal. Consolidate.

### L11. `epoch.c:140` comment "Validate snapshot_epoch is odd" is followed by the check, but the blank line/comment pair is repeated for `vfs_delete_snapshot` with inconsistent indentation (`epoch.c:185-187`)
Style only; the `}` on its own line at `:147`, `:192` suggests an editing
artifact.

### L12. `nodes.h:29-51` has a garbled comment block — two `DirNode (32 bytes…)` headers back-to-back
The first (lines 29-30) is a truncated leftover; the second (lines 33+) is the
real one. Harmless but messy.

### L13. `gc.h:200-203` comment references "DententryCache" (misspelled, and the concept was removed in Phase 24)
Stale documentation in `vfs_gc`'s doc comment.

### L14. `tcache` in `tree_resolve_page` is `static __thread` with a fixed `TCACHE_SIZE=16` and no invalidation on file growth within the same segment (`tree.c:292-298`)
The cache is keyed `(file_vp << 20) | segment_idx` and validated against
`gc_generation`. But within a generation, a segment can grow (new PageNodes
appended) and the cached `vptr_array` won't see the new pages — the code at
`tree.c:314-318` falls through to chain walk and patches a single entry, which
is correct but means the tcache is incomplete until rebuilt. Performance issue,
not correctness.

### L15. `page_array.c:13` uses `memset(arr->vptr_array, 0, segment_size * sizeof(int64_t))` — `segment_size` is `uint32_t`, `sizeof(int64_t)` is `size_t`, the product is fine, but the same `malloc` at `:9` casts `(size_t)segment_size` and the `memset` doesn't
Inconsistent casting style; both are correct but should match.

---

## Cross-cutting observations (not bugs, worth flagging)

1. **No `pool_free`.** The pool allocator has `pool_alloc` but no
   `pool_free` — slots are reclaimed only by wholesale GC compaction. This is a
   deliberate design (simpler free-list, GC is the reclamation point), but it
   means every transient allocation (e.g., CAS-retry orphans, H1) is permanent
   until GC. The leak rate under contention is therefore a real capacity
   concern, not just tidiness.

2. **Two parallel mapper implementations.** `mapper_resolve`/`mapper_insert`
   (`mapper.c`) walks the on-disk pool chain; `mapper_table_*` keeps an in-memory
   snapshot. They're kept in sync manually (`vfs_commit` calls `mapper_insert`
   *then* `mapper_table_append`). Forgetting one (e.g., in a future code path)
   silently desyncs the in-memory view from durable state. A single source of
   truth with the in-memory table as a pure cache would be safer.

3. **Lock-free claims vs. actual locking.** The SPEC and docs describe a
   "lock-free CAS" hot path, but the file-lock subsystem (`vfs.c`) uses
   `pthread_mutex`, the FUSE dir cache uses `pthread_mutex`
   (`fuse_dir_cache.c:37`), the indirection overflow growth uses a spinlock
   (`indirection.c:179`), and the page cache uses per-bucket spinlocks
   (`page_cache.c:29`). The "lock-free" label is accurate only for the
   pool-allocator CAS and the indirection-entry CAS — the system as a whole is
   a hybrid. The documentation should reflect that.

4. **Error propagation is inconsistent.** Some functions return negative
   `vfs_error_t` (vfs_create, vfs_open), some return -1 on error (vfs_read,
   vfs_write), some set `ctx->last_error` and some don't. `vfs_read`/`vfs_write`
   never set `last_error` on their internal failure paths (e.g.,
   `tree.c:2269`, `:2301`), so `vfs_last_error` after a failed write returns
   whatever the last successful operation left. The FUSE layer relies on
   `vfs_last_error` for errno mapping (`fuse_vfs.c:431`), so failed writes can
   surface a stale or wrong errno.

5. **`vfs_root` is documented as "safe to call before any file operations"
   (`vfs.h:86`) but returns `ctx->rootNodeOffset`, which is only populated after
   `tree_bootstrap_superblock` inside `vfs_mount`.** A NULL `vfs` is handled,
   but the phrasing suggests pre-mount safety, which isn't a real use case.
   Minor doc mismatch.
