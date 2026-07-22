# Phase 28 Bin Job: Rename Tombstone Removal — Implementation Review

Review of the rename-tombstone bin job implementation (commits `ad6a66c`..
`7305202`, 6 workloads W1-W6). Tests: **16/16 pass** (ctest, 162s).
Build: clean.

## Verdict

The implementation is **structurally complete and functionally correct** for
the tested cases. The critical B3 CAS pattern (live cache payload, not stack-
local) is correctly applied — no regression from the file-deletion bug. The
OLD NameEntry is freed unconditionally (no tree-wide lookup). Multi-slot
NameEntry chains are freed. The vfs_rename push site is correctly wired.

Two issues need fixing: **`dircontentindex_remove` is not called** (leaves
stale radix links), and **`process_slot_entry` frees a still-linked SlotNode**
when the SlotNode is mid-chain (corrupts the segment chain). Three test gaps
(cross-dir, diff-epoch, OLD-name-reuse) should be filled. One latent code-
quality issue (use-after-release in `create_reachable_at`).

---

## ✅ What's correct

### W1: Constants + dispatch — correct
`BIN_TRIGGER_TOMBSTONE_ADDED = 2` (`bin.h:64`), `BIN_WORK_REMOVE_TOMBSTONE = 0x101` (`bin.h:87`). Dispatch cases at `gc.c:1494-1495` (trigger) and `gc.c:1513-1514` (work).

### W2: Analysis handler — correct
`gc_handle_rename_done` (`gc_bin_rename_tombstone.c:445-629`). Reads `file_vp` from context, `tombstone_vp` from context2. Finds src SlotNode via `find_parent_dir_rename` + `find_slotnode_in_dir_rename`. Walks chain, identifies tombstone (`namePtr==0`) and create (`namePtr!=0`). Freeability checks correct (tombstone needed iff R ≥ tombstone_epoch; create needed iff R ∈ [create_epoch, tombstone_epoch)). Same-dir (`context2==0`) handled: skips tombstone, only frees create.

### W3: Work handler — correct (critical B3 CAS verified)
`gc_handle_remove_tombstone` (`gc_bin_rename_tombstone.c:880-932`). **CAS uses `live_pool_page` / `cas_head_ptr_live` / `cas_next_ptr_live`** — the B3 pattern from file-deletion. NOT the stack-local bug. `pool_free` present for both DC slots (`:767`) and NameEntry slots (`:795`). Multi-slot NameEntry chain walked and freed (`process_name_entry`, `:778-801`).

### W4: vfs_rename push — correct
`src_dc_vp` hoisted to function scope (`tree.c:2425`). Single push site at `tree.c:2903-2904`: `BIN_TRIGGER_TOMBSTONE_ADDED` with `rn_childPtr` as context, `src_dc_vp` as context2 (0 for same-dir).

### W5: Tests — 3 integration tests pass
`test_rename_tombstone_bin_job_basic` (same-dir, no-snapshot), `test_rename_tombstone_with_active_snapshot`, `test_rename_no_space_leak`. All pass (279/279 in test_gc_thread).

---

## 🟠 Issues to fix

### I1. `dircontentindex_remove` not called — stale radix links

**Location:** `gc_bin_rename_tombstone.c:748-765`

The spec (§4.2 step 4) requires `dircontentindex_remove` to clear the radix-
tree index link for the removed DC. The implementation skips this with a
TODO comment: `"TODO(W3+): pass name_hash in the batch entry"` (`:765`).

**Impact:** After a rename's tombstone/create is CAS-removed from the chain,
the radix index still has a link pointing at the (now-removed) DirContent.
Subsequent `dirchain_find_child` calls that hit the radix index may follow
the stale link, find an empty/invalid SlotNode, and fall through to the chain
walk. This is functionally correct (the chain walk finds the right answer)
but degrades the radix index's effectiveness. Under heavy rename workloads,
the index accumulates stale links.

The file-deletion reference implementation correctly calls `dircontentindex_remove`
at `gc_bin_file_deleted.c:271`.

**Fix:** Add `name_hash` and `parent_dir_vp` fields to the batch entry struct
(or encode them into the existing `context_vp` field), and call
`dircontentindex_remove` in the work handler after CAS-removing the DC.

### I2. `process_slot_entry` frees a still-linked SlotNode — chain corruption

**Location:** `gc_bin_rename_tombstone.c:863-872`

When a SlotNode is NOT the segment head (it's mid-chain in the DirSegment's
SlotNode chain), the code has a `(void)0; /* placeholder */` for the CAS-
remove from the segment chain, then unconditionally calls `pool_free(slot_vp)`
(`:872`). This frees a SlotNode that is **still linked** in the DirSegment's
chain — corrupting the chain for any subsequent walk.

The spec §9.5 flagged this as an open question. The same-dir test passes
because the SlotNode ends up as the segment head (where the headPtr CAS
path handles it correctly). But for a cross-dir rename where the file is the
Nth child (not the first), the SlotNode is mid-chain and `pool_free` would
corrupt the chain.

**Fix:** Either implement the mid-chain CAS-remove (walk the SlotNode chain
to find the predecessor, CAS its `sibPtr` to skip the target), or skip the
SlotNode removal entirely for mid-chain nodes (leave the empty SlotNode in
the chain — it's a small leak but not corruption). Skipping is safer for now.

---

## 🟡 Test gaps

### T1. Cross-dir rename not covered by unit tests
The spec §8.1 requires `test_rename_tombstone_freed_cross_dir_no_snapshot`.
The existing `test_rename_tombstone_bin_job_basic` tests same-dir only.
Cross-dir is where the tombstone is actually created (`context2 != 0`), so
it's the more important case.

### T2. Diff-epoch rename not covered
The spec requires `test_rename_create_freed_diff_epoch_no_snapshot`. This
tests the create-freeability check when `create_epoch < tombstone_epoch` and
no active snapshot falls in between.

### T3. OLD-name-reuse not covered
The spec requires `test_rename_OLD_name_referenced_by_another_file`. This
tests that the OLD NameEntry is NOT freed when another file's DC references
it. Per M1, this case is theoretical (each create has its own NameEntry),
but the test validates the assumption.

---

## 🟢 Low

### L1. Use-after-release in `create_reachable_at`
`gc_bin_rename_tombstone.c:417-418`: reads `picked_epoch` from `dc.bytes`
after `pool_release(&dc)` at `:409`. The cache page may still be resident
(works in practice), but it's fragile. Fix: read the epoch before releasing.

### L2. Defensive name-match check is vestigial
`gc_bin_rename_tombstone.c:548-555`: acquires the NameEntry's first slot but
only checks `vptr != VFS_VPTR_NULL` — never compares the name bytes. The
defensive check doesn't actually defend against slot reuse. Minor (M1 says
the case is theoretical).

---

## Summary

The rename-tombstone bin job is correctly implemented for the tested paths.
The critical B3 CAS pattern (live cache payload) is properly applied — no
regression. The OLD NameEntry unconditional free (M1 resolution) works.
Multi-slot NameEntry chains are freed. The vfs_rename push site is correctly
wired.

**Fix I1 (dircontentindex_remove)** and **I2 (mid-chain SlotNode
corruption)** before cross-dir rename workloads are exercised. Both are
straightforward — I1 needs the batch entry expanded to carry the name hash;
I2 needs either a mid-chain CAS-remove or a "skip if not head" guard.

Fill the three test gaps (T1-T3) to match the spec's test plan. Fix L1
(use-after-release) for code quality.
