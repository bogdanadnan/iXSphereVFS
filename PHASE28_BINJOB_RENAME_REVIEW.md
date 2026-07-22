# Phase 28 Bin Job: Rename Tombstone Removal — Spec Review

Review of `impl/phase-28-bin-job-rename-tombstone.md` against the current
codebase and the file-deletion bin job spec (`impl/phase-28-bin-job-file-deletion.md`).

## Verdict: **Ready for implementation with one design clarification needed.** ✅

The spec is well-structured and follows the file-deletion bin job's proven
patterns correctly. The trigger/work split, reference-point analysis, CAS-on-
live-payload (B3 fix), pool_free integration, and crash safety are all
consistent with the approved file-deletion spec. The test plan is
comprehensive.

One design question (the OLD NameEntry reclamation's tree-wide lookup cost)
needs a decision before W2. Two medium items and two low items noted.

---

## 🟡 Medium

### M1. OLD NameEntry reclamation requires a tree-wide lookup — potentially expensive

**Spec §3.6 (line 436-443):** To determine whether the OLD NameEntry is
still referenced, the analysis does "a name-hash lookup in the radix tree of
ALL dirs." This is O(D × radix_depth) where D is the number of directories.
For a VFS with 10,000 directories, this is 10,000 radix lookups per rename
trigger.

**Current codebase context:** `dircontentindex_lookup` (`tree.c:2917`) is a
per-directory radix lookup. There's no cross-directory name-lookup mechanism.
The spec's approach requires walking the entire directory tree, visiting every
DirNode, and checking its radix index for the name hash. This is the
`find_parent_dir` pattern (which already exists in `gc_bin_file_deleted.c`)
but applied to every directory, not just one.

**The spec's §9.2 acknowledges this** and recommends "do the lookup" because
"the cost is amortized." But for a rename-heavy workload (e.g., atomic-save
patterns: write to .tmp, rename to final — hundreds per second), the per-
rename tree walk is a real concern.

**Alternative:** The OLD NameEntry is referenced by exactly one thing: the
create DirContent's `namePtr` field. When the create is CAS-removed from the
chain, no remaining DirContent can reference the OLD NameEntry (unless a
different file was created with the same name — in which case it has its OWN
NameEntry at a different VP, not the OLD one). So the OLD NameEntry can
safely be freed unconditionally when its create is freed — **no tree-wide
lookup needed.**

The only exception is if two files shared the same NameEntry VP (impossible —
each `vfs_create` allocates its own NameEntry via `nodes_write_name`). So
the lookup is always unnecessary.

**Direction:** Skip the tree-wide lookup. Free the OLD NameEntry
unconditionally when its create DirContent is freed. This eliminates the
performance concern and simplifies the spec significantly.

### M2. `src_dc_vp` is not available at the push site for same-dir renames

**Spec §2.4 (line 185-191):**
```c
if (cross_dir) {
    bin_push(ctx->sb, BIN_TRIGGER_TOMBSTONE_ADDED,
             (int64_t)rn_childPtr, (int64_t)src_dc_vp);
} else {
    bin_push(ctx->sb, BIN_TRIGGER_TOMBSTONE_ADDED,
             (int64_t)rn_childPtr, 0);
}
```

**Codebase verification:** `src_dc_vp` is allocated at `tree.c:2789` inside
the `if (cross_dir)` block. It's NOT available outside that block. The push
at `tree.c:2883` is after the `cross_dir` block closes (`tree.c:2860`).
So `src_dc_vp` is out of scope at the push site.

**But:** the spec's push site (line 185-191) shows the push INSIDE the
`if (cross_dir)` branch for the cross-dir case and a separate push for the
same-dir case. This means the push would need to be duplicated — one push
inside the `cross_dir` block (with `src_dc_vp`), one push outside (with
`context2=0`). This is slightly awkward.

**Alternative:** Move `src_dc_vp`'s declaration to the function scope (before
the `if (cross_dir)` block), initialize to 0, and set it inside the block.
Then one push at `tree.c:2883` with `context2 = src_dc_vp` (0 for same-dir).

**Direction:** Declare `int64_t src_dc_vp = 0;` at function scope. The
existing code already has `src_dc_vp` as a local inside the `if` block — just
hoist the declaration.

---

## 🟢 Low

### L1. Same-dir rename: spec says "new_dc directly shadows the create" — verify

**Spec §2.3 (line 156-160):** "Same-dir: the new_dc is at the head; the
create is below."

**Codebase:** For same-dir rename, `vfs_rename` at `tree.c:2612` does:
```c
nodes_write_dircontent(dc.bytes, rn_childId, (uint32_t)epoch,
                       rn_childPtr, new_name_vp, fshead, ctx->page_size);
```
This writes a new DirContent (`dc`) into the SlotNode's chain, prepended
before the old entries. The old `create` is still in the chain at a lower
position. The spec's description is correct.

But the spec says "the new_dc directly shadows the create" — more precisely,
the new_dc shadows ALL lower-epoch entries in the chain (not just the create).
The spec's wording is a simplification; it's correct for the common case
(create + new_dc, no intervening entries).

### L2. Open question §9.4 (multiple creates) is resolved by the spec's own analysis

The spec asks "do we free just the highest create, or walk the entire chain?"
and recommends walking the entire chain. This is correct and consistent with
the file-deletion bin job's approach (walk the entire chain, classify each
entry). The question is more of a confirmation than an open issue.

### L3. `epoch_rename` not available in the trigger context

The spec §3.4 says "the one with `epoch == epoch_rename`" to identify the
tombstone. But the trigger's context only carries `file_vp` and
`tombstone_vp` — not the rename epoch. The analysis would need to read the
tombstone's epoch from its DirContent slot (via `pool_acquire` + read
`DIRCONTENT_OFF_EPOCH`). This is correct but not explicitly stated in the
spec. A one-line clarification would help the implementer.

---

## What the spec gets right

1. **Trigger/work split is correct and consistent** with the file-deletion
   bin job. The trigger carries `file_vp` + `tombstone_vp`; the analysis
   does the chain walk + classification; the work handler does the CAS +
   pool_free.

2. **The reference-point analysis is correct.** The tombstone is needed iff
   some active snapshot R ≥ tombstone_epoch would see the tombstone (no
   namePtr≠0 entry at src picks first at R). The create is needed iff some
   R ∈ [create_epoch, tombstone_epoch) is active. Both match the epoch model.

3. **The CAS-on-live-payload (B3 fix) is correctly reused.** The spec
   explicitly references `live_pool_page`, `cas_head_ptr_live`,
   `cas_next_ptr_live` from `gc_bin_file_deleted.c`. No new CAS-on-local
   hazard.

4. **The `pool_free` integration is correct.** The work handler calls
   `pool_free` for the tombstone DC, create DC, and OLD NameEntry slots.
   `pool_free` is already implemented (Phase 28 W6, CAS-based).

5. **The same-dir handling is correct.** `context2 = 0` signals "no
   tombstone"; the analysis skips the tombstone branch and only frees the
   create + OLD NameEntry.

6. **The test plan is comprehensive.** 5 unit tests (cross-dir, diff-epoch,
   with-snapshot, same-dir, name-reuse), 3 integration tests (basic,
   with-snapshot, no-leak), 2 crash tests. The "no space leak" test verifies
   `pool_alloc_count` returns to baseline.

7. **The crash safety story is consistent** with the framework's. The
   trigger is durable (Bin persistence). Mid-drop crashes leave a partially-
   modified chain that the re-run cleans up. Work entry batch list is in
   pool slots (durable).

8. **The open questions (§9) are well-analyzed.** Each has a recommendation
   with justification. The recommendations are sound (push for same-dir,
   walk the entire chain, remove empty SlotNodes, defer batch-slot free to
   TODO-12).

---

## Recommendations

1. **Resolve M1 (OLD NameEntry lookup).** Free unconditionally when the
   create is freed — no tree-wide lookup needed. Each `vfs_create` allocates
   its own NameEntry; no sharing. This eliminates the performance concern
   and simplifies the spec by ~20 lines.

2. **Resolve M2 (src_dc_vp scope).** Hoist the declaration to function
   scope. One push site, `context2 = src_dc_vp` (0 for same-dir).

3. **Add L3 clarification.** Note that `epoch_rename` is read from the
   tombstone's DirContent slot (not carried in the trigger context).

With these resolved, the spec is ready for implementation. The bin job is
structurally simpler than file-deletion (no data pages, no mirrors, no
VersionPage classification) — it's purely metadata cleanup with CAS-based
chain removal and pool slot reclamation.
