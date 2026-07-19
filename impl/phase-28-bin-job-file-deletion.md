# Phase 28 — Bin Job: Type 1 — Free Pages from File Deletion

**Status:** Draft. First per-bin-job spec after the framework
(impl/phase-28-gc.md). Implements the work for the
`BIN_TRIGGER_FILE_DELETED` trigger that `vfs_delete` produces.

**Goal:** Specify the analysis and work handlers for the
`vfs_delete` garbage. When a file is deleted, the bin job:

1. Walks the file's chain, classifies each data page as live/dead
   based on the current snapshot branch state (per the corrected
   VFS epoch model, 2026-07-18).
2. Walks the parent dir's chain, checks if the file is referenced
   by any other epoch. If not, drops the create + tombstone entries
   from the parent dir's chain.
3. Frees the dead data pages (and their mirror siblings).

**What's in this spec:**

- The trigger and work types
- The analysis algorithm (trigger handler)
- The work handlers (free pages, drop dir entries)
- The lock model (least-disruptive: CAS-based, no blocking)
- The mirror-sibling handling for `storage_free`
- The crash safety story
- The test plan

**What's NOT in this spec:**

- Pool slot freeing (TODO-12, deferred — the spec uses a stub)
- The other bin-job specs (Type 2 tombstones, Type 3 soft-deletes,
  Type 4 committed rewrites, file truncate, snapshot delete, mirror
  write failure) — each gets its own spec

---

## Constants (per the review's L1 finding)

- `MAX_ACTIVE_SNAPSHOTS = 64` — maximum number of active
  snapshots considered in the reference-point analysis
  (§3.1, §3.3). For systems with more than 64 active
  snapshots, only the LATEST 64 are considered (earlier
  ones are shadowed by newer ones in the read rule). 64
  is generous; typical workloads have ≤ 8 active
  snapshots at a time.

## Mental model (prerequisites)

This spec relies on the VFS epoch mental model corrected by the
user on 2026-07-18. Quoting the saved user memory:

> **Odd epochs = snapshot branches. Like git branches.** You get
> a frozen head image that you can play with WITHOUT affecting
> the head. Even epochs = live head, and ONLY the last even
> epoch (the current head) is writable. All previous even
> epochs become the **frozen head source** for the snapshot
> taken. Epochs always advance by 2: D-1 / old even becomes
> the frozen image, D / odd becomes the snapshot, D+1 / new
> even becomes the new head.
>
> **So vfs_delete works fine as-is.** Deleting in a snapshot
> D only affects the snapshot. The head still sees the file
> via the highest even < head_epoch (per the read rule, §7.2
> step 7: odd below R' is skipped). There is NO BUG in
> vfs_delete — operations in a branch never affect main until
> merge. This is by design.
>
> **Implications for GC (Phase 28, 2026-07-18, per user)**:
> The GC must account for the branch model. A file deleted at
> odd D: D still active (no mapper entry for D) → head DOES
> see the file. Data pages are LIVE. The deletion is only
> visible AT the snapshot.

The file-deletion bin job is **"act now, based on current
state"**. It does not defer waiting for the snapshot to resolve.
The snapshot-resolution cases (commit, soft-delete) are handled
by separate bin activities (`vfs_commit` and
`vfs_delete_snapshot`).

---

## 1. Name and motivation

**Name:** Type 1 — Free pages from file deletion.

**Motivation:** A `vfs_delete` operation creates four kinds of
garbage:

1. **Orphan data pages** — the file's VersionPage chain may
   reference data pages (the file's content) that are no longer
   needed because the file is now deleted. For a delete in an
   active snapshot, the COW data pages from the snapshot's
   modifications of the file (before deletion) are orphaned —
   no reference point can see them. For a delete at the head
   with no live snapshots, all data pages are orphans.
2. **Orphan chain slots** — the FileNode, FileContent,
   PageNode, FileSize, and NameSlots form a chain rooted at
   the FileNode. When the file is no longer referenced by any
   reference point, the entire chain is dead and the slots
   can be returned to the pool's free list.
3. **Tombstone in the parent dir's chain** — a DirContent with
   `namePtr=0` was prepended to the parent dir's chain (the
   deletion marker). The tombstone is in use as long as some
   reference point sees it as "the visible entry for this
   child". When no reference point needs the tombstone
   (because no reference point has any other live entry for
   this child), the tombstone is dead and can be removed.
4. **Create entry in the parent dir's chain** — a DirContent
   with `namePtr!=0` that originally created the file is
   also in the parent dir's chain. The create entry is what
   makes the file visible at some reference point. If no
   reference point needs the file (no live entry), the create
   entry is also dead and can be removed.

**Why background GC:** the garbage is large (a file with
thousands of data pages has thousands of garbage pages) and
the free is non-trivial (CAS-based chain modifications,
mirror-sibling handling). Doing it inline in `vfs_delete`
would bloat the per-delete latency. The bin job defers the
work to the GC thread, which can process entries at its own
pace with the lock model of its choice.

---

## 2. Trigger and work types

### 2.1 Trigger: `BIN_TRIGGER_FILE_DELETED`

- **Type tag:** `BIN_TRIGGER_FILE_DELETED = 1` (assigned
  explicitly here; the framework spec §4 lists it as an
  illustrative sketch without a value).
- **Context:** `file_vp` — VirtualPtr of the FileNode slot
  (the file being deleted). Passed by `vfs_delete` as
  `found_childPtr`.
- **Context2:** `tombstone_vp` — VirtualPtr of the tombstone
  DirContent slot that `vfs_delete` just inserted into the
  parent dir's chain. Passed by `vfs_delete` as `dc_vp` (the
  slot allocated by `pool_alloc` and written with
  `nodes_write_dircontent(..., namePtr=0, ...)` at the start
  of `vfs_delete`'s CAS loop).
- **Idempotency:** idempotent at the trigger level. The
  analysis re-walks the chain based on current state, so
  re-pushing the same trigger produces the same result (modulo
  any state changes between pushes, which the analysis handles
  correctly).

### 2.2 Work: `BIN_WORK_FREE_PAGES`

- **Type tag:** `BIN_WORK_FREE_PAGES = 0x100`
  (`BIN_TYPE_WORK_THRESHOLD + 0`; assigned explicitly
  here).
- **Context:** `pages_head` — pointer (encoded as a tagged
  VirtualPtr) to the head of a per-batch linked list of
  `logical_page` entries to free. The list lives in
  freshly-allocated pool slots and is freed by the work
  handler.
- **Context2:** `pages_count` — number of `logical_page`
  entries in the batch.
- **Idempotency:** idempotent. `storage_free` is idempotent
  (it checks `indir_lookup` before acting; freeing an
  already-freed page is a no-op). Re-pushing the same work
  entry is safe.

### 2.3 Dir-entry drop is done inline in the analysis

**Design decision (per H1 in `PHASE28_BINJOB_FILEDEL_REVIEW.md`):**
the create + tombstone drop is done **inline in the analysis
handler**, NOT deferred to a separate `BIN_WORK_DROP_DIR_ENTRIES`
work type.

Rationale:

- The dir-entry drop is a single CAS-based chain
  modification (one or two CAS operations on the
  SlotNode's nextPtr). It's quick (no I/O, no
  allocation) and doesn't benefit from batching.
- Deferring it to a work entry requires the trigger
  to carry the parent dir VP (or the SlotNode VP) to
  the work handler. The FileNode doesn't have a parent
  pointer, and walking from root to find the parent on
  every work-entry dispatch is O(N) in the tree depth.
- The analysis already has the tombstone VP and the
  file VP in its context; the SlotNode VP is the
  tombstone's slot (it's in the SlotNode's chain
  directly). The analysis can do the drop with no
  additional lookups.
- The work handler (`BIN_WORK_FREE_PAGES`) handles the
  page frees because those are expensive (mirror
  handling, batched freeing).

**Trigger context (final form):**
- `context` = `file_vp` (the FileNode)
- `context2` = `tombstone_vp` (the DirContent the
  vfs_delete operation just prepended; it's in the
  SlotNode's chain)

**Note on the per-user clarification (2026-07-18):**
"We need to drop create entry as well as long as the
file is not referenced in any other epoch (same as for
tombstone)." This spec honors this by having the
analysis check file visibility and drop BOTH entries
when no reference point needs them.

### 2.4 Producer push call site

Replace the existing `vfs_delete` placeholder push in
`src/tree.c:1902`:

```c
/* Before (W3 placeholder): */
bin_push(ctx->sb, BIN_TRIGGER_NOOP, (int64_t)found_childPtr, 0);

/* After (this spec): */
bin_push(ctx->sb, BIN_TRIGGER_FILE_DELETED,
         (int64_t)found_childPtr, (int64_t)dc_vp);
```

Where `dc_vp` is the tombstone slot VirtualPtr allocated
and written earlier in `vfs_delete` (line ~1834: `int64_t
dc_vp = pool_alloc(...)`).

This is the only producer change required by this spec.
Other producers (`vfs_rename`, `vfs_truncate`,
`vfs_commit`, `vfs_delete_snapshot`, `mirror_write`) push
different trigger types defined by their own bin-job specs.

---

## 3. Analysis (trigger handler)

The trigger handler runs in the GC thread when it pops a
`BIN_TRIGGER_FILE_DELETED` entry. The handler:

1. Walks the file's chain and classifies each data page
   as live/dead based on visibility at the reference
   points (§3.1, §3.4).
2. Walks the parent dir's chain to determine if the file
   is referenced by any reference point (§3.5).
3. If dead data pages exist, pushes a `BIN_WORK_FREE_PAGES`
   work entry (§3.4).
4. If the file is not referenced, performs the inline
   dir-entry drop (create + tombstone, §3.9) — this is
   a CAS operation, no separate work entry.

The analysis is **idempotent** (§3.10) and is the single
trigger handler for this bin job.

### 3.1 Reference points

A **reference point** is a point in the snapshot/head space
where a reader might exist. For the purpose of "is this page
freeable now", we consider:

- `H = currentEpoch` — the live head (always even, the latest
  even in the system).
- Each **active snapshot** `S` where `S` is odd, `S < H`, and
  the mapper has no entry for `S` (i.e., `mapper.resolve(S)
  == S`, indicating no commit or soft-delete has resolved the
  snapshot).

Active snapshots are determined by walking the mapper
table's in-memory state (`ctx->mapper_table`). The mapper
table is a small dict keyed on `fromEpoch`. An odd `S` is
active iff `S` is NOT a key in the mapper table (no commit
or soft-delete has resolved the snapshot).

**Performance note (per L3 in `PHASE28_BINJOB_FILEDEL_REVIEW.md`):**
The recommended implementation maintains an in-memory set
of active snapshot epochs in `ctx` (updated by `vfs_snapshot`,
`vfs_commit`, `vfs_delete_snapshot`). The analysis iterates
this set, not the mapper table. This is O(active_snapshots)
per trigger instead of O(mapper_size) per trigger. For
typical workloads (≤ a few active snapshots at a time),
this is in noise; for a long-running VFS with hundreds of
active snapshots, the cache makes the difference.

Note: committed snapshots (`traversalApply=true`) and
soft-deleted snapshots (`traversalApply=false` with an entry
present) are NOT reference points. They are resolved; only
their data is in the system (either remapped at H or simply
gone).

### 3.2 File visibility at a reference point

A file is **visible at R** iff the parent dir's chain, when
walked per the read rule at R, has a DirContent with
`namePtr != 0` (a "live entry") for this child.

Read rule at R for a chain entry at epoch E (per SPEC §7.2):
- If `mapper_table_traversal_apply(E)` is true, replace E
  with `mapper_table_resolve(E)` (the toEpoch).
- If E == R: use it (exact match).
- If E > R: skip (future).
- If E < R AND E is even: use it (committed base, chains are
  descending).
- If E < R AND E is odd: skip (unrelated snapshot).

### 3.3 File-deletion trigger analysis algorithm

```c
int gc_handle_file_deleted(vfs_t* vfs, const BinEntry* entry) {
    TreeContext* ctx = vfs->ctx;
    int64_t file_vp = entry->context;
    int64_t tombstone_vp = entry->context2;

    /* 1. Sanity check: the file slot still exists.  If not,
          a prior run already processed this trigger. */
    PoolSlot file_slot = {0};
    pool_acquire(&ctx->pool, file_vp, false, &file_slot);
    if (file_slot.vptr == VFS_VPTR_NULL) {
        /* File slot already freed; nothing to do. */
        return VFS_OK;
    }
    pool_release(&ctx->pool, &file_slot);

    /* 2. Determine reference points: H + each active snapshot. */
    int64_t H = ctx->currentEpoch;
    /* MAX_ACTIVE_SNAPSHOTS = 64.  For systems with more than 64
       active snapshots, only the LATEST 64 are considered (the
       earlier ones are dominated — any reference point R_old is
       shadowed by any newer active R_new in the read rule, so
       R_old doesn't add visibility that R_new doesn't already
       provide).  In practice, 64 is generous; typical workloads
       have ≤ 8 active snapshots at a time. */
    int64_t active_snapshots[MAX_ACTIVE_SNAPSHOTS];
    int    num_active = 0;
    collect_active_snapshots(ctx, H, active_snapshots, &num_active);

    /* 3. Walk the file's chain, classify data pages. */
    LivePageSet* dead_data_pages = live_page_set_create(256);
    classify_data_pages(ctx, file_vp, H, active_snapshots, num_active,
                        dead_data_pages);

    /* 4. Walk the parent dir's chain, determine file visibility
          at each reference point.  This finds the SlotNode
          containing the tombstone and the create entry (if
          any), and the file's reference status. */
    int    file_referenced = 0;
    int64_t create_vp = 0;
    int64_t slot_vp   = 0;
    int err = check_file_referenced(ctx, file_vp, tombstone_vp,
                                     H, active_snapshots, num_active,
                                     &file_referenced, &create_vp, &slot_vp);
    if (err != VFS_OK) {
        live_page_set_destroy(dead_data_pages);
        return err;
    }

    /* 5. If there are dead data pages, push a BIN_WORK_FREE_PAGES. */
    if (dead_data_pages->count > 0) {
        int64_t batch_head = batch_pages_into_pool_list(ctx, dead_data_pages);
        bin_push(ctx->sb, BIN_WORK_FREE_PAGES, batch_head,
                 (int64_t)dead_data_pages->count);
    }
    live_page_set_destroy(dead_data_pages);

    /* 6. If the file is not referenced by any other epoch, do the
          dir-entry drop inline.  No separate work entry — this
          is a quick CAS-based operation, deferred execution
          would just add latency. */
    if (!file_referenced) {
        if (create_vp != 0) {
            /* Drop both create and tombstone from the SlotNode's chain. */
            drop_dir_entries(ctx, slot_vp, create_vp, tombstone_vp);
        } else {
            /* No create entry found (the file was created in the same
               tombstone's epoch, or the chain was already partially
               cleaned up).  Just drop the tombstone. */
            drop_dir_entries(ctx, slot_vp, 0, tombstone_vp);
        }
    }

    return VFS_OK;
}
```

### 3.4 `classify_data_pages` algorithm

Walk the file's chain (FileContent → PageNode → VersionPage).
For each VersionPage `E_VP` at epoch `E_VP_epoch` referencing
data page `data_page`:

```c
int live = 0;
for (each reference point R in {H} ∪ active_snapshots) {
    /* Check if the file is reachable at R. */
    if (!file_visible_at(R, file_vp, tombstone_vp)) continue;
    /* Check if E_VP is visible per the read rule at R. */
    if (read_rule_picks(R, E_VP, file_vp)) { live = 1; break; }
}
if (!live) {
    /* Data page is not visible at any reference point.
       Add to the dead set.  The mirror sibling is identified
       later by the work handler (§4.1) which reads the
       PageHeader.  We add the logical page here; the work
       handler resolves the mirror at free time. */
    live_page_set_add(dead_data_pages, data_page);
}
```

Note: per the review's H2 finding, the mirror is NOT
identified here. The work handler reads the PageHeader at
free time and also frees the mirror. This matches the
existing `deferred_free_enqueue` pattern in `src/gc.c`.

### 3.5 `check_file_referenced` algorithm

Walk the parent dir's chain (SlotNode → DirContent). For each
reference point R, apply the read rule at R. If R's view has
a DirContent with `namePtr != 0` for this child, the file is
referenced at R. If any R references the file, the file is
referenced.

```c
int check_file_referenced(ctx, file_vp, tombstone_vp,
                          H, active_snapshots, num_active,
                          *out_referenced, *out_create_vp, *out_slot_vp) {
    /* The tombstone is in a specific SlotNode in the parent dir's
       chain.  Find the SlotNode by walking from the tombstone's
       slot — the slot's `sibPtr` (in the DirContent entry that
       points to the SlotNode, NOT in the SlotNode itself) leads
       us to the SlotNode.  Specifically, the tombstone's
       SlotNode is the SlotNode that the tombstone was prepended
       to (captured by vfs_delete as `found_slot_vp`).
       Implementation: walk the parent dir's chain from the root
       to find the SlotNode whose head's nextPtr == tombstone_vp.
       This is O(SlotNode chain length); for typical directories
       (≤ 16 SlotNodes), this is in noise. */

    int64_t slot_vp = find_slotnode_for_tombstone(ctx, tombstone_vp);
    *out_slot_vp = slot_vp;

    int referenced = 0;
    int64_t create_vp = 0;

    for (each reference point R in {H} ∪ active_snapshots) {
        /* Walk the SlotNode's DirContent chain at R, find the
           first applicable DirContent.  If it's a tombstone
           (namePtr=0), file is not referenced at R.  If it's
           a live entry (namePtr!=0) with childPtr == file_vp,
           file IS referenced at R. */
        int64_t first_dc = read_rule_pick(R, slot_vp);
        if (first_dc == 0) continue;
        PoolSlot dc_slot = {0};
        pool_acquire(&ctx->pool, first_dc, false, &dc_slot);
        if (dc_slot.vptr == VFS_VPTR_NULL) continue;
        int64_t name_ptr = vfs_rd8_s(dc_slot.bytes, DIRCONTENT_OFF_NAMEPTR,
                                      ctx->page_size);
        int64_t child_ptr = vfs_rd8_s(dc_slot.bytes,
                                       DIRCONTENT_OFF_CHILDPTR, ctx->page_size);
        pool_release(&ctx->pool, &dc_slot);

        if (name_ptr != 0 && child_ptr == file_vp) {
            /* Live entry points to this file.  File is referenced
               at R.  Also capture the create entry VP for the
               dir-entry drop (step 6). */
            referenced = 1;
            create_vp = first_dc;
        }
    }

    *out_referenced = referenced;
    *out_create_vp = create_vp;
    return VFS_OK;
}
```

### 3.6 Why the read rule is applied per-entry, not per-walk

The mapper remap is per-entry: the read rule at R for a chain
entry at epoch E applies `traversalApply` to E (not to R's
view of E). The existing `vfs_chain_walk` in `src/tree.c:791`
implements this. The analysis reuses the same logic.

### 3.7 `drop_dir_entries` — inline CAS-based drop

The analysis performs the dir-entry drop inline. The drop
removes the create and tombstone from the SlotNode's
DirContent chain via CAS operations. The function is
**idempotent** (re-running on a partially-dropped chain is
safe) and **retry-safe** (CAS failures due to concurrent
prepends are retried with a fresh walk).

```c
int drop_dir_entries(ctx, slot_vp, create_vp, tombstone_vp) {
    /* Phase A: re-check the file-referenced condition.  A concurrent
       vfs_create or vfs_rename in the same parent dir may have
       re-added a live entry for the file since the analysis's
       check_file_referenced call.  If so, abort the drop. */
    /* (Re-check is a single dir-chain walk — same logic as
       check_file_referenced, abbreviated here.) */

    /* Phase B: locate the create + tombstone in the SlotNode's
       DirContent chain.  If create_vp == 0 (no create entry
       found), only the tombstone is dropped. */
    /* (Walk the chain to find both entries' positions and their
       predecessor nextPtr values.  The walk is bounded by the
       SlotNode's chain length, typically 1-2 entries.) */

    /* Phase C: CAS-based removal.
       - If the tombstone is at the SlotNode's head: CAS-update
         the SlotNode's headPtr to skip the tombstone.
       - Else: find the tombstone's predecessor, CAS-update
         its nextPtr to skip the tombstone.
       - If create_vp != 0 and create_vp != tombstone_vp:
         similarly CAS-skip the create entry.
       - The two removals are independent CASes.  Between CAS 1
         (remove tombstone) and CAS 2 (remove create), a
         concurrent reader walking the chain may see one entry
         removed and the other still present.  This is safe:
         the reader's view of the chain at any intermediate
         state is either the pre-drop state (both present) or
         the post-drop state for one entry.  A reader that
         walks between CAS 1 and CAS 2 sees the create entry
         but not the tombstone — the read rule at any R
         (where R is not equal to the tombstone's epoch) does
         not pick the tombstone anyway, so the reader's view
         is unchanged.  The create entry, if picked, correctly
         reflects the file's visibility. */

    /* Phase D: if both entries were removed, also update the
       chain index (per M3 in the review).  See §3.8. */

    return VFS_OK;
}
```

### 3.8 Chain index update (per M3 in the review)

Per the review's M3 finding, the chain index is **updated**
(via `dircontentindex_remove`, the existing
`src/tree.c:3147` function), not invalidated. This is one
CAS per dropped entry, not a full index rebuild.

The analysis calls `dircontentindex_remove` after
`drop_dir_entries` removes the entries. The remove is keyed
on the name hash and the SlotNode VP (the same key used by
`vfs_create` / `vfs_delete` when they add to the index).

If `drop_dir_entries` is a no-op (create + tombstone
already removed by a prior run), the index is also already
clean — no update needed.

### 3.9 Implementation note on parent dir lookup

The FileNode layout (§6.3) doesn't have a direct parent dir
pointer. The analysis finds the SlotNode by walking the
parent dir's chain from the root to find the SlotNode whose
chain head's nextPtr (or whose head itself, if the tombstone
is at the head) is the tombstone_vp. This is
O(SlotNode chain length), typically ≤ 16.

The trigger's `context2` (tombstone_vp) is sufficient: the
analysis walks the tombstone's SlotNode, which is the entry
point to the parent dir. No additional context field is
needed.

### 3.10 Idempotency

The analysis is idempotent: re-running on the same trigger
context produces the same result (modulo intervening state
changes, which the analysis handles correctly because it
re-walks the current state). Specifically:

- `classify_data_pages` re-walks the current file chain and
  produces the same set of dead data pages (modulo new
  VersionPages added between runs).
- `check_file_referenced` re-walks the current parent dir
  chain and produces the same `file_referenced` verdict.
- `drop_dir_entries` is a no-op if the create + tombstone
  are already removed.

If the file's chain has been modified between two runs (e.g.,
a new VersionPage was added), the second run picks up the
new state. No spurious work entries are pushed.

---

## 4. Work handlers

### 4.1 `BIN_WORK_FREE_PAGES` handler

The analysis batches the dead data pages into a
pool-allocated linked list and pushes the list head. The
list layout (per the review's H2 finding):

```
Per-batch linked list, allocated as a chain of pool slots:
  Each slot: int64 next_batch_slot; int32 logical_page;
             (12 bytes padding)
  (16 bytes per slot = 1 pool slot, since 32 bytes is the
   slot size and we have 8+4+4 = 16 bytes used)

Encoding: pages_head is the VirtualPtr of the first batch
slot. pages_count is the number of logical pages in the
batch. The mirror sibling is NOT pre-computed in the
batch — it is identified at free time by reading the
PageHeader.
```

The handler iterates the list, freeing each logical page
via `storage_free`, then reads the PageHeader to find the
mirror sibling and frees it too:

```c
int gc_handle_free_pages(vfs_t* vfs, const BinEntry* entry) {
    TreeContext* ctx = vfs->ctx;
    int64_t pages_head  = entry->context;
    int64_t pages_count = entry->context2;
    int64_t slot = pages_head;
    for (int i = 0; i < pages_count && slot != 0; i++) {
        PoolSlot bs = {0};
        pool_acquire(&ctx->pool, slot, false, &bs);
        if (bs.vptr == VFS_VPTR_NULL) return VFS_ERR_IO;
        int32_t logical = vfs_rd4_s(bs.bytes, 8, ctx->page_size);
        int64_t next = vfs_rd8_s(bs.bytes, 0, ctx->page_size);
        pool_release(&ctx->pool, &bs);

        /* Free the logical page. */
        storage_free(ctx->sb, (int64_t)logical);

        /* Find and free the mirror sibling, if any.  The
           current `storage_free` does NOT free mirrors
           (per the §6.2 finding).  The work handler reads
           the PageHeader to find the mirror, matching the
           pattern in `deferred_free_enqueue` (src/gc.c).
           After reading the mirror, both pages are freed
           via `storage_free` (idempotent). */
        int64_t mirror = read_mirror_page(ctx->sb, (int64_t)logical);
        if (mirror >= 0) storage_free(ctx->sb, mirror);

        slot = next;
    }
    /* The batch slots themselves are not freed in this spec
       (pool_free is TODO-12).  They leak for now; an
       out-of-band shadow-compaction reclaims them.  See
       §4.3 for the expected leak rate. */
    return VFS_OK;
}
```

`read_mirror_page(sb, logical)` reads the PageHeader of
`logical` (via `indir_lookup` + `pread`) and returns
`ph.mirrorPage` if non-negative, else -1. This is the
same logic as `deferred_free_enqueue` in `src/gc.c`.

The handler is **idempotent**: `storage_free` checks
`indir_lookup` before acting; an already-freed page is a
no-op. Re-running the handler on the same batch is safe.

### 4.2 Stub for pool_free (and expected leak rate)

Pool slot freeing is **not** in this spec (TODO-12). The
chain slot freeing (FileNode, FileContent, PageNode,
FileSize, NameSlots, the create + tombstone DirContent
slots) is not done by this spec. The spec uses a stub
`pool_free` that returns success without action:

```c
/* STUB (TODO-12): real implementation pending. */
static inline int pool_free(Pool* pool, int64_t slot_vp) {
    (void)pool; (void)slot_vp;
    return VFS_OK;  /* slot leaks; out-of-band GC reclaims */
}
```

The slots are leaked per-process. The next shadow-compaction
(by an out-of-band GC pass, or by the future full-pool
rebuild) reclaims them.

**Expected leak rate (per the review's M2 finding):**

The `BIN_WORK_FREE_PAGES` work handler batches N dead data
pages into a linked list of pool slots. Each batch slot
is 32 bytes (1 pool slot holds the `next_batch_slot` +
`logical_page` + padding). The batch slots are leaked
per-process.

For a delete-heavy workload (e.g., extracting then
deleting a zip with 1000 files, each 100 pages):

- 1000 file-deletion triggers
- Each trigger has 100 dead pages → 100 batch slots
- Total batch slots leaked: 1000 × 100 = 100,000 slots
- At 32 bytes per slot: ~3.2 MB leak per workload

The chain slots (FileNode, FileContent, etc.) are also
leaked per-process. For the same workload, that's
~5,000 chain slots per file × 1000 files = 5,000,000 slots
(~160 MB leak) — much larger than the batch slot leak.

**Urgency:** the chain slot leak is significant. A future
implementation that doesn't address TODO-12 will see
substantial memory growth on delete-heavy workloads. The
spec's pre-MVP status (no real users) makes this
acceptable for now, but TODO-12 should land before
delete-heavy workloads are exercised in production.

**When TODO-12 lands:** the batch slots are freed by the
work handler after iterating the list, and the chain
slots are freed by the analysis (or a follow-up work
entry). The spec's behavior is unchanged.

---

## 5. Lock model

**Principle:** least disruptive. The user said:

> it needs to be done in the least disruptive way possible -
> you can forget the old GC implementation, that one will be
> fully dropped. the new one happens while the vfs is in use,
> so we should strive to avoid blocking the users if possible.

### 5.1 Chain walks — no lock

The trigger handler's chain walks (file's chain, parent
dir's chain) are read-only. The existing
`vfs_chain_walk` and `walk_anchor_chain` (in `src/tree.c`)
use by-value pool slots with `pinPage=false`, closing the
C1 hazard (Phase 25 fix). No lock is taken.

Concurrent readers (other GC threads, FUSE workers doing
reads) walk the same chains concurrently. By-value pool
slots mean the walks don't interfere with each other. No
data race.

### 5.2 Chain modifications — CAS

The dir-entry drop (§3.7) modifies the parent dir's
chain (dropping the create + tombstone). The
modification is CAS-based:

- Find the predecessor via walk.
- CAS-update the predecessor's nextPtr to skip the target
  entry.
- If the CAS fails (concurrent prepend), retry the walk +
  CAS.

The same pattern is used by `vfs_create` and `vfs_delete`
for their own chain modifications. No new locking primitive
is needed.

### 5.3 Page frees — no lock

`storage_free` is internally thread-safe (the indirection
table's CAS handles concurrent frees; the page cache
handles concurrent reads of the freed page). The mirror
sibling free is sequential (`storage_free(logical); if
(mirror) storage_free(mirror);`) but neither call is
blocking.

### 5.4 Pool slot frees — no lock (stub)

The stub `pool_free` is a no-op. The real implementation
(TODO-12) will use per-page CAS on the pool's `poolState`,
matching the existing `pool_alloc` pattern.

### 5.5 What is NOT taken

- No per-file lock (`vfs_lock(vfs, file, epoch)`) — the
  file is being deleted; taking the lock for a deleted
  file is awkward and unnecessary (no concurrent writer
  can succeed on a deleted file).
- No tree shared lock (`tree_lock_acquire_shared`) — the
  current §9.6 tree lock is unused by readers, so the
  bin job's walk doesn't need it; taking it would be
  needless overhead.
- No exclusive lock (`tree_lock_acquire_exclusive`) — that
  blocks ALL operations and is reserved for the future
  full shadow-compaction GC.

### 5.6 Lock duration

- Trigger handler: no lock held.
- `BIN_WORK_FREE_PAGES` handler: no lock held.
- `drop_dir_entries` (inline): no lock held; the CAS
  retries are bounded by the per-call retry limit (the
  same 1000-iteration cap as `bin_push`).

---

## 6. Page free

### 6.1 Which pages are freed

- The data pages identified by `classify_data_pages` (§3.4)
  as "not visible at any reference point".
- The mirror sibling of each such data page (read from
  PageHeader; if `mirrorPage != -1`, also freed).

### 6.2 The free mechanism

The work handler (`§4.1`) frees pages by calling
`storage_free(StorageBackend* sb, int64_t logical_page)`
(defined in `src/storage.c:1135`).

The current `storage_free`:
```c
void storage_free(StorageBackend* sb, int64_t logical_page) {
    if (logical_page < 2) return;  /* don't free header or superblock */
    int64_t phys = indir_lookup(sb, logical_page);
    if (phys == 0) return;  /* already free */
    indir_set(sb, logical_page, 0);
    cache_invalidate(&sb->cache, logical_page);
    enqueue_free_page(sb, logical_page, phys);  /* Phase 27 free-list */
}
```

**Finding: `storage_free` does NOT free the mirror sibling.**
The spec §3.3 says:

> `Free(logicalPage)`: marks the logical page as free. If a
> mirror sibling exists (`mirrorPage != -1`), the sibling
> is also freed.

But the current implementation is missing this. The
existing stop-the-world GC's `deferred_free_enqueue` (in
`src/gc.c`) handles the mirror explicitly:

```c
/* In src/gc.c, deferred_free_enqueue: */
if (append_ok && sb) {
    int64_t offset = indir_lookup(sb, logical_page);
    if (offset > 0) {
        PageHeader ph;
        ssize_t n = pread(sb->fd, &ph, PAGE_HEADER_SIZE, offset);
        if (n == PAGE_HEADER_SIZE && ph.mirror_page >= 0) {
            int64_t mirror_page = (int64_t)ph.mirror_page;
            /* ... enqueue mirror ... */
        }
    }
}
```

**Per the review's H2 finding:** the mirror free stays
in the **work handler** (§4.1), not in `storage_free`
itself. The work handler reads the PageHeader via
`read_mirror_page(sb, logical)`, finds the mirror, and
calls `storage_free` on both. This:

- Keeps `storage_free` as-is (frees only the logical page;
  matches the current behavior).
- Lets the work handler match the existing
  `deferred_free_enqueue` pattern.
- The mirror is found at free time, not pre-computed in
  the batch (saves space in the batch list; one pread per
  page is in noise vs. the free itself).

The work handler (§4.1) does:
```c
storage_free(ctx->sb, (int64_t)logical);
int64_t mirror = read_mirror_page(ctx->sb, (int64_t)logical);
if (mirror >= 0) storage_free(ctx->sb, mirror);
```

Note: the analysis (`classify_data_pages`, §3.4) does
NOT identify the mirror — it only adds the logical page
to the dead set. The mirror is identified at free time by
the work handler.

### 6.3 Why the free does not need a lock

Per SPEC §3.3 and the existing GC's analysis, the
synchronization excluding readers is in place:

- For data pages: a data page's "liveness" is determined
  by the read rule at each reference point. The bin job
  classifies the page based on the current state. After
  classification, if the page is dead, no reference point
  reads it. Concurrent reads via the page cache are safe
  because `storage_free` calls `cache_invalidate` to
  mark the page as stale.
- For mirror siblings: the mirror is freed after the
  logical page. The mirror is reachable only via the
  PageHeader's `mirrorPage` field of the logical page.
  Once the logical page is freed (and the indirection
  entry is 0), no reader can resolve the logical page →
  no reader can find the mirror.

The existing free-list mechanism (`enqueue_free_page`)
and the Phase 27 free-page queue handle the on-disk
indirection update. The free is crash-safe: a crash
mid-free leaves the indirection entry non-zero, and the
next mount's `validate_free_list_on_mount` (Phase 27 W5)
rebuilds the free list.

---

## 7. Crash safety

### 7.1 Crash windows

| Window | State on disk after crash | Recovery on next mount |
|---|---|---|
| After vfs_delete pushed the trigger, before the trigger is popped | Trigger is in the Bin (or the Bin's per-page CAS is mid-update) | Mount-time `validate_bin_on_mount` (Phase 28 W1) rebuilds `bin_count` and verifies the per-page counts. The trigger is re-processed by the GC thread. |
| Trigger popped, analysis mid-walk (chain walk in progress) | Bin entry is gone (popped); the analysis is in-memory only | The trigger is lost. No GC work happens for this deletion. The garbage accumulates; a future related producer (e.g., another `vfs_delete` in the same parent dir, or a future `vfs_commit`) may re-push a related trigger. The garbage is reclaimed by the next shadow-compaction (out-of-band GC pass). |
| Analysis done, BIN_WORK_FREE_PAGES pushed, before the work is popped | Work entry is in the Bin | The work is re-processed by the GC thread. `storage_free` is idempotent. |
| BIN_WORK_FREE_PAGES popped, mid-free | The logical page is in the free-list (Phase 27). The mirror may or may not be freed depending on where the crash happened. | The free-list's on-disk state is consistent (Phase 27 W5: payload CRC repair + free-list validator). The page is reclaimable on the next free. If the mirror is leaked, the next mount's free-list validator reclaims it (the mirror's indirection entry is still non-zero, but no logical page references it). |
| `drop_dir_entries` (inline in analysis) mid-CAS | The chain is partially modified: the create or tombstone may be removed; the other may still be in the chain. | The handler is idempotent. If the trigger is re-pushed, the analysis re-walks the chain and applies the remaining modification. If the trigger is lost (bin crash window), the chain is left partially modified until the next `vfs_commit` or `vfs_delete_snapshot` re-evaluates the file's chain. |

### 7.2 Idempotency story

- **Trigger analysis:** re-running on the same trigger
  context produces the same result (modulo state
  changes). The chain walks are deterministic given the
  current state.
- **`BIN_WORK_FREE_PAGES`:** `storage_free` is idempotent
  (checks `indir_lookup` before acting).
- **`drop_dir_entries` (inline):** the CAS-based removal
  is retry-safe; if the create + tombstone are already
  gone, the walk finds nothing and the handler is a
  no-op.

### 7.3 Bin entry state machine

Per the framework spec, the Bin entry is in one of:
- **In Bin** (per-page CAS incremented, not yet popped)
- **Popped** (per-page CAS decremented by the GC thread
  via `bin_pop`)

The trigger is popped before the analysis runs. If the
process crashes after the pop but before the analysis
completes, the trigger is lost. This is a known
trade-off (per the framework spec's crash safety story);
the garbage is reclaimed by the next out-of-band GC pass.

The work entries pushed by the analysis are also in the
Bin. If the process crashes after the work is pushed but
before the work is processed, the work is re-processed
on the next mount (via the Bin's mount-time validation).

---

## 8. Test plan

### 8.1 Unit tests

In `test/test_gc.c` (existing file, extend with new test
cases):

- `test_file_deleted_active_snapshot_data_pages` —
  vfs_delete in an active snapshot; verify the COW data
  pages are freed and the head-visible data pages are
  not. Setup: create file, write 100 pages, take
  snapshot, write 10 more pages in the snapshot (COW),
  vfs_delete in the snapshot, wait for GC, verify the
  10 COW pages are freed and the 100 head pages are not.
- `test_file_deleted_head_with_active_snapshots` —
  vfs_delete at the head with active snapshots holding
  the file. Verify the file's data pages are NOT freed
  (the snapshot still references them).
- `test_file_deleted_head_no_snapshots` — vfs_delete at
  the head with no active snapshots. Verify all data
  pages are freed, all chain slots are leaked (stub),
  the create + tombstone are dropped from the parent
  dir's chain.
- `test_file_deleted_soft_deleted_snapshot` — vfs_delete
  in a snapshot, then vfs_delete_snapshot, then wait
  for GC. Verify the data pages and create + tombstone
  are freed.
- `test_file_deleted_committed_snapshot` — vfs_delete in
  a snapshot, then vfs_commit, then wait for GC. Verify
  the COW data page (now head-visible via remap) is NOT
  freed; the pre-snapshot data pages are freed (no
  active snapshots hold them).

### 8.2 Integration tests

In `test/test_gc_thread.c` (existing file, extend):

- `test_file_deletion_through_gc_thread` — full
  end-to-end: FUSE worker calls vfs_delete, the
  trigger is pushed to the Bin, the GC thread pops it
  and processes it, the data pages are freed, the
  create + tombstone are removed. Verify the file is
  fully gone (not visible at any reference point).
- `test_file_deletion_with_concurrent_creates` — FUSE
  worker calls vfs_delete; another worker concurrently
  calls vfs_create in the same parent dir. Verify the
  CAS-based drop handles the concurrent prepend without
  losing the new create.
- `test_file_deletion_with_concurrent_reads` — FUSE
  worker calls vfs_delete; another worker concurrently
  reads the file. Verify the read succeeds (the data
  pages are not freed until the GC thread processes
  the trigger) and the read returns the correct data
  (the file is still visible at the head if the
  deletion is in an active snapshot).

### 8.3 Crash safety tests

In `test/test_crash.c` (existing file, extend):

- `test_crash_after_trigger_pushed` — abort the process
  after vfs_delete pushes the trigger but before the
  GC thread processes it. On remount, verify the
  trigger is re-processed and the data pages are freed.
- `test_crash_mid_free` — abort the process during
  `storage_free` of a data page. On remount, verify the
  free-list is consistent (Phase 27 W5) and the
  remaining dead data pages are freed on the next GC
  cycle.
- `test_crash_mid_drop` — abort the process during the
  CAS-based drop of the create + tombstone. On remount,
  verify the chain is consistent (the create or
  tombstone may be missing; the handler re-runs and
  cleans up the other).

### 8.4 Performance tests

In `bench/bench.c` (existing file, add a new workload):

- `bench_file_deletion` — create N files, write 100
  pages to each, delete all, measure the per-delete
  latency (vfs_delete returns quickly because the work
  is deferred) and the GC thread's drain time.
- Expected: per-delete latency is unchanged from the
  pre-this-spec baseline (the bin_push is ~600-1300 ns
  per Phase 28 W4 measurement). The GC thread drains
  the Bin in a few hundred milliseconds for N=1000.

### 8.5 Mirror handling test

In `test/test_gc.c`:

- `test_file_deletion_frees_mirror_sibling` — create a
  file, write a page twice (triggers mirror allocation
  per SPEC §3.7), vfs_delete, wait for GC, verify
  both the logical page and its mirror sibling are
  freed.

---

## 9. Open questions

### 9.1 Pool slot freeing is deferred

TODO-12. The chain slot freeing (FileNode, FileContent,
PageNode, FileSize, NameSlots, the create + tombstone
DirContent slots) is not done by this spec. The slots
are leaked per-process. The next shadow-compaction (by
an out-of-band GC pass, or by the future full-pool
rebuild) reclaims them.

If TODO-12 lands before this spec is implemented, the
stub `pool_free` is replaced with the real
implementation. The spec's behavior is unchanged.

### 9.2 Reference point iteration cost

The analysis iterates active snapshots in
`collect_active_snapshots`. For systems with many
active snapshots, this is O(active_snapshots) per
trigger. The mapper is a small in-memory dict; the
iteration is O(mapper_size) worst case. For typical
workloads (a few active snapshots at a time), this is
in noise.

If the iteration becomes a bottleneck, a future
optimization can cache the active snapshot set in
`ctx` (updated on `vfs_snapshot`, `vfs_commit`,
`vfs_delete_snapshot`). The spec's analysis uses the
un-cached version for now.

### 9.3 ~~Interaction with the chain index~~ (RESOLVED — see §3.8)

**Resolved by §3.8.** Per the review's M3 finding, the
spec uses option (a): the analysis calls
`dircontentindex_remove` after `drop_dir_entries`
removes the entries. This is one CAS per dropped
entry, not a full index rebuild. The existing
`dircontentindex_remove` in `src/tree.c:3147` is
reused.

### 9.4 Snapshot at the same epoch as the file's create

If a file is created at even E and deleted at the same
even E (with no intervening snapshot), the read rule at
H (where H > E) picks the tombstone (most recent at E).
The file is deleted at H. The analysis correctly
classifies the data pages as freeable (no live
reference point).

But the read rule at H is "first even below H". If E is
the highest even < H, the tombstone is picked. If a
later write to the file happened at an even E' with E
< E' < H, the read rule at H picks the E' entry instead.
The analysis walks the file's chain and applies the read
rule at H to each VersionPage; the classification is
correct.

### 9.5 Interaction with BIN_TRIGGER_EPOCH_COMMITTED and BIN_TRIGGER_EPOCH_SOFT_DELETED

These are separate bin activities (per the user's
2026-07-18 clarification). When a snapshot is committed
or soft-deleted, the corresponding bin job (defined in
its own spec) re-evaluates the files that were deleted
in the snapshot. The current spec is concerned only with
the file-deletion trigger; the snapshot-resolution specs
are independent.

If the snapshot is committed and a file was deleted in
it, the committed mapper makes the tombstone visible at
H. The file becomes "deleted at the head" via the
remap. The committed bin job (separate spec) re-walks
the file's chain (or the tombstone's chain) and
identifies the now-freeable data pages. The two bin
jobs (this spec's file-deletion + the future commit
spec) coordinate via the shared state (mapper, chain
visibility), not via direct message passing.

If the snapshot is soft-deleted, the tombstone becomes
invisible at H. The soft-delete bin job (separate spec)
drops the tombstone (and the create, if applicable)
from the chain. The file-deletion bin job's leftover
state (the tombstone) is cleaned up by the soft-delete
bin job.

---

## 10. Source

### Spec review

`PHASE28_BINJOB_FILEDEL_REVIEW.md` — the external
reviewer's findings. Resolved as follows:

- H1: §2.3 (design decision: dir-entry drop is inline),
  §3.3 (analysis calls `drop_dir_entries` directly), §4
  (only `BIN_WORK_FREE_PAGES` is a work type).
- H2: §4.1 (work handler reads PageHeader for mirror),
  §6.2 (`storage_free` stays as-is; mirror free in work
  handler).
- M1: §3.7 Phase C (the two-CAS unlink is correct;
  reader/writer safety explained).
- M2: §4.2 (expected leak rate documented: ~3.2 MB
  batch slots + ~160 MB chain slots per 1000-file
  delete workload).
- M3: §3.8 (chain index is updated via
  `dircontentindex_remove`, one CAS per drop).
- L1: `Constants` section (MAX_ACTIVE_SNAPSHOTS = 64,
  overflow behavior: latest 64 only, earlier shadowed).
- L2: §2.1 (`BIN_TRIGGER_FILE_DELETED = 1`), §2.2
  (`BIN_WORK_FREE_PAGES = 0x100`).
- L3: §3.1 (active snapshot set cache is the
  recommended implementation).

### Brainstorming

The bin job's motivation and the trigger/work type
structure come from `impl/phase-28-gc.md` §1, §3, and
§5 (the per-bin-job spec template).

### Existing code

- `src/tree.c::vfs_delete` (line 1770) — the producer
  push call site. The current W3 placeholder at line
  1902 (`bin_push(ctx->sb, BIN_TRIGGER_NOOP, ...)`) is
  replaced by the new push in §2.4.
- `src/tree.c::vfs_chain_walk` (line 791) — the
  per-leaf chain walker that the trigger analysis
  reuses.
- `src/tree.c::walk_anchor_chain` (line 855) — the
  per-anchor chain walker that the trigger analysis
  reuses for the parent dir's SlotNode chain.
- `src/storage.c::storage_free` (line 1135) — the page
  free primitive. The mirror handling is added in §3.4
  and §4.1.
- `src/bin.h` / `src/bin.c` — the framework's Bin API
  (`bin_push`, `bin_pop`).
- `src/gc.h` / `src/gc.c` — the framework's GC thread
  and dispatch. The new trigger and work types are
  added in `src/bin.h` and the new dispatch cases in
  `src/gc.c::gc_handle_trigger` and
  `src/gc.c::gc_handle_work`.

### ISSUES.md entries

- N2 (GC coupled with code paths): the new per-bin-job
  GC decouples the GC from the producer's hot path. The
  bin_push is ~600-1300 ns; the actual free is deferred.
  N2 is **partially fixed** by this spec; fully fixed
  when the other bin jobs (commit, soft-delete, truncate)
  land.
- M4 (GC M4 / per-type field descriptor): unrelated to
  this spec. M4 was fixed in Phase 26 W5e.
- L4 (Page-2 indirection reset): unrelated to this spec.

### Prior art

The "act now based on current state" pattern is borrowed
from the existing `deferred_free_enqueue` in `src/gc.c`,
which classifies pages based on the current read state
without deferring for snapshot resolution. The per-entry
read rule with mapper remap is the same as the
existing `vfs_chain_walk` in `src/tree.c`.
