# C1 Fix Review — Pool by-value migration (Phase 25)

Scope: the uncommitted changes across `src/{pool,pool.h,tree,tree.h,gc,mapper,
epoch,nodes,page_array,page_array.h,vfs}.c/h`, as specified in
`impl/phase-25-pool-by-value-migration.md`. ~1.5k lines changed across 11 files.

## Verdict

The **structural goal is met**: no production code in `src/` holds a raw pointer
into the page-cache payload across an allocation. `pool_resolve*` is now used
only by test shims; every `src/` caller goes through `pool_acquire`/`pool_release`
with a stack-local `PoolSlot`. The C1 use-after-free window (raw pointer freed by
`cache_evict_batch`) is genuinely closed for the listed hazardous sites.

However, the migration introduces a **new concurrency hazard** that partially
trades C1 for a different problem: several CAS loops now operate on the
caller's *local* byte buffer instead of the shared cache line, which silently
disables the lock-free coordination those CASes were providing. Details below,
plus a handful of correctness issues in specific call sites.

---

## ✅ What's correct

1. **`pool_acquire`/`pool_release` implementation** (`src/pool.c:206-280`) is
   sound. Zero-fill on failure, `pinnedPage` carried in the struct, release is
   idempotent (clears the pin), and the dirty-mark-in-acquire keeps the page
   resident so the release `storage_read` is a cache hit. The failure model
   (zero bytes → caller's type check catches it) matches the spec.

2. **Read-only paths are clean.** Every read-only caller (`dirchain_find_child`,
   `dirchain_list`, `verchain_get`, `sizechain_get`, `mapper_resolve`,
   `commit_scan_dir`, `vfs_read`, `vfs_file_size`, GC walks) uses
   `pinPage=false`, acquires, reads `.bytes`, and releases as a no-op. No
   behavior change vs. the old `pool_resolve_ro`, just a 32-byte memcpy. This
   is the 107-of-116 majority and it's done uniformly.

3. **`tree_resolve_page` tcache fall-through bug** (`src/tree.c:517-523`) —
   the `done:` → `retry_walk:` fall-through that doubled cache reads was found
   and fixed with an explicit `break`. Good catch; the W9 status note documents
   it (seqwrite cache reads dropped from ~4M to ~27K).

4. **`vfs_write` re-acquires `file_slot` before the FileSize update**
   (`src/tree.c:2947`). This is the subtle correctness fix the migration
   required: `tree_resolve_page` mutates `HEADPTR` on the FileNode via its own
   `file_slot`, so the caller's stale local would clobber it on release.
   Re-acquiring picks up the new `HEADPTR`. The comment at `:2939-2946`
   documents why. This is exactly right.

5. **Release discipline on error paths is thorough.** Every early `return` in
   `vfs_create`/`mkdir`/`delete`/`rmdir`/`rename`/`write`/`truncate` calls
   `pool_release` on each pinned slot. The `{0}` initializer makes a missed
   release a harmless no-op rather than a stale write. Spot-checked all of them.

6. **`dirnode_increment_child_count` was correctly inlined** (`src/tree.c:894-
   901`). The old standalone helper wrote to the cache via `pool_resolve_rw`,
   which would have raced with the parent's `pool_release`. Doing the
   read-modify-write on `parent_slot.bytes` and persisting via the same release
   is the right shape.

---

## 🔴 New hazard introduced: CAS-on-local disables lock-free coordination

**Locations:**
- `src/tree.c:866-875` — `vfs_create`, CAS on `parent_slot.bytes + DIRNODE_OFF_HEADPTR`
- `src/tree.c:1813-1822` — `vfs_rename` (cross-dir), CAS on `dst_slot.bytes + HEADPTR`
- `src/tree.c:2036-2044` — `dircontentindex_insert`, CAS on `slot.bytes + LISTVP`
- `src/tree.c:415-417`, `:426-428`, `:462-464`, `:473-475` — `tree_resolve_page`, CAS on `fc_slot.bytes + ROOTPTR` / `prev_slot.bytes + NEXTPTR`
- `src/tree.c:2702-2704` — `vfs_truncate` shrink, CAS on `file_slot.bytes + SIZEPTR`
- `src/tree.c:2920-2922` — `vfs_write` COW path, CAS on `pn_slot.bytes + VERSIONROOT`

**The problem.** These CAS loops were written to coordinate *concurrent* writers
via the shared cache line. Example, `vfs_create` (head-prepend):
```c
do {
    old_head = vfs_atomic_load_i64((const int64_t*)(parent_slot.bytes + DIRNODE_OFF_HEADPTR));
    nodes_write_dircontent(dc_slot.bytes, ..., old_head, ...);
} while (vfs_cas_i64((int64_t*)(parent_slot.bytes + DIRNODE_OFF_HEADPTR),
                     old_head, dc_vp) != old_head);
```
`parent_slot.bytes` is now a **stack-local copy**. `vfs_cas_i64` compiles to
`__atomic_compare_exchange_n(ptr, &expected, desired, …)` where `ptr` points
into the caller's stack. So:

- The CAS always compares against the *local* value and writes to the *local*
  value. It can only fail if this same thread re-raced itself, which it can't
  inside one `do/while` body. **The loop is effectively a single iteration** —
  the CAS is a no-op write to a local.
- Two concurrent `vfs_create` calls in the same parent each start from their own
  copy of `HEADPTR` (say, both read `old_head = H`). Both write `dc_vp_A` /
  `dc_vp_B` to their local, both "CAS-succeed" trivially, both `pool_release`
  writes their local back to the cache. **Last writer wins; the other
  DirContent is orphaned** (its `nextPtr` pointed at a `HEADPTR` value that's
  now overwritten). The prepend chain is broken — one of the two files becomes
  unreachable from the directory.

The old code CAS'd the *cache line* (raw pointer into the page-cache payload),
which is the shared memory all threads see — that's what made the prepend
lock-free and correct under concurrency. The migration converted the pointer to
a local copy but left the CAS pointed at the local.

**The `pool_release` write-back is non-atomic** (`memcpy` in `pool.c:268`), so
it cannot substitute for the CAS even if only the "winner" releases.

**Severity:** This is the same class of bug C1 was — a concurrency hazard — but
in the opposite direction. C1 was a UAF under cache pressure; this is a lost
update under concurrent mutation. The migration doc and code comments
acknowledge "CAS on the local copy (per-thread, last-writer-wins at release)"
(`tree.c:2014-2016`, `:2693-2701`, `:2913-2918`) and frame it as acceptable
because "vfs_lock(file, epoch) serializes same-epoch writers." But:

- `vfs_create`/`mkdir` lock on the **new child's** nodeId (`tree.c:810`), not
  the parent's, so two creates in the same directory take different locks and
  do not serialize.
- `vfs_truncate` does not take the file lock at all (the comment at `:2693`
  admits this).
- `tree_resolve_page` takes no lock.
- The file lock table is global and shared across VFS instances (ISSUES.md H4);
  relying on it for tree-structure correctness is fragile.

**Direction.** The CAS must target the shared cache line, not the local copy.
Three options, in order of cleanliness:

1. **CAS through the cache.** After preparing the new entry in `dc_slot` (or
   `linkSlot`, etc.), re-resolve the *parent* slot's HEADPTR field directly
   from the cache and CAS there. The local `parent_slot` is used only for
   reads/validation, not as the CAS target. Concretely for `vfs_create`:
   ```c
   uint8_t* live = storage_read(pool->sb, VFS_VPTR_PAGE(parent));
   int64_t* head_ptr = (int64_t*)(live + slot_off(parent) + DIRNODE_OFF_HEADPTR);
   /* CAS on head_ptr, which is the shared cache line */
   ```
   This reopens a small C1 window *for the parent slot*, so it must be paired
   with a pin (the old `pool_resolve_rw` dirty-mark) or done in a tight loop
   that re-resolves on retry.

2. **Add a `pool_cas_field(pool, vptr, offset, expected, desired)` primitive**
   that resolves the page, takes the bucket lock, CASes the field in the cache
   payload, and returns. This keeps the copy-out discipline for everything
   *except* the atomic updates, which genuinely need shared memory.

3. **Serialize tree-structure mutations with a per-directory lock** and drop
   the CAS fiction entirely. If the lock table is fixed (H4) and scoped to the
   *parent* nodeId, the CAS-on-local becomes safe because no two writers can be
   in the loop at once. This is the biggest change but the most honest about
   the actual concurrency model.

Until one of these is done, concurrent directory mutations (create/create,
create/delete, rename/anything) can silently lose entries.

---

## 🟠 Specific correctness issues

### R1. `vfs_truncate` shrink path still leaks FileSize slots on CAS-retry (ISSUES.md H1, unfixed)
**Location:** `src/tree.c:2670-2706`.

The CAS is now local (so it never fails — see above), which means the
`while(1)` loop body runs exactly once and the leak doesn't trigger in
practice. But the structure is still "allocate inside the retry loop," and if
the CAS-on-local is ever fixed to target shared memory, the leak returns.
Allocate `fs_vp` once before the loop, matching `vfs_create`'s pattern.

### R2. `vfs_truncate` shrink does not take the file lock — concurrent truncate/write loses updates
**Location:** `src/tree.c:2637-2710`.

The comment at `:2693-2701` acknowledges this ("vfs_truncate does not take the
file lock … the NEW CAS gave best-effort cache-level atomicity … last-writer-
wins"). But "last-writer-wins" on the FileSize chain means a concurrent
`vfs_write` that grows the file and a `vfs_truncate` that shrinks it will race:
whichever `pool_release` fires last writes the FileNode's `SIZEPTR`, and the
loser's FileSize entry is orphaned (still allocated, not linked). The file's
reported size becomes nondeterministic. The old code had the same issue but at
least the CAS was on shared memory so the *link* was atomic; now even that is
gone.

**Direction:** take `vfs_lock(vfs, file, epoch)` at the top of `vfs_truncate`,
matching `vfs_write`.

### R3. `dircontentindex_insert` leaks the root slot on CAS-race
**Location:** `src/tree.c:1990-2003`.

```c
PoolSlot rootSlot = {0};
pool_acquire(pool, rootVP, true, &rootSlot);
...
nodes_write_dircontentindex(rootSlot.bytes, ...);
pool_release(pool, &rootSlot);
vfs_cas_i64((int64_t*)indexRoot, 0, rootVP);
```
If the CAS fails (another thread installed a root first), `rootVP` is an
orphaned allocated slot — leaked permanently (no `pool_free`). The old code had
the same leak; the migration preserves it. Also, `pool_release` writes
`rootSlot.bytes` back to the cache before the CAS, so for a brief window the
new page is initialized in the cache but unreferenced — harmless, but the
ordering is "persist then try to link," which is backwards if you care about
not dirtying pages for orphaned allocations.

**Direction:** CAS first (into a scratch slot), release only on success.

### R4. `tree_resolve_page` mutates `fc_slot.bytes` via CAS but releases with a possibly-stale local
**Location:** `src/tree.c:415-435` (the `pn_idx > page_in_segment` insert-before
branch) and `:461-482` (append-at-tail).

The CAS on `fc_slot.bytes + FILECONTENT_OFF_ROOTPTR` (`:416`) is local-only
(see the main hazard). But separately: on `goto retry_walk`, the code re-reads
`fc_page_root` from `fc_slot.bytes` (`:419`, `:431`, `:466`, `:478`) — which is
still the *stale local*. A concurrent writer that updated the cache's ROOTPTR
won't be seen, so the retry walks the old chain. With the old raw-pointer API
this worked because `fc_slot` pointed at the live cache line; now it doesn't.

**Direction:** re-acquire `fc_slot` at `retry_walk:` to pick up the current
ROOTPTR, or read ROOTPTR from the cache directly.

### R5. `vfs_rename` same-dir path writes `dc.bytes + NAMEPTR` via plain store, then releases
**Location:** `src/tree.c:1731-1733`.

```c
vfs_atomic_store_i64((int64_t*)(dc.bytes + DIRCONTENT_OFF_NAMEPTR), new_name_vp);
pool_release(&ctx->pool, &dc);
```
`vfs_atomic_store_i64` on a local is just a store. The `pool_release` then
`memcpy`s the local back to the cache. The atomic-store semantics are lost —
the write-back is a non-atomic 32-byte copy. A concurrent `dirchain_find_child`
reading the cache line sees a torn update (old bytes for some fields, new for
the renamed NAMEPTR). The old code wrote directly to the cache with the atomic
store, so readers saw a consistent atomic update. Same class as the main hazard.

---

## 🟡 Smaller issues

### R6. `pool_release` unconditionally re-marks dirty even when `storage_read` failed
**Location:** `src/pool.c:271-273`.

If the page was evicted between acquire and release (shouldn't happen when
pinned, but the pin is a dirty-mark, not a hard refcount), `storage_read`
returns NULL, the memcpy is skipped, but `cache_mark_dirty` still runs on
`page_index`. If that page index was since reallocated to a different logical
page, the dirty mark lands on the wrong page. Unlikely but the comment says
"bail safely without writing back" and then doesn't bail the dirty mark.

### R7. Compat shims keep the C1 hazard alive for tests
**Location:** `src/pool.c:191-204`, `src/tree.c:596-610`.

`pool_resolve*` and `tree_resolve_page_compat` are retained as test-only shims
that return raw pointers into the cache — i.e., they preserve exactly the C1
hazard the phase set out to fix, just scoped to tests. The migration doc
(`impl/phase-25-pool-by-value-migration.md:516-523`) acknowledges this and
flags it as future cleanup. Two concerns:

- The acceptance criterion "`pool_resolve` / `pool_resolve_ro` /
  `pool_resolve_rw` removed" is **not met** (they're shimmed, not removed). The
  doc's checkbox at `:563` remains unchecked.
- `tree_resolve_page_compat` uses a **rotating array of 128 thread-local
  PoolSlots** (`tree.c:592-594`). A caller that resolves >128 pages without
  releasing will silently alias an earlier slot and corrupt it. Tests that
  walk long chains could hit this. The shim has no wrap detection.

### R8. `pool_acquire` failure does not set `last_error`
**Location:** `src/pool.c:211-228`.

The spec (`impl/phase-25-...md:189-203`) says acquire failures should surface
via `ctx->last_error = VFS_ERR_IO`. The implementation just zeroes `bytes` and
returns — no error is set. Callers check `slot.vptr == VFS_VPTR_NULL` and set
their own error codes, which is workable, but inconsistent (some set NOTFOUND,
some IO, some NOMEM). A centralized error in `pool_acquire` would have been
cleaner and is what the spec promised.

### R9. `vfs_write` reads `file_nodeId` from the local but never uses it after `tree_resolve_page` re-acquires
**Location:** `src/tree.c:2808` reads `file_nodeId`; `:2947` re-acquires
`file_slot`. The `file_nodeId` read at `:2808` is from the pre-re-acquire local
and is never used (grep shows no subsequent use). Dead read, harmless, but
worth removing for clarity.

---

## 🟢 Style / consistency

- **R10.** The `PoolSlot` initializer is inconsistent: most sites use `PoolSlot
  x = {0};`, but `vfs_write` uses `PoolSlot file_slot;` (uninitialized) at
  `:2779` and `:2960`, and `PoolSlot vp_new_slot;` at `:2906`, `PoolSlot
  dst_dc_slot;` at `:1801`. The `{0}` is important because `pinnedPage` must
  start at 0 for a stray release to no-op. The uninitialized ones happen to be
  `pool_acquire`'d before any release, so it's not a live bug, but it's a
  latent footgun if someone adds an early return.

- **R11.** Many error-path `pool_release` calls are repeated 2-3× for the same
  slot on adjacent lines (e.g., `tree.c:2852`, `2881`, `2885`, `2905` each
  release `pn_slot` and `file_slot`). A `goto err;` cleanup pattern would be
  shorter and less error-prone than the linear repetition. Not a bug.

- **R12.** The doc's per-workload status notes (W1-W9) reference bench numbers
  and FUSE scenarios that aren't reproducible from the code alone. They're
  useful history but the file is now ~670 lines of spec + status; consider
  splitting status into commit messages and keeping the spec as the target.

---

## Summary

The C1 hazard (cache pointer invalidated mid-use) is **structurally closed**
for all production paths — the copy-out discipline is correct and uniform, and
the read-heavy majority of call sites are clean. The migration was
well-executed mechanically across 116 sites.

The **new hazard** (CAS-on-local) is the thing to fix next: the prepend/COW
CAS loops in `vfs_create`, `vfs_rename`, `dircontentindex_insert`,
`tree_resolve_page`, `vfs_truncate`, and `vfs_write` no longer coordinate
across threads because they operate on per-thread stack buffers instead of the
shared cache line. Under the documented concurrency model (which the code
claims to support via these CASes), concurrent directory mutations can lose
entries. This is the same severity class as C1 and should be tracked as a
follow-up — either route the CASes back through the cache (option 1/2 above)
or make the per-directory locking real and stop pretending the CAS does
anything (option 3).

The per-site issues (R1-R9) are mostly pre-existing problems the migration
inherited rather than introduced, except R5 (`vfs_rename` atomic-store-on-local)
which is newly broken by the copy-out.
