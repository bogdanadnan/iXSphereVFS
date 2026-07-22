# Phase 28 — Bin Job: Rename Tombstone Removal

**Status:** DRAFT for external review
**Date:** 2026-07-20
**Spec family:** Phase 28 per-bin-job specs (sibling of `phase-28-bin-job-file-deletion.md`)
**Review target:** external reviewer for sign-off BEFORE implementation

This spec is for the vfs_rename GC path. It follows the same
trigger-and-work pattern as the file-deletion bin job (Type 1,
approved `4a569c1`) but is much smaller: no data pages, no mirror
handling. The work is **purely metadata cleanup** — the analysis
finds the tombstone at src (and possibly the create at src) and the
work handler CAS-removes it from the chain, plus `pool_free` for
the slots. Goal: **after GC runs, no space leaks from a rename**.

---

## Constants (per the L1 finding from the file-deletion review)

```c
#define BIN_TRIGGER_TOMBSTONE_ADDED   2   /* Phase 28 Type 2: rename
                                            bin job (tombstone +
                                            possibly create at src) */
#define BIN_WORK_REMOVE_TOMBSTONE     0x101  /* Phase 28 Type 2 work:
                                              CAS-remove + free
                                              tombstone (and create,
                                              if unreachable) slots
                                              at src + free OLD
                                              NameEntry */
```

`BIN_TRIGGER_TOMBSTONE_ADDED` is the next unused trigger slot (1
is the file-deletion trigger; 0 is NOOP). `BIN_WORK_REMOVE_TOMBSTONE`
is the next unused work slot (0x100 is the free-pages work; work
values are `BIN_TYPE_WORK_THRESHOLD + N`).

The `T` values must be defined in `src/bin.h` next to the existing
file-deletion definitions. The dispatch in `src/gc.c::gc_handle_trigger`
and `src/gc.c::gc_handle_work` must add the new cases.

---

## Mental model (prerequisites)

The reader must understand (in order):

1. **Phase 28 GC framework** (`impl/phase-28-gc.md`) — Bin push/pop,
   per-entry dispatch, GC thread lifecycle, the NOOP trigger pattern.
2. **File-deletion bin job** (`impl/phase-28-bin-job-file-deletion.md`)
   — Trigger/work split, reference-point check (`file_visible_at_R`),
   drop order (CREATE first then tombstone), B3 fix (CAS on live
   cache payload), W6+ `pool_free` integration.
3. **DirContent + SlotNode structure** (`src/nodes.h`) — chain walk,
   anchor read/write, `ANCHOR_OFF_HEADPTR`, `ANCHOR_OFF_SIBPTR`,
   `DIRCONTENT_OFF_*` offsets.
4. **Epoch model** (from the corrected memory, 2026-07-18):
   - Even epochs = live head; only the LAST even (currentEpoch) is
     writable.
   - Odd epochs = snapshot branches (frozen head images).
   - Epochs always advance by 2: `E_prev → E_next` with a snapshot
     `E_prev + 1` taken from `E_prev` in between (after a snapshot).

The read rule (per `src/gc_bin_file_deleted.c::read_rule_pick_first_dc`)
applies unchanged here. The same `file_visible_at_R` helper is used.

---

## §1. Name and motivation

### §1.1 Name

**vfs_rename bin job (Phase 28 Type 2)** — releases the chain slots
and OLD NameEntry created by `vfs_rename` once they're no longer
reachable.

### §1.2 Motivation

`vfs_rename` allocates per-call:
- `dst_name_vp` — a new NameEntry (for the new name) — **kept** (referenced by new DC).
- `dst_dc_vp` — a new DirContent at dst — **kept** (primary entry in dst chain).
- `dst_slot_vp` — a new SlotNode at dst parent (cross-dir only) —
  **kept** (in dst parent's DirSegment).
- `src_dc_vp` — a tombstone DC at src (cross-dir only) — **freed
  by this spec** when unreachable.
- The pre-existing `src_name_vp` (OLD NameEntry) is left untouched
  by `vfs_rename` — **freed by this spec** when its DC is freed.

For different-epoch renames (the common case), `vfs_rename` also
leaves a `create` DC at src in shadow state (reachable at the
original epoch via snapshots that were taken between create and
rename). When the original epoch is no longer referenced, the
`create` DC is also unreachable — **freed by this spec**.

**Current state (without this bin job):**

- `vfs_rename` pushes `BIN_TRIGGER_NOOP` placeholder
  (`src/tree.c:2883`).
- The tombstone, the shadowed `create`, and the OLD NameEntry leak
  per-rename. For a delete-heavy workload with frequent renames
  (e.g., temp-file patterns: create, rename to .partial, rename to
  final, delete), the leak is ~3 pool slots per rename plus a
  NameEntry. The NameEntry is small (~16 bytes header + name bytes,
  rounded to 32 bytes slot), but the pattern matters at scale.

**After this spec:**

- `vfs_rename` pushes `BIN_TRIGGER_TOMBSTONE_ADDED`.
- The GC's analysis handler re-evaluates the source chain each pass
  (idempotent re-run) and frees what's unreachable.
- Work handler `BIN_WORK_REMOVE_TOMBSTONE` does the CAS-removes and
  `pool_free` calls.
- **No space leak from a rename after GC drains the bin.**

This is the **complete version** requested: tombstone + create (when
unreachable) + OLD NameEntry.

---

## §2. Trigger and work types

### §2.1 Trigger: `BIN_TRIGGER_TOMBSTONE_ADDED`

- **Type tag:** `BIN_TRIGGER_TOMBSTONE_ADDED = 2`
- **Context:** `file_vp` — the FileNode slot of the renamed file
  (same as the file-deletion trigger's context).
- **Context2:** `tombstone_vp` — the tombstone DC slot that
  `vfs_rename` just prepended to the src SlotNode's chain (for
  cross-dir rename). For same-dir rename, the trigger is still
  pushed but `context2 = 0` (no tombstone was created; the analysis
  falls through to "create-only" cleanup).
- **Idempotency:** the analysis re-walks the chain each pass, so
  re-pushing the same trigger is safe (a no-op if everything is
  already cleaned up).

### §2.2 Work: `BIN_WORK_REMOVE_TOMBSTONE`

- **Type tag:** `BIN_WORK_REMOVE_TOMBSTONE = 0x101`
- **Context:** head of a per-trigger linked list of chain entries
  to remove. The list is small (1-2 entries max for a rename: the
  tombstone, plus possibly the create). Allocated in freshly-pooled
  slots, same pattern as `BIN_WORK_FREE_PAGES`.
- **Context2:** count of entries in the list.
- **Idempotency:** the work handler is idempotent. If the entry is
  no longer in the chain (e.g., a concurrent operation moved it),
  the work handler treats it as already-done and returns.

### §2.3 Same-dir vs cross-dir handling

The trigger is pushed by `vfs_rename` for **both** same-dir and
cross-dir renames. The analysis handler determines what to clean up:

- **Cross-dir**: src chain has `[tombstone(epoch), create(epoch_prev) ?]`.
  Analysis can free: tombstone (always, if unreachable) + create
  (if unreachable) + OLD NameEntry.
- **Same-dir**: src SlotNode is the same as dst SlotNode; the chain
  is `[new_dc(epoch), create(epoch_prev) ?]`. The new_dc is
  the "new" entry; the create is the "old" entry. No tombstone
  was added — the new_dc directly shadows the create. Analysis can
  free: create (if unreachable) + OLD NameEntry. The new_dc is
  NOT freed (it's the primary entry in the SlotNode now).

For same-dir, `context2 = 0` (no tombstone). The analysis detects
this and skips the tombstone-removal branch.

### §2.4 Producer push call site

`src/tree.c:2883` (currently the NOOP placeholder). Per the
review's **M2**, hoist `src_dc_vp` to function scope so the push
is a single call site (not duplicated inside the `cross_dir`
block).

At the top of `vfs_rename` (after the lock acquisition and parent
slot pinning), add:

```c
int64_t src_dc_vp = 0;  /* Tombstone DC at src. 0 for same-dir. */
```

Inside the existing `if (cross_dir)` block at `tree.c:2789`,
`src_dc_vp` is already set by the existing code (just rename
the local or rely on the hoisted declaration).

At the push site (`src/tree.c:2883`):

```c
/* Phase 28 Type 2 (spec: impl/phase-28-bin-job-rename-tombstone.md):
   push BIN_TRIGGER_TOMBSTONE_ADDED to the Bin.  The GC's analysis
   handler will:
     1. Find the src SlotNode via the file's childId.
     2. Walk the chain to find the tombstone (cross-dir, if any)
        and the create (always).
     3. Determine which are freeable based on active snapshots.
     4. If freeable: CAS-remove from chain, dircontentindex
        update, pool_free the DC slots + the OLD NameEntry.
   context  = file VP (rn_childPtr).
   context2 = src_dc_vp (the tombstone vfs_rename just prepended
              for cross-dir renames).  0 for same-dir renames
              (no tombstone; the analysis falls through to
              create-only cleanup). */
bin_push(ctx->sb, BIN_TRIGGER_TOMBSTONE_ADDED,
         (int64_t)rn_childPtr, (int64_t)src_dc_vp);
```

---

## §3. Analysis (trigger handler)

The analysis handler is `gc_handle_rename_done(vfs, entry)`. It runs
on the GC thread, called by `gc_process_entry` after `bin_pop`
returns the trigger.

### §3.1 Sanity check

If the file slot or the tombstone slot no longer exists (e.g., the
VFS was closed and the slots were re-allocated by other work), the
trigger is a no-op. Return `VFS_OK`.

### §3.2 Determine reference points

Same as file-deletion (§3.4 of that spec):

```c
ref_points_t rp = {0};
rp.H = ctx->currentEpoch;
collect_active_snapshots(ctx, &rp);
```

The reference points are `{H, active[0], active[1], ...}`.

### §3.3 Find the src SlotNode

The analysis needs the SlotNode VP at src. The file is identified by
its FileNode VP (`file_vp`) and its `childId` (read from FileNode).
The walk:

1. `find_parent_dir(ctx, file_vp, &src_parent_vp)` — tree walk from
   root to find the dir whose chain contains a DirContent with
   `childPtr == file_vp` AND `namePtr != 0` (the original create).
   The `namePtr != 0` filter distinguishes the create from the
   tombstone (which has `namePtr == 0`).

   **Caveat for same-dir rename:** after the rename, the create
   is shadowed by the new_dc in the same SlotNode. The `namePtr != 0`
   filter on the find still finds the create (we walk the chain and
   look for an entry with namePtr != 0, regardless of read-rule
   visibility). Same code as file-deletion's
   `find_create_in_slot`.

2. `find_slotnode_in_dir(ctx, src_parent_vp, file_childId, &slot_vp)`
   — walk the dir's DirSegments → SlotNodes, find the one whose
   `nodeId == file_childId`.

For same-dir rename, the SlotNode found is the SAME SlotNode as
the dst (since src_parent == dst_parent and the file has one
SlotNode in the parent). The analysis correctly identifies it via
the `childId` lookup, not the `childPtr` (since the new_dc also
has `childPtr == file_vp`).

### §3.4 Walk src chain to find the tombstone and the create

The chain at src is one of:
- Cross-dir: `[tombstone(epoch_rename), create(epoch_create), ...]`
  — possibly more entries (e.g., from a previous rename of the same
  file to the same name and back).
- Same-dir: `[new_dc(epoch_rename), create(epoch_create), ...]`
  — the new_dc is at the head; the create is below.

Walk the chain:
- For each DC entry, read `namePtr`, `epoch`, and `childPtr`.
- Tombstone detection: `namePtr == 0` AND `childPtr == file_vp`
  AND `epoch == epoch_rename`. The `epoch_rename` is **read from
  the tombstone's DirContent slot** (the trigger's `context2` is
  just the tombstone's VP — per the review's **L3**, the trigger
  does NOT carry the rename epoch). The tombstone detection uses
  the tombstone's `epoch` field as the rename epoch.
- Create detection: `namePtr != 0` AND `childPtr == file_vp`. The
  create we want to free is the OLD entry that was the "current"
  entry BEFORE the current rename.  This is the entry at the
  head's NEXTPTR in the SlotNode chain (i.e., the entry that the
  current rename's new entry was prepended on top of).  For
  same-dir rename, the head is the new_dc and the OLD create is
  at head.next.  For cross-dir rename, the head is the tombstone
  and the OLD create is at tombstone.next.

  **IMPORTANT**: the previous wording "pick the highest epoch
  among creates" was **wrong** for the bin job's purpose.  The
  bin job frees the OLD entry (the one that was current BEFORE
  the current rename), not the most recent entry.  Picking the
  most recent would free the CURRENT entry, making the file
  unreachable from the head.  This is corrected in the W5 fix
  (the "create" is the head's next, not the highest epoch).

Per-L3 implementation note: the trigger's `context2 = tombstone_vp`
is used to find the chain initially (via `dirchain_find_slotnode`).
The rename epoch is read from the tombstone's slot itself
(`pool_acquire(tombstone_vp) → read DIRCONTENT_OFF_EPOCH`). This
avoids carrying the epoch in the trigger context (saves 8 bytes
and matches the file-deletion bin job's trigger structure).

Cache the result:
- `tombstone_vp_found` (0 if not in the chain — happens for
  same-dir or if the tombstone was already removed by a prior
  pass).
- `tombstone_epoch` (read from the entry, even if `tombstone_vp_found == 0`,
  for use in the file_visible check).
- `create_vp_found` (0 if not in the chain).
- `create_epoch` (highest epoch among creates, or 0 if none).

If neither tombstone nor create is in the chain, the trigger is
fully resolved; return `VFS_OK`.

### §3.5 Determine which entries are freeable

For each candidate (tombstone and create), check whether any
reference point still needs to see it.

**Tombstone freeable iff:** for every reference point R:
- If R >= tombstone_epoch: the tombstone is visible at R (it
  matches the read rule at R; the chain walk picks it first). The
  tombstone is NEEDED. **Not freeable.**
- If R < tombstone_epoch: the tombstone is invisible at R (it's
  an even above R or, for odd R, future). The tombstone is
  NOT NEEDED at R. (But this is per-R; we need ALL R to not need
  the tombstone for it to be freeable overall.)

Concrete check: any active snapshot at R >= tombstone_epoch? If
yes, the tombstone is needed (the snapshot would see "name not
present at src"). If no, the tombstone is freeable.

This is the same `file_visible_at_R` check as file-deletion, but
with R = tombstone_epoch. Since the tombstone has `namePtr == 0`,
the function returns 0 (file not visible at R) — the file
**does** have an entry at R (the tombstone), but the entry is
a tombstone, which the helper considers as "name hidden". So the
helper's return value is **inverted** for the tombstone check:

> `file_visible_at_R` returns 1 iff the picked DC has `namePtr != 0`.
> For the tombstone, `namePtr == 0`, so the helper returns 0.
> The tombstone is NEEDED when 0 is returned (the file is hidden,
> which is the tombstone's purpose).

So the tombstone freeable check is:
```c
int tombstone_needed = 0;
for (int ri = 0; ri <= rp.num_active; ri++) {
    int64_t R = (ri == 0) ? rp.H : rp.active[ri - 1];
    if (R < tombstone_epoch) continue;
    /* R >= tombstone_epoch: tombstone is at the right epoch to
       be picked.  Check if any entry with namePtr != 0 picks first
       (e.g., a create at the same or higher epoch in the chain). */
    if (tombstone_is_in_chain_at_R(ctx, slot_vp, R, tombstone_vp_found)) {
        tombstone_needed = 1;
        break;
    }
}
```

Where `tombstone_is_in_chain_at_R` is similar to
`read_rule_pick_first_dc` but returns 1 iff the picked DC is the
tombstone (matching VP `tombstone_vp_found` or, if not found, the
first entry with `epoch == tombstone_epoch` and `namePtr == 0`).

Actually, a simpler check: the tombstone is needed iff there
exists a reference point R >= tombstone_epoch AND no entry
in the src chain at epoch >= tombstone_epoch (other than the
tombstone) has `namePtr != 0` AND is visible at R. The read rule
would pick the first `namePtr != 0` entry with the right epoch.
If no such entry exists, the tombstone is needed (it's the
"file hidden" marker).

In the typical case (the tombstone is the latest entry, and the
new_dc at dst is the primary "file visible" entry), the tombstone
is needed at R >= tombstone_epoch as long as a query at R at
src would see the tombstone (no name-visible entry at src in the
chain).

So a more direct check:

```c
/* The tombstone is needed iff there exists R >= tombstone_epoch
   such that the src chain at R has no namePtr != 0 entry that
   would be picked before the tombstone. */
int tombstone_needed = 0;
for (int ri = 0; ri <= rp.num_active; ri++) {
    int64_t R = (ri == 0) ? rp.H : rp.active[ri - 1];
    if (R < tombstone_epoch) continue;
    /* Walk the src chain at R; if the read rule would pick a
       namePtr != 0 entry, the tombstone is shadowed (not
       needed for the "name hidden" semantic); if it would pick
       the tombstone or a later entry with namePtr == 0, the
       tombstone IS needed. */
    int64_t picked_vp = 0;
    read_rule_pick_first_dc(ctx, slot_vp, R, &picked_vp);
    if (picked_vp == 0) continue;  /* no entry at R; tombstone is irrelevant */
    /* Check the picked entry: is it the tombstone? */
    PoolSlot dc = {0};
    pool_acquire(&ctx->pool, picked_vp, false, &dc);
    if (dc.vptr == VFS_VPTR_NULL) continue;
    int64_t picked_namePtr = vfs_rd8_s(dc.bytes, DIRCONTENT_OFF_NAMEPTR, ctx->page_size);
    pool_release(&ctx->pool, &dc);
    if (picked_namePtr == 0) {
        /* Picked is the tombstone (or another namePtr==0 entry).
           The tombstone is needed at this R. */
        tombstone_needed = 1;
        break;
    }
}
```

**Create freeable iff:** the create's epoch < tombstone's epoch
AND for every reference point R:
- If R == create_epoch: the create is visible at R (R = E, even,
  the create is at E, picked by read rule). The create is
  NEEDED at R. **Not freeable** if R == create_epoch is
  referenceable.
- If E < R < tombstone_epoch: the create is visible at R (R > E
  and even, or R odd and E < R; the create is the highest
  even < R in the chain). The create is NEEDED. **Not freeable**
  if any such R is referenceable.
- If R >= tombstone_epoch: the create is shadowed by the
  tombstone. Not needed at R.
- If R < create_epoch: the create is invisible (above R). Not
  needed at R.

Simplification: the create is needed iff any R in
`[create_epoch, tombstone_epoch)` is referenceable. The only R
in that range that's typically referenceable is:
- `currentEpoch` (if `currentEpoch` is in `[create_epoch, tombstone_epoch)`)
  — rare, since currentEpoch is usually >= tombstone_epoch
  (the tombstone is at the latest epoch).
- An active snapshot at some odd R in `[create_epoch, tombstone_epoch)`
  — a snapshot taken between create and rename.

So the check is: any active snapshot at R in `[create_epoch, tombstone_epoch)`?

If no, the create is unreachable. Free it.
If yes, the create is reachable at that snapshot. Don't free.

**Special case: same-epoch rename** (create_epoch == tombstone_epoch):

The range `[create_epoch, tombstone_epoch)` is empty. The create
is unreachable. Free it immediately.

**Same-dir special case**: there's no tombstone. The tombstone's
freeable check is skipped. The create's freeable check is the same
(the create is at the same SlotNode, shadowed by the new_dc at
epoch_rename). The analysis only frees the create + OLD NameEntry.

### §3.6 Free what's freeable

For each freeable entry (tombstone and/or create):
1. Allocate a small per-entry pool slot via `alloc_pool_slot_32`
   (existing helper, same pattern as `batch_dead_pages_into_pool_list`).
2. Write the entry's VP, the entry type, and a "free" flag.
3. Link the slots into a per-trigger list (head in the work entry).

If the SlotNode's chain becomes empty after both removals (and the
SlotNode was at the tail of its DirSegment's SlotNode chain), also
mark the SlotNode for removal in the work entry (the work handler
will `pool_free` the SlotNode and unlink it from the DirSegment).
The radix-tree index link to the SlotNode is also removed via
`dircontentindex_remove`.

**OLD NameEntry reclamation** (per the review's **M1** — no
tree-wide lookup needed): after the create at src is freeable, the
OLD NameEntry (referenced by the create) is no longer referenced
**by anything**. Why no lookup is needed:

- Each `vfs_create` calls `nodes_write_name` (`src/nodes.c:355`),
  which calls `pool_alloc` to allocate **fresh slots** for the new
  NameEntry. Two `vfs_create` calls for the same name produce two
  **different** NameEntry VPs (no sharing). The OLD NameEntry is
  referenced **only** by the create being freed.
- The create's `namePtr` field is the VP of the OLD NameEntry.
  When the create is CAS-removed from the chain, no remaining
  DirContent anywhere in the tree can reference the OLD NameEntry's
  VP — the create was the only reference.
- **Defensive check (still cheap)**: the analysis reads the OLD
  NameEntry's first slot (`pool_acquire(old_name_first_vp)`) and
  verifies the name string matches the create's expected OLD name.
  If the slot was reused for something else since (e.g., another
  `vfs_create` allocated at the same VP after a `pool_free`), the
  content won't match and the analysis skips the OLD NameEntry
  free (defensive, expected to never fire in practice).

The OLD name is read from the create's `namePtr` field (read in
§3.4). For multi-slot NameEntries (name > 16 bytes), the chain is
walked and **all chain slots** are added to the per-trigger batch
list (one batch entry per chain slot, all marked as
`BATCH_ENTRY_NAME_CHAIN`). The work handler iterates the chain
in order, freeing each slot. This avoids leaking the chain slots
(see §4.4 below for the work-handler logic).

### §3.7 Push the work entry

```c
bin_push(ctx->sb, BIN_WORK_REMOVE_TOMBSTONE, batch_head, batch_count);
```

`batch_head` is the head of the per-entry list (1-3 entries typical:
tombstone, create, OLD NameEntry). `batch_count` is the number of
entries.

### §3.8 Idempotency

The analysis re-walks the chain each pass. If the tombstone or
create is already removed (e.g., a prior pass succeeded), the
analysis sees it's not in the chain and skips it. The work handler
also handles "entry no longer in chain" gracefully (treated as
already-done).

---

## §4. Work handlers

The work handler is `gc_handle_remove_tombstone(vfs, entry)`. It
runs on the GC thread, called by `gc_process_entry` after
`bin_pop` returns the work entry.

### §4.1 Iterating the batch

Same pattern as `gc_handle_free_pages`: iterate the per-entry
linked list, do the per-entry work.

### §4.2 Per-entry work

The batch is heterogeneous (tombstone, create, OLD NameEntry), so
each list entry has a "type" field:
- `BATCH_ENTRY_TOMBSTONE`: a DC slot to CAS-remove from a chain
- `BATCH_ENTRY_CREATE`: a DC slot to CAS-remove from a chain
- `BATCH_ENTRY_NAME`: a NameEntry slot to `pool_free`

(The exact encoding: a 32-byte batch slot, first 8 bytes = next
batch slot VP, second 8 bytes = target slot VP, third 4 bytes =
entry type, fourth 4 bytes = parent_dir_vp (for dircontentindex
update).)

For `BATCH_ENTRY_TOMBSTONE` or `BATCH_ENTRY_CREATE`:
1. Read the chain head via `live_pool_page` (B3 fix — live cache
   payload, not stack-local).
2. CAS-remove the entry from the chain (same pattern as
   `drop_dir_entries` in file-deletion, with the B3 fix).
3. If the SlotNode's chain is now empty AND the SlotNode is at
   the tail of its DirSegment's SlotNode chain, also unlink the
   SlotNode from the DirSegment and `pool_free` the SlotNode.
4. Call `dircontentindex_remove(parent_dir_vp, name_hash,
   slot_vp, ctx->page_size)` to clear the radix-tree link.
5. `pool_free(target_vp)`.

For `BATCH_ENTRY_NAME`:
1. `pool_free(target_vp)`.

### §4.3 Idempotency

If a chain entry is no longer at the expected position (e.g.,
another operation moved it), the CAS-remove fails; the handler
treats it as "already done" and continues.

If the OLD NameEntry's slot is no longer valid (e.g., it was
already freed by a prior pass), the `pool_free` is a no-op (or
returns `VFS_ERR_IO` — the handler logs and continues).

### §4.4 Multi-slot NameEntry chain handling

A NameEntry can span multiple pool slots when the name is > 16
bytes (the first slot holds 8 bytes of hash + up to 16 bytes of
name; additional slots hold 24 bytes of name each). The OLD
NameEntry's **first slot VP** is what was added to the per-trigger
batch list (per §3.6). To free the entire NameEntry chain (no
slot leak):

1. The work handler reads the first slot's `nextPtr` field
   (`ANCHOR_OFF_SIBPTR` — slots use the same anchor layout as
   DirContent).
2. For each chain slot VP, add a `BATCH_ENTRY_NAME_CHAIN` entry
   to a per-trigger mini-list (similar to the batch list itself).
3. Iterate the mini-list, `pool_free` each chain slot VP in
   reverse order (last slot first, so the first slot is freed
   last — this matches the per-slot reads in `nodes_write_name`).

For names ≤ 16 bytes (single-slot NameEntry), the chain is
trivial: only the first slot, no chain. The `BATCH_ENTRY_NAME_CHAIN`
list is empty; the work handler frees just the first slot.

**Net effect**: no slot leak from the OLD NameEntry's chain
slots, regardless of name length. The complete version's "clean
slate, no space leak" guarantee holds for all rename workloads.

---

## §5. Lock model

Same as file-deletion (§5 of that spec):

- The analysis handler runs on the GC thread. The GC thread is a
  single consumer; no lock contention.
- The `drop_dir_entries` equivalent uses **CAS on the live cache
  payload** (B3 fix), same as file-deletion. No lock acquisition.
- The `vfs_lock` on the parent dir is NOT acquired by the GC
  thread. Concurrent `vfs_create`/`vfs_delete`/`vfs_rename` on the
  same parent dir are serialized via their own locks; the GC's CAS
  on the chain head naturally coexists with the lock-protected
  write paths (the CAS only succeeds when the live cache reflects
  the expected pre-state).

For OLD NameEntry reclamation (per the M1 simplification — no
tree-wide lookup): a concurrent `vfs_create` could allocate a
new NameEntry at the same VP (if a slot was freed and re-allocated
in a prior GC pass). The analysis's defensive check reads the OLD
NameEntry's first slot content and verifies the name string
matches the create's expected OLD name (read from the create's
`namePtr` field via the chain walk in §3.4). If the content
doesn't match, the OLD NameEntry slot has been reused and the
analysis skips the `pool_free` (defensive, expected to never
fire in practice).

---

## §6. Page free

This spec does NOT free any data pages. The file is being renamed,
not deleted. The data pages remain live (referenced by the
FileNode's FileContent → PageNode → VersionPage chain).

The "page free" work handler (`gc_handle_free_pages`) is reused as-is
for the file-deletion bin job; this spec doesn't add a new page-free
work handler.

---

## §7. Crash safety

### §7.1 Trigger durability

The trigger (`BIN_TRIGGER_TOMBSTONE_ADDED`) is pushed to the Bin,
which is persisted to the VFS header (Phase 27). The trigger is
durable across crashes.

### §7.2 Work entry durability

The work entry (`BIN_WORK_REMOVE_TOMBSTONE`) is also in the Bin,
durable. The work entry's `context` (batch_head) points at a
freshly-allocated pool slot containing the per-entry list. The pool
slot is in the VFS file (durable).

### §7.3 Idempotency on remount

The trigger is re-encountered on remount (via `validate_bin_on_mount`
in `src/bin.c`). The analysis runs again, re-walks the chain, and
determines what's freeable. If a crash happened mid-drop, the
chain might be in an inconsistent state (e.g., the tombstone was
removed but the create is still there). The analysis handles this:
if the entry is no longer in the chain, skip it.

The chain itself uses a "walk to find the entry by VP" approach
(see §3.4 fallback to "find by epoch + namePtr" if VP doesn't
match). So a partial drop doesn't break the analysis.

### §7.4 Work entry durability

The work entry's batch list is in pool slots (durable). If a crash
happens during the work handler, the work entry is re-processed
on remount. The per-entry CAS-removes are idempotent.

---

## §8. Test plan

### §8.1 Unit tests in `test/test_gc.c`

- `test_rename_tombstone_freed_cross_dir_no_snapshot` — cross-dir
  rename at the same epoch as create. After GC, the src chain is
  empty, the OLD NameEntry is freed, the SlotNode is unlinked from
  the DirSegment. Verify `pool_alloc_count` returns to baseline +
  3 slots (the new dst_dc, dst_slot, and dst_name).

- `test_rename_create_freed_diff_epoch_no_snapshot` — create at
  epoch 0, no snapshot, rename at epoch 2. After GC, the tombstone
  is freed; the create is freed after the bin pass (since no
  active snapshot at R = 1 in `[0, 2)`). Verify all 3 slots freed.

- `test_rename_create_kept_with_active_snapshot` — create at epoch 0,
  snapshot at epoch 1, rename at epoch 2. After GC, the tombstone
  is freed; the create is KEPT (the snapshot at epoch 1 sees the
  create). After vfs_delete_snapshot, the create is freed.

- `test_rename_same_dir_no_tombstone` — same-dir rename, the
  trigger is pushed with `context2 = 0`. The analysis skips the
  tombstone branch, frees the create (when unreachable) + OLD
  NameEntry.

- `test_rename_OLD_name_freed_even_if_another_file_has_same_name`
  — cross-dir rename "foo" → "bar"; then create a NEW file named
  "foo" in some dir. The OLD NameEntry (from the create that's
  being freed) and the NEW NameEntry (from the new create) are
  **different VPs** (`nodes_write_name` always allocates fresh).
  Both are freed. The test asserts `pool_alloc_count` returns
  to baseline + the right number of slots (dst_dc, dst_slot,
  dst_name, new_foo_dc, new_foo_name). The OLD foo's NameEntry
  is freed even though a new file named "foo" exists.

### §8.2 Integration tests in `test/test_gc_thread.c`

- `test_rename_tombstone_bin_job_basic` — mount, create, rename
  cross-dir, flush, sleep 200ms, verify the src dir no longer has
  "foo" (it's been renamed to "bar" at dst), verify the file is
  accessible at the new name, verify the OLD name lookup at the
  original snapshot (if any) returns "foo".

- `test_rename_tombstone_with_active_snapshot` — create, snapshot,
  rename, wait for GC. The OLD name "foo" is still findable at
  the snapshot. Drop the snapshot, wait for GC. The OLD name is
  gone from src.

- `test_rename_no_space_leak` — heavy rename workload (100
  creates, 100 renames). After GC drains, `pool_alloc_count`
  returns to a bounded value (no per-rename leak).

### §8.3 Crash safety test in `test/test_crash.c`

- `test_crash_after_rename_before_gc` — fork+kill+remount after
  the rename pushes the trigger but before the GC processes it.
  On remount, the trigger is re-processed, the cleanup completes.

- `test_crash_mid_drop` — kill the process after the work handler
  has CAS-removed the tombstone but before the create is removed.
  On remount, the analysis re-runs, finds the create is still
  there, removes it.

### §8.4 Performance test in `bench/bench.c`

- `bench_rename_drain` — create N files, rename each cross-dir,
  wait for GC, measure the per-rename GC latency and the bin
  drain time. Compare to the pre-this-spec baseline (where the
  bin was empty post-rename because the trigger was NOOP).

Expected: per-rename GC latency is small (a few µs per rename
analysis + work). The bin drain is dominated by the analysis
overhead (~10 µs per rename for the chain walk + reference point
check). For N = 1000 renames, total drain time is ~10 ms. The
pre-this-spec baseline is ~0 ms (NOOP trigger), but the
post-this-spec drain is still "in noise" compared to the overall
benchmark (which is dominated by vfs_write throughput).

---

## §9. Open questions

1. **Should the trigger be pushed for same-dir renames?** The
   analysis handles same-dir (skip the tombstone branch, free the
   create). Pushing adds a GC entry per same-dir rename. Not
   pushing means same-dir renames never free the create. **Decision
   needed: push or skip?**

   My recommendation: **push**. The cost is small (~1 µs push +
   ~10 µs analysis) and the benefit is consistency (rename always
   reclaims the old name's resources).

2. **OLD NameEntry lookup is unnecessary** (per the review's **M1**).
   Each `vfs_create` allocates its own NameEntry via
   `nodes_write_name` → `pool_alloc` (no sharing between files). The
   OLD NameEntry is referenced **only** by the create being freed.
   The analysis unconditionally marks the OLD NameEntry for
   `pool_free` once the create is freeable. A defensive name-match
   check (read the OLD NameEntry's content and compare) catches the
   theoretical case of slot reuse (extremely unlikely in practice).

3. **What if the `vfs_rename` was a no-op (src == dst)?** The
   `vfs_rename` code should reject this (returns `VFS_ERR_*`), but
   if it's somehow called, the trigger's tombstone and create are
   at the same VP. The analysis would see a self-referencing chain
   and either loop infinitely or skip. **Decision needed**:
   defensive check in the analysis to detect self-referencing
   chains.

   My recommendation: **add a defensive check**. If the
   tombstone_vp == create_vp, log a warning and return
   `VFS_OK` (no-op).

4. **What if the chain has multiple creates (e.g., the file was
   renamed away and back)?** The chain at src has multiple
   `namePtr != 0` entries. The analysis picks the highest-epoch
   one as the "create to free". But the lower-epoch creates might
   be reachable at lower snapshots. **Decision needed**: do we
   free just the highest create, or do we walk the entire chain
   and free all unreachable creates?

   My recommendation: **walk the entire chain**, free each
   unreachable create individually. The chain walk is bounded by
   the chain length (typically 1-2 entries per rename). The
   per-create `namePtr` hash check decides each.

5. **What if the SlotNode becomes empty (all DCs removed) and the
   SlotNode is in the middle of the DirSegment's chain?** The
   SlotNode can be removed and the chain re-linked. **Tradeoff**:
   more code in the work handler. **Alternative**: leave the empty
   SlotNode (it's a small leak per fully-renamed-away file).

   My recommendation: **remove the empty SlotNode**. The code is
   straightforward (CAS the DirSegment's SlotNode chain, similar
   to the existing DirContent CAS-remove).

6. **Work entry batch list lifetime**: the batch list is in pool
   slots. The work handler iterates the list and `pool_free`s
   the entry slots (DC + NameEntry) but NOT the batch slots
   themselves (TODO-12). After the work handler returns, the
   batch slots are leaked (small, similar to existing leak
   pattern). **Decision needed**: pool_free the batch slots too?

   My recommendation: **defer to TODO-12**. The batch slot leak
   is bounded (a few slots per rename) and is the same pattern
   as the existing `BIN_WORK_FREE_PAGES` batch. Adding pool-free
   for batch slots is a TODO-12 follow-up.

---

## §10. Source

Files to read for context:

- `impl/phase-28-bin-job-file-deletion.md` — the spec this one
  follows; reuse §2-§7 patterns.
- `src/gc_bin_file_deleted.c` — reference implementation of the
  file-deletion analysis handler and work handler. The B3 fix
  (`live_pool_page`, `cas_head_ptr_live`, `cas_next_ptr_live`)
  is reused here.
- `src/gc_bin_free_pages.c` — reference work handler. The
  per-batch linked list pattern is reused.
- `src/bin.c` — Bin push/pop, the B1-fix tail-dangling fix in
  `bin_pop` (relevant for the per-batch list's Bin page).
- `src/tree.c:2396+` — `vfs_rename` implementation. The current
  push site is `src/tree.c:2883`.
- `src/nodes.c::nodes_read_name_hash` — name-hash extraction for
  the radix-tree link.
- `src/nodes.h` — DirContent, SlotNode, NameEntry offset
  definitions.
- `src/mapper.h` — `mapper_table_traversal_apply`,
  `mapper_table_resolve` for the read-rule mapper handling.
- `src/gc_map.h` (if it exists) — GC-specific helpers.
- `src/ixsphere/vfs_internal.h` — `vfs_lock`, `vfs_unlock`
  semantics (for §5's lock model).

---

## §11. Migration plan (workloads)

1. **W1: Constants** (`~10 LOC` + spec review)
   - Define `BIN_TRIGGER_TOMBSTONE_ADDED = 2` and
     `BIN_WORK_REMOVE_TOMBSTONE = 0x101` in `src/bin.h`.
   - Add the dispatch cases in `src/gc.c::gc_handle_trigger` and
     `src/gc.c::gc_handle_work`.
   - Verify: build clean, 16/16 ctest pass.

2. **W2: Analysis handler** (`~400 LOC` in
   `src/gc_bin_rename_tombstone.c` — reduced from 500 by the M1
   simplification that removed the tree-wide lookup)
   - Implement `gc_handle_rename_done(vfs, entry)`.
   - Reuse `collect_active_snapshots`, `read_rule_pick_first_dc`,
     `find_parent_dir`, `find_slotnode_in_dir` from
     `gc_bin_file_deleted.c`.
   - Implement the tombstone + create freeable checks.
   - For OLD NameEntry: read the create's `namePtr` to get the
     first slot VP; defensively verify the slot content matches
     the expected name; walk the chain to collect all chain
     slot VPs; add them to the per-trigger batch list.
   - Implement the per-trigger batch list builder.
   - Verify: build clean, 16/16 ctest pass.

3. **W3: Work handler** (`~150 LOC` — increased from 100 by the
   multi-slot NameEntry chain handling in §4.4)
   - Implement `gc_handle_remove_tombstone(vfs, entry)`.
   - Reuse the B3 fix from `drop_dir_entries` (CAS on live
     cache payload).
   - Implement the per-entry work (DC removal + dircontentindex
     update + pool_free; empty-SlotNode unlink; NameEntry
     single-slot free; NameEntry multi-slot chain free).
   - Verify: build clean, 16/16 ctest pass.

4. **W4: Wire up vfs_rename** (`~5 LOC`)
   - Hoist `int64_t src_dc_vp = 0;` to function scope at the top
     of `vfs_rename` (per the review's M2 — single push site).
   - Replace the `BIN_TRIGGER_NOOP` push at `src/tree.c:2883`
     with `bin_push(BIN_TRIGGER_TOMBSTONE_ADDED, file_vp, src_dc_vp)`.
   - `src_dc_vp` is 0 for same-dir (no tombstone) and the actual
     tombstone VP for cross-dir.
   - Verify: build clean, 16/16 ctest pass.

5. **W5: Tests** (`~600 LOC` in `test/test_gc.c` and
   `test/test_gc_thread.c`)
   - Implement the 5 unit tests (§8.1).
   - Implement the 3 integration tests (§8.2).
   - Verify: build clean, all tests pass.

6. **W6: ISSUES.md update** (`~50 LOC` text)
   - Document the new bin job in `ISSUES.md` (section "Phase 28
     Type 2: rename tombstone removal").
   - Cross-link to this spec.

7. **W7: TODO-12 partial resolution** (mark complete)
   - The "OLD NameEntry" leak is now closed. Update TODO-12 to
     reflect: pool slots for DC + NameEntry are freed by the
     bin job. Only the per-batch batch slots and the
     pool-page-level reclamation remain as TODO-12 follow-ups.

---

## §12. Risk assessment

**Low risk**:
- The spec follows a well-trodden pattern (file-deletion bin job).
- No data page handling (much simpler than file-deletion).
- Idempotent: re-pushing the trigger is a no-op.
- The B3 fix (CAS on live cache payload) is already implemented
  in file-deletion and reused here.

**Medium risk**:
- The OLD NameEntry lookup is a tree walk (potential perf
  concern). Mitigated by amortizing across many renames and
  by the lookup's bounded cost (O(log N) for the radix tree
  per dir, O(D) dirs).
- The empty-SlotNode removal (when all DCs are gone) adds code
  complexity. Mitigated by the bounded case (only fires when
  a file is fully renamed away).

**High risk**:
- None identified. The spec is conservative (it doesn't free
  anything that could be referenced by a reachable snapshot).

**Mitigations**:
- The spec is reviewed externally before implementation.
- Workloads W1-W7 are separable; each can be reviewed and
  tested independently.
- The idempotency check (§3.8) ensures a re-run is safe even
  if a partial state is encountered.
- The defensive check for self-referencing chains (§9.3) catches
  any unexpected `vfs_rename` no-op case.
