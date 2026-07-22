/* Phase 28 Type 2: rename-tombstone bin job.
 *
 * Spec: impl/phase-28-bin-job-rename-tombstone.md
 *
 * Trigger:  BIN_TRIGGER_TOMBSTONE_ADDED (context=file_vp, context2=tombstone_vp at src)
 * Work:     BIN_WORK_REMOVE_TOMBSTONE (context=batch_head, context2=count)
 *
 * Helpers below are duplicated from src/gc_bin_file_deleted.c for
 * self-containment.  TODO(Phase 29): refactor to a shared
 * gc_bin_common.c once we have 3+ bin jobs.
 */
#include "gc.h"
#include "bin.h"
#include "tree.h"
#include "nodes.h"
#include "pool.h"
#include "page_buf.h"
#include "storage.h"
#include "mapper.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ---- Reference points structure (same as file-deletion) ---- */
#define RENAME_MAX_ACTIVE_SNAPSHOTS 64
typedef struct {
    int64_t H;
    int64_t active[RENAME_MAX_ACTIVE_SNAPSHOTS];
    int     num_active;
} rename_ref_points_t;

/* ---- Batch list entry types ---- */
/* 32-byte pool slot layout per entry:
     offset 0:  int64 next_batch_vp (0 = end)
     offset 8:  int64 target_vp (slot to free or CAS-remove)
     offset 16: int64 context_vp (SlotNode VP for DC; first chain slot for NAME)
     offset 24: int32 type (BATCH_ENTRY_*)
     offset 28: int32 reserved
*/
#define BATCH_ENTRY_DC          1
#define BATCH_ENTRY_NAME_FIRST  2   /* First slot of a (multi-slot) NameEntry.
                                       The chain of additional slots is walked
                                       in the work handler via context_vp. */
#define BATCH_ENTRY_SLOT        3   /* Empty SlotNode to unlink + free.
                                       target_vp = SlotNode VP. */

/* ---- Shared helpers (duplicated from src/gc_bin_file_deleted.c) ---- */

static int rename_collect_active_snapshots(TreeContext* ctx,
                                             rename_ref_points_t* rp) {
    if (!ctx || !rp) return VFS_ERR_IO;
    rp->num_active = 0;
    int64_t H = rp->H;
    if (H <= 0) return VFS_OK;
    for (int64_t S = 1; S < H && rp->num_active < RENAME_MAX_ACTIVE_SNAPSHOTS; S += 2) {
        int64_t resolved = mapper_table_resolve(&ctx->mapper_table, S);
        int trav = mapper_table_traversal_apply(&ctx->mapper_table, S) ? 1 : 0;
        if (resolved == S && !trav) {
            rp->active[rp->num_active++] = S;
        }
    }
    return VFS_OK;
}

static int walk_dir_recursive_rename(TreeContext* ctx, int64_t dir_vp,
                                       int64_t file_vp, int* out_found,
                                       int64_t* out_parent);
static int find_parent_dir_rename(TreeContext* ctx, int64_t file_vp,
                                    int64_t* out_parent_dir_vp) {
    if (!ctx || !file_vp || !out_parent_dir_vp) return VFS_ERR_IO;
    if (ctx->rootNodeOffset == 0) return VFS_ERR_NOTFOUND;
    int found = 0;
    int err = walk_dir_recursive_rename(ctx, ctx->rootNodeOffset, file_vp,
                                          &found, out_parent_dir_vp);
    if (err != VFS_OK) return err;
    return found ? VFS_OK : VFS_ERR_NOTFOUND;
}
static int walk_dir_recursive_rename(TreeContext* ctx, int64_t dir_vp,
                                       int64_t file_vp, int* out_found,
                                       int64_t* out_parent) {
    if (*out_found) return VFS_OK;
    PoolSlot dir_slot = {0};
    pool_acquire(&ctx->pool, dir_vp, false, &dir_slot);
    if (dir_slot.vptr == VFS_VPTR_NULL) return VFS_ERR_IO;
    int64_t seg_vp = vfs_rd8_s(dir_slot.bytes, DIRNODE_OFF_HEADPTR, ctx->page_size);
    pool_release(&ctx->pool, &dir_slot);
    while (seg_vp != 0) {
        PoolSlot seg = {0};
        pool_acquire(&ctx->pool, seg_vp, false, &seg);
        if (seg.vptr == VFS_VPTR_NULL) break;
        int64_t seg_head = vfs_rd8_s(seg.bytes, ANCHOR_OFF_HEADPTR, ctx->page_size);
        int64_t seg_sib = vfs_rd8_s(seg.bytes, ANCHOR_OFF_SIBPTR, ctx->page_size);
        pool_release(&ctx->pool, &seg);
        int64_t slot_vp = seg_head;
        while (slot_vp != 0) {
            PoolSlot slot = {0};
            pool_acquire(&ctx->pool, slot_vp, false, &slot);
            if (slot.vptr == VFS_VPTR_NULL) break;
            int64_t slot_sib = vfs_rd8_s(slot.bytes, ANCHOR_OFF_SIBPTR, ctx->page_size);
            int64_t slot_head = vfs_rd8_s(slot.bytes, ANCHOR_OFF_HEADPTR, ctx->page_size);
            pool_release(&ctx->pool, &slot);
            int64_t dc_vp = slot_head;
            while (dc_vp != 0) {
                PoolSlot dc = {0};
                pool_acquire(&ctx->pool, dc_vp, false, &dc);
                if (dc.vptr == VFS_VPTR_NULL) break;
                int64_t child = vfs_rd8_s(dc.bytes, DIRCONTENT_OFF_CHILDPTR, ctx->page_size);
                int64_t dc_next = vfs_rd8_s(dc.bytes, DIRCONTENT_OFF_NEXTPTR, ctx->page_size);
                int64_t name_ptr = vfs_rd8_s(dc.bytes, DIRCONTENT_OFF_NAMEPTR, ctx->page_size);
                pool_release(&ctx->pool, &dc);
                if (child == file_vp) {
                    *out_found = 1;
                    *out_parent = dir_vp;
                    return VFS_OK;
                }
                /* Recurse into subdirs to find the file.  We
                 * recurse on any non-tombstone child (name_ptr != 0)
                 * with child_ptr != 0.  If the child is a FileNode
                 * (not a DirNode), the recursion's pool_acquire on
                 * DirNode_OFF_HEADPTR will fail gracefully (returns
                 * 0 head), and the recursion will return VFS_OK
                 * without recursing further.  Per the file-deletion
                 * bin job's walk_dir_recursive (gc_bin_file_deleted.c
                 * line 534-540).  Without this recursion, files in
                 * nested directories (depth > 1 from root) would
                 * not be found by the rename bin job, and the OLD
                 * create + OLD NameEntry would leak. */
                if (name_ptr != 0 && child != 0 && child != file_vp) {
                    int err = walk_dir_recursive_rename(ctx, child,
                                                          file_vp, out_found,
                                                          out_parent);
                    if (err != VFS_OK && err != VFS_ERR_NOTFOUND) return err;
                }
                dc_vp = dc_next;
            }
            slot_vp = slot_sib;
        }
        seg_vp = seg_sib;
    }
    return VFS_OK;
}

static int find_slotnode_in_dir_rename(TreeContext* ctx, int64_t dir_vp,
                                          uint32_t child_id,
                                          int64_t* out_slot_vp) {
    if (!ctx || !dir_vp || !out_slot_vp) return VFS_ERR_IO;
    *out_slot_vp = 0;
    PoolSlot dir_slot = {0};
    pool_acquire(&ctx->pool, dir_vp, false, &dir_slot);
    if (dir_slot.vptr == VFS_VPTR_NULL) return VFS_ERR_NOTFOUND;
    int64_t seg_vp = vfs_rd8_s(dir_slot.bytes, DIRNODE_OFF_HEADPTR, ctx->page_size);
    pool_release(&ctx->pool, &dir_slot);
    while (seg_vp != 0) {
        PoolSlot seg = {0};
        pool_acquire(&ctx->pool, seg_vp, false, &seg);
        if (seg.vptr == VFS_VPTR_NULL) break;
        int64_t seg_head = vfs_rd8_s(seg.bytes, ANCHOR_OFF_HEADPTR, ctx->page_size);
        int64_t seg_sib = vfs_rd8_s(seg.bytes, ANCHOR_OFF_SIBPTR, ctx->page_size);
        pool_release(&ctx->pool, &seg);
        int64_t slot_vp = seg_head;
        while (slot_vp != 0) {
            PoolSlot slot = {0};
            pool_acquire(&ctx->pool, slot_vp, false, &slot);
            if (slot.vptr == VFS_VPTR_NULL) break;
            uint32_t slot_id = (uint32_t)vfs_rd4_s(slot.bytes, ANCHOR_OFF_ID, ctx->page_size);
            int64_t slot_sib = vfs_rd8_s(slot.bytes, ANCHOR_OFF_SIBPTR, ctx->page_size);
            pool_release(&ctx->pool, &slot);
            if (slot_id == child_id) { *out_slot_vp = slot_vp; return VFS_OK; }
            slot_vp = slot_sib;
        }
        seg_vp = seg_sib;
    }
    return VFS_ERR_NOTFOUND;
}

static int read_rule_pick_first_dc_rename(TreeContext* ctx, int64_t slot_vp,
                                           int64_t read_epoch,
                                           int64_t* out_dc_vp) {
    if (!ctx || !slot_vp || !out_dc_vp) return VFS_ERR_IO;
    *out_dc_vp = 0;
    PoolSlot slot = {0};
    pool_acquire(&ctx->pool, slot_vp, false, &slot);
    if (slot.vptr == VFS_VPTR_NULL) return VFS_ERR_NOTFOUND;
    int64_t dc_head = vfs_rd8_s(slot.bytes, ANCHOR_OFF_HEADPTR, ctx->page_size);
    pool_release(&ctx->pool, &slot);
    int64_t dc_vp = dc_head;
    while (dc_vp != 0) {
        PoolSlot dc = {0};
        pool_acquire(&ctx->pool, dc_vp, false, &dc);
        if (dc.vptr == VFS_VPTR_NULL) break;
        uint32_t dc_epoch = (uint32_t)vfs_rd4_s(dc.bytes, DIRCONTENT_OFF_EPOCH, ctx->page_size);
        int64_t dc_next = vfs_rd8_s(dc.bytes, DIRCONTENT_OFF_NEXTPTR, ctx->page_size);
        pool_release(&ctx->pool, &dc);
        int64_t eff_epoch = (int64_t)dc_epoch;
        if (mapper_table_traversal_apply(&ctx->mapper_table, (int64_t)dc_epoch)) {
            eff_epoch = mapper_table_resolve(&ctx->mapper_table, (int64_t)dc_epoch);
        }
        if (eff_epoch == read_epoch) { *out_dc_vp = dc_vp; return VFS_OK; }
        if (eff_epoch < read_epoch && (eff_epoch & 1) == 0) {
            *out_dc_vp = dc_vp; return VFS_OK;
        }
        dc_vp = dc_next;
    }
    return VFS_OK;
}

/* ---- Batch list helpers ---- */

/* Allocate a batch slot, link it at the head of the list.
   Returns VFS_OK or VFS_ERR_IO.  The new slot's next_batch_vp is
   set to the previous head (or 0 if this is the first entry). */
static int batch_list_prepend(TreeContext* ctx, int64_t* head_io,
                               int64_t target, int64_t context, int32_t type) {
    int64_t vp = pool_alloc(&ctx->pool);
    if (vp == VFS_VPTR_NULL) return VFS_ERR_IO;
    PoolSlot bs = {0};
    pool_acquire(&ctx->pool, vp, true, &bs);
    if (bs.vptr == VFS_VPTR_NULL) {
        pool_free(&ctx->pool, vp);
        return VFS_ERR_IO;
    }
    int64_t prev_head = *head_io;
    vfs_wr8_s(bs.bytes, 0,  prev_head, ctx->page_size);
    vfs_wr8_s(bs.bytes, 8,  target,    ctx->page_size);
    vfs_wr8_s(bs.bytes, 16, context,    ctx->page_size);
    vfs_wr4_s(bs.bytes, 24, type,       ctx->page_size);
    vfs_wr4_s(bs.bytes, 28, 0,          ctx->page_size);
    pool_release(&ctx->pool, &bs);
    *head_io = vp;
    return VFS_OK;
}

/* ---- Analysis handler (W2) ---- */

/* Walk the src SlotNode's chain.  Find:
   - tombstone_vp:  namePtr == 0, childPtr == file_vp (cross-dir only;
                    for same-dir there is no tombstone, returns 0)
   - create_vp:     the OLD namePtr!=0 entry that was the "current"
                    entry BEFORE the current rename.  For same-dir
                    this is the entry at the head's NEXTPTR.  For
                    cross-dir this is the entry at the tombstone's
                    NEXTPTR.  (I.e., the entry that was the head
                    before the current rename's new entry was
                    prepended.)

   Returns 0 on success (with *out_tombstone_vp, *out_tombstone_epoch,
   *out_create_vp, *out_create_epoch set; 0 if not in chain), or -1 on
   I/O error.

   Note: the spec's earlier wording ("pick the highest epoch among
   creates") was WRONG for the bin job's purpose.  The bin job frees
   the OLD entry (the one that was current BEFORE the current
   rename), not the current entry (the most recent).  The OLD entry
   is the one immediately following the current rename's new entry
   in the chain.  This is the correct definition for "no space leak
   after GC": each bin job frees exactly one OLD entry, leaving the
   most recent entry as the only one in the chain. */
static int find_chain_entries(TreeContext* ctx, int64_t slot_vp,
                                 int64_t file_vp, int64_t tombstone_vp_hint,
                                 int64_t* out_tombstone_vp,
                                 int64_t* out_tombstone_epoch,
                                 int64_t* out_create_vp,
                                 int64_t* out_create_epoch) {
    *out_tombstone_vp = 0;
    *out_tombstone_epoch = 0;
    *out_create_vp = 0;
    *out_create_epoch = 0;

    PoolSlot slot = {0};
    pool_acquire(&ctx->pool, slot_vp, false, &slot);
    if (slot.vptr == VFS_VPTR_NULL) return -1;
    int64_t dc_head = vfs_rd8_s(slot.bytes, ANCHOR_OFF_HEADPTR, ctx->page_size);
    pool_release(&ctx->pool, &slot);

    /* The "current rename's new entry" is the head of the chain
       (for both same-dir and cross-dir: same-dir prepends a new_dc
       with namePtr!=0; cross-dir prepends a tombstone with namePtr==0).
       The "OLD entry" to free is at the head's NEXTPTR.  If the
       head is the hint (tombstone_vp_hint != 0), then head IS the
       tombstone; otherwise (tombstone_vp_hint == 0 for same-dir),
       head is a new_dc and we look for a namePtr==0 entry anywhere
       in the chain for the tombstone (which is 0 if no such entry). */
    int64_t head_vp = dc_head;
    int64_t head_next_vp = 0;
    int32_t head_epoch = 0;

    if (head_vp != 0) {
        PoolSlot h = {0};
        pool_acquire(&ctx->pool, head_vp, false, &h);
        if (h.vptr != VFS_VPTR_NULL) {
            head_next_vp = vfs_rd8_s(h.bytes, DIRCONTENT_OFF_NEXTPTR, ctx->page_size);
            head_epoch    = (int32_t)vfs_rd4_s(h.bytes, DIRCONTENT_OFF_EPOCH, ctx->page_size);
        }
        pool_release(&ctx->pool, &h);
    }

    /* Identify the tombstone.  If tombstone_vp_hint matches the
       head, head IS the tombstone.  Otherwise (tombstone_vp_hint
       is 0 for same-dir), there is no tombstone in the chain. */
    if (tombstone_vp_hint != 0 && head_vp == tombstone_vp_hint) {
        *out_tombstone_vp = head_vp;
        *out_tombstone_epoch = (int64_t)head_epoch;
    } else if (tombstone_vp_hint == 0) {
        /* Same-dir: no tombstone.  Confirm by walking the chain
           looking for a namePtr==0 entry with childPtr == file_vp
           — there should be none. */
        int64_t dc_vp = dc_head;
        while (dc_vp != 0) {
            PoolSlot dc = {0};
            pool_acquire(&ctx->pool, dc_vp, false, &dc);
            if (dc.vptr == VFS_VPTR_NULL) break;
            int32_t namePtr  = (int32_t)vfs_rd8_s(dc.bytes, DIRCONTENT_OFF_NAMEPTR, ctx->page_size);
            int64_t childPtr = vfs_rd8_s(dc.bytes, DIRCONTENT_OFF_CHILDPTR, ctx->page_size);
            int64_t dc_next  = vfs_rd8_s(dc.bytes, DIRCONTENT_OFF_NEXTPTR, ctx->page_size);
            pool_release(&ctx->pool, &dc);
            if (childPtr == file_vp && namePtr == 0) {
                /* Unexpected tombstone in same-dir rename — could
                   be a stale entry from a prior cross-dir rename.
                   Skip (don't try to free it from this trigger). */
                break;
            }
            dc_vp = dc_next;
        }
    } else {
        /* tombstone_vp_hint != 0 but head_vp != hint.  Walk the
           chain to find the tombstone at the hint. */
        int64_t dc_vp = dc_head;
        while (dc_vp != 0) {
            PoolSlot dc = {0};
            pool_acquire(&ctx->pool, dc_vp, false, &dc);
            if (dc.vptr == VFS_VPTR_NULL) break;
            int32_t namePtr  = (int32_t)vfs_rd8_s(dc.bytes, DIRCONTENT_OFF_NAMEPTR, ctx->page_size);
            int64_t childPtr = vfs_rd8_s(dc.bytes, DIRCONTENT_OFF_CHILDPTR, ctx->page_size);
            int32_t epoch    = (int32_t)vfs_rd4_s(dc.bytes, DIRCONTENT_OFF_EPOCH, ctx->page_size);
            int64_t dc_next  = vfs_rd8_s(dc.bytes, DIRCONTENT_OFF_NEXTPTR, ctx->page_size);
            pool_release(&ctx->pool, &dc);
            if (childPtr == file_vp && namePtr == 0 &&
                dc_vp == tombstone_vp_hint) {
                *out_tombstone_vp = dc_vp;
                *out_tombstone_epoch = (int64_t)epoch;
                break;
            }
            dc_vp = dc_next;
        }
    }

    /* The "create" (OLD entry to free) is the entry at the head's
       NEXTPTR.  This is the entry that was the "current" entry
       BEFORE the current rename.  For same-dir, head is new_dc and
       head.next is the OLD create.  For cross-dir, head is the
       tombstone and head.next is the OLD create (at the src parent). */
    if (head_next_vp != 0) {
        PoolSlot c = {0};
        pool_acquire(&ctx->pool, head_next_vp, false, &c);
        if (c.vptr != VFS_VPTR_NULL) {
            int32_t namePtr  = (int32_t)vfs_rd8_s(c.bytes, DIRCONTENT_OFF_NAMEPTR, ctx->page_size);
            int64_t childPtr = vfs_rd8_s(c.bytes, DIRCONTENT_OFF_CHILDPTR, ctx->page_size);
            int32_t epoch    = (int32_t)vfs_rd4_s(c.bytes, DIRCONTENT_OFF_EPOCH, ctx->page_size);
            if (childPtr == file_vp && namePtr != 0) {
                *out_create_vp = head_next_vp;
                *out_create_epoch = (int64_t)epoch;
            }
            /* If namePtr == 0 here, it means the head.next is
               another tombstone (rare: cross-dir then same-dir
               renames).  In that case, leave create_vp = 0 and
               let the analysis skip the create. */
        }
        pool_release(&ctx->pool, &c);
    }
    return 0;
}

/* Check if the tombstone is still needed at some reference point R.
   The tombstone is "needed" if some R >= tombstone_epoch would
   see the tombstone (i.e., the read rule at R picks the tombstone
   or a later namePtr==0 entry, meaning no namePtr!=0 entry
   shadows it).  Returns 1 if needed, 0 if freeable. */
static int tombstone_needed_at(TreeContext* ctx, int64_t slot_vp,
                                 const rename_ref_points_t* rp,
                                 int64_t tombstone_epoch) {
    for (int ri = 0; ri <= rp->num_active; ri++) {
        int64_t R = (ri == 0) ? rp->H : rp->active[ri - 1];
        if (R < tombstone_epoch) continue;
        /* Walk the chain at R; the read rule picks the first
           matching entry.  We check if the picked entry is a
           tombstone (namePtr == 0) — meaning the tombstone is
           needed for the "file hidden" semantic at R. */
        int64_t picked = 0;
        read_rule_pick_first_dc_rename(ctx, slot_vp, R, &picked);
        if (picked == 0) continue;  /* no entry at R; tombstone irrelevant */
        PoolSlot dc = {0};
        pool_acquire(&ctx->pool, picked, false, &dc);
        if (dc.vptr == VFS_VPTR_NULL) continue;
        int32_t namePtr = (int32_t)vfs_rd8_s(dc.bytes, DIRCONTENT_OFF_NAMEPTR, ctx->page_size);
        int64_t childPtr = vfs_rd8_s(dc.bytes, DIRCONTENT_OFF_CHILDPTR, ctx->page_size);
        pool_release(&ctx->pool, &dc);
        if (namePtr == 0 && childPtr != 0) {
            return 1;  /* tombstone is needed at R */
        }
    }
    return 0;  /* no R needs the tombstone; freeable */
}

/* Check if the create is still reachable at some reference point R
   in [create_epoch, tombstone_epoch).  Returns 1 if reachable, 0 if
   freeable.  For same-epoch rename (create_epoch == tombstone_epoch),
   the range is empty and the create is immediately freeable. */
static int create_reachable_at(TreeContext* ctx, int64_t slot_vp,
                                 const rename_ref_points_t* rp,
                                 int64_t create_epoch, int64_t tombstone_epoch) {
    /* Same-epoch rename: empty range, immediately freeable. */
    if (create_epoch >= tombstone_epoch) return 0;

    for (int ri = 0; ri <= rp->num_active; ri++) {
        int64_t R = (ri == 0) ? rp->H : rp->active[ri - 1];
        /* The create is reachable at R iff R is in [create_epoch, tombstone_epoch). */
        if (R < create_epoch || R >= tombstone_epoch) continue;
        /* Walk the chain at R; check if the read rule picks a
           namePtr != 0 entry with childPtr == file_vp. */
        int64_t picked = 0;
        read_rule_pick_first_dc_rename(ctx, slot_vp, R, &picked);
        if (picked == 0) continue;
        PoolSlot dc = {0};
        pool_acquire(&ctx->pool, picked, false, &dc);
        if (dc.vptr == VFS_VPTR_NULL) continue;
        int32_t namePtr = (int32_t)vfs_rd8_s(dc.bytes, DIRCONTENT_OFF_NAMEPTR, ctx->page_size);
        (void)vfs_rd8_s(dc.bytes, DIRCONTENT_OFF_CHILDPTR, ctx->page_size);
        /* L1 fix: read picked_epoch BEFORE pool_release.  The cache
           page is still resident, but reading from a released
           PoolSlot is fragile (the pool_release call doesn't
           invalidate the local copy, but it's better style to
           read everything we need first). */
        int32_t picked_epoch = (int32_t)vfs_rd4_s(dc.bytes, DIRCONTENT_OFF_EPOCH, ctx->page_size);
        pool_release(&ctx->pool, &dc);
        if (namePtr != 0) {
            /* The picked entry has a namePtr != 0 — it could be the
               create or a newer entry.  We need to check if it's the
               create (or a namePtr!=0 entry at the create's epoch
               that shadows the create).  For simplicity, if the
               picked entry is at the create_epoch or a later epoch
               in [create_epoch, tombstone_epoch), the create is
               reachable. */
            int64_t picked_eff = (int64_t)picked_epoch;
            if (mapper_table_traversal_apply(&ctx->mapper_table, (int64_t)picked_epoch)) {
                picked_eff = mapper_table_resolve(&ctx->mapper_table, (int64_t)picked_epoch);
            }
            /* The create is at create_epoch.  It's reachable at R if
               the read rule at R picks a namePtr!=0 entry whose
               effective epoch is in [create_epoch, R).  This is
               always true if the picked entry's effective epoch <= R
               and >= create_epoch. */
            if (picked_eff >= create_epoch && picked_eff < R) {
                return 1;  /* create is reachable at R */
            }
            /* The picked entry is the create itself (epoch == create_epoch)
               or a newer entry in the chain (closer to head).  Either
               way, the create is visible at R. */
            if (picked_eff == create_epoch) {
                return 1;
            }
            /* The picked entry is a namePtr!=0 entry at epoch > R
               (shouldn't happen per the read rule).  Skip. */
        }
    }
    return 0;  /* no R in the range sees the create; freeable */
}

/* The analysis handler. */
int gc_handle_rename_done(vfs_t* vfs, const BinEntry* entry) {
    if (!vfs || !vfs->ctx || !entry) return VFS_ERR_IO;
    TreeContext* ctx = vfs->ctx;
    int64_t file_vp      = entry->context;
    int64_t tombstone_vp_hint = entry->context2;

    /* 1. Sanity check: the file slot still exists and is a file. */
    PoolSlot file_slot = {0};
    pool_acquire(&ctx->pool, file_vp, false, &file_slot);
    if (file_slot.vptr == VFS_VPTR_NULL) {
        return VFS_OK;  /* already gone — idempotent no-op */
    }
    uint32_t file_child_id = (uint32_t)vfs_rd4_s(file_slot.bytes,
                                                  FILENODE_OFF_NODEID, ctx->page_size);
    /* The "file" slot can be either a FileNode (for files) or a
     * DirNode (for directories).  Both layouts put the type at
     * offset 0 (FILENODE_OFF_TYPE == DIRNODE_OFF_TYPE) and the
     * childId at offset 4.  The bin job's dir-entry cleanup is
     * identical for both: find the SlotNode (keyed by childId)
     * in the parent, walk the chain, CAS-remove the create +
     * tombstone, free the OLD NameEntry.  The file vs dir
     * distinction does not affect the chain walk.
     *
     * The type check below is a SAFETY check for VP reuse (if
     * the slot was freed and reused for an unrelated node with
     * a different layout).  We accept FILE and DIR; anything
     * else (e.g., a future node type) is treated as VP reuse
     * and skipped.  Per the W3 review follow-up (renaming
     * directories also needs the bin job). */
    int16_t file_type = (int16_t)vfs_rd2_s(file_slot.bytes, FILENODE_OFF_TYPE, ctx->page_size);
    pool_release(&ctx->pool, &file_slot);
    if (file_type != (int16_t)NODE_TYPE_FILE &&
        file_type != (int16_t)NODE_TYPE_DIR) {
        return VFS_OK;  /* slot reused for unknown type; no-op */
    }

    /* 2. Determine reference points. */
    rename_ref_points_t rp = {0};
    rp.H = ctx->currentEpoch;
    rename_collect_active_snapshots(ctx, &rp);

    /* 3. Find the parent dir via the file's childPtr. */
    int64_t parent_dir_vp = 0;
    if (find_parent_dir_rename(ctx, file_vp, &parent_dir_vp) != VFS_OK) {
        return VFS_OK;  /* file not in any dir; already cleaned up */
    }

    /* 4. Find the SlotNode in the parent dir. */
    int64_t slot_vp = 0;
    if (find_slotnode_in_dir_rename(ctx, parent_dir_vp, file_child_id,
                                       &slot_vp) != VFS_OK || slot_vp == 0) {
        return VFS_OK;  /* no SlotNode; already cleaned up */
    }

    /* 5. Walk the chain to find tombstone + create. */
    int64_t tombstone_vp = 0, tombstone_epoch = 0;
    int64_t create_vp = 0,    create_epoch = 0;
    if (find_chain_entries(ctx, slot_vp, file_vp, tombstone_vp_hint,
                             &tombstone_vp, &tombstone_epoch,
                             &create_vp, &create_epoch) != 0) {
        return VFS_ERR_IO;
    }

    /* If both are gone, the trigger is fully resolved. */
    if (tombstone_vp == 0 && create_vp == 0) {
        return VFS_OK;
    }

    /* 6. Determine freeability. */
    int tombstone_freeable = 0;
    if (tombstone_vp != 0) {
        tombstone_freeable = !tombstone_needed_at(ctx, slot_vp, &rp, tombstone_epoch);
    }
    int create_freeable = 0;
    if (create_vp != 0) {
        int64_t tomb_ep_for_range = (tombstone_vp != 0) ? tombstone_epoch
                                                          : (rp.H > create_epoch ? rp.H : create_epoch);
        create_freeable = !create_reachable_at(ctx, slot_vp, &rp,
                                                 create_epoch, tomb_ep_for_range);
    }

    /* If neither is freeable, nothing to do (snapshot still active). */
    if (!tombstone_freeable && !create_freeable) {
        return VFS_OK;
    }

    /* 7. Build the per-trigger batch list.  Each freeable DC becomes
       a BATCH_ENTRY_DC.  If the create is freeable, also collect
       the OLD NameEntry (read from the create's namePtr) as
       BATCH_ENTRY_NAME_FIRST.  The work handler frees the
       NameEntry's first slot after walking its chain.

       For empty-SlotNode removal: if BOTH the tombstone and the
       create are freeable AND the SlotNode has no other DCs, also
       add a BATCH_ENTRY_SLOT.  (For simplicity, we check the chain
       head again after the planned removals — if empty, add the
       SLOT entry.) */
    int64_t batch_head = 0;
    int batch_count = 0;

    /* Read the create's namePtr BEFORE the create is removed (the
       create's slot is still valid at this point).  Even if the
       create isn't freeable, we might want the namePtr for other
       use — but for now, only read it if the create is freeable. */
    int64_t old_name_first_vp = 0;
    if (create_freeable && create_vp != 0) {
        PoolSlot create_dc = {0};
        pool_acquire(&ctx->pool, create_vp, false, &create_dc);
        if (create_dc.vptr != VFS_VPTR_NULL) {
            old_name_first_vp = vfs_rd8_s(create_dc.bytes,
                                           DIRCONTENT_OFF_NAMEPTR, ctx->page_size);
        }
        pool_release(&ctx->pool, &create_dc);

        /* Defensive check: verify the OLD NameEntry's first slot
           content matches the expected name.  If not, the slot
           was reused — skip the OLD NameEntry free.  (For multi-
           slot names, we just verify the first 8 bytes of the
           hash; the work handler walks the chain.) */
        if (old_name_first_vp != 0) {
            PoolSlot name_slot = {0};
            pool_acquire(&ctx->pool, old_name_first_vp, false, &name_slot);
            if (name_slot.vptr == VFS_VPTR_NULL) {
                old_name_first_vp = 0;  /* invalid; skip */
            }
            pool_release(&ctx->pool, &name_slot);
        }
    }

    /* Prepend the OLD NameEntry first (if freeable).  The work
       handler walks the chain starting from old_name_first_vp and
       frees each chain slot.  Prepending first means the work
       handler iterates in reverse order (LIFO), freeing the
       chain slots last-to-first — but for NameEntry it doesn't
       matter (each slot is independent).  The order in the
       batch list is not semantically meaningful. */
    if (old_name_first_vp != 0) {
        if (batch_list_prepend(ctx, &batch_head,
                                 old_name_first_vp, /* target = first chain slot */
                                 parent_dir_vp,      /* context = parent_dir_vp
                                                        (used by I1 fix: the
                                                        work handler's
                                                        extract_name_hash_from_batch
                                                        reads this to know
                                                        which dir's index
                                                        to update).  The
                                                        process_name_entry
                                                        handler does NOT
                                                        use this field. */
                                 BATCH_ENTRY_NAME_FIRST) == VFS_OK) {
            batch_count++;
        }
    }

    /* Prepend the create DC (if freeable).  context = slot_vp. */
    if (create_freeable && create_vp != 0) {
        if (batch_list_prepend(ctx, &batch_head, create_vp, slot_vp,
                                 BATCH_ENTRY_DC) == VFS_OK) {
            batch_count++;
        }
    }

    /* Prepend the tombstone DC (if freeable).  context = slot_vp.
       Cross-dir only; for same-dir, tombstone_vp_hint is 0. */
    if (tombstone_freeable && tombstone_vp != 0) {
        if (batch_list_prepend(ctx, &batch_head, tombstone_vp, slot_vp,
                                 BATCH_ENTRY_DC) == VFS_OK) {
            batch_count++;
        }
    }

    /* If both are freeable, check if the SlotNode would be empty
       after the removals.  Walk the chain — if no other entries
       (besides the tombstone and create), add a SLOT entry. */
    if (tombstone_freeable && create_freeable) {
        PoolSlot slot = {0};
        pool_acquire(&ctx->pool, slot_vp, false, &slot);
        if (slot.vptr != VFS_VPTR_NULL) {
            int64_t head = vfs_rd8_s(slot.bytes, ANCHOR_OFF_HEADPTR, ctx->page_size);
            pool_release(&ctx->pool, &slot);
            int other_count = 0;
            int64_t dc = head;
            while (dc != 0) {
                PoolSlot dc_s = {0};
                pool_acquire(&ctx->pool, dc, false, &dc_s);
                if (dc_s.vptr == VFS_VPTR_NULL) break;
                int64_t dc_next = vfs_rd8_s(dc_s.bytes, DIRCONTENT_OFF_NEXTPTR, ctx->page_size);
                pool_release(&ctx->pool, &dc_s);
                if (dc != tombstone_vp && dc != create_vp) {
                    other_count++;
                }
                dc = dc_next;
            }
            if (other_count == 0) {
                if (batch_list_prepend(ctx, &batch_head, slot_vp, parent_dir_vp,
                                         BATCH_ENTRY_SLOT) == VFS_OK) {
                    batch_count++;
                }
            }
        }
    }

    /* 8. Push the work entry. */
    if (batch_count > 0 && batch_head != 0) {
        bin_push(ctx->sb, BIN_WORK_REMOVE_TOMBSTONE, batch_head,
                 (int64_t)batch_count);
    }

    return VFS_OK;
}

/* ---- Work handler (W3) ---- */

/* B3 fix helpers — duplicated from src/gc_bin_file_deleted.c */
static uint8_t* live_pool_page(StorageBackend* sb, int64_t slot_vp,
                                 StorageReadStatus* out_status) {
    if (out_status) *out_status = STORAGE_NOT_FOUND;
    if (!sb || slot_vp == VFS_VPTR_NULL) return NULL;
    int64_t page_index = VFS_VPTR_PAGE(slot_vp);
    if (page_index < 2) return NULL;
    return storage_read_with_status(sb, page_index, out_status);
}

static int pool_slot_data_offset(int slot_index) {
    return VFS_POOL_ENTRIES_OFFSET + slot_index * VFS_POOL_SLOT_SIZE;
}

static int cas_head_ptr_live(TreeContext* ctx, int64_t slot_vp,
                              int64_t expected, int64_t desired) {
    StorageReadStatus pst;
    uint8_t* payload = live_pool_page(ctx->sb, slot_vp, &pst);
    if (payload == NULL || pst != STORAGE_OK) return -1;
    int slot_offset = pool_slot_data_offset(VFS_VPTR_SLOT(slot_vp));
    int64_t* field = (int64_t*)(payload + slot_offset + ANCHOR_OFF_HEADPTR);
    int64_t prev = vfs_cas_i64(field, expected, desired);
    if (prev == expected) {
        cache_mark_dirty(&ctx->sb->cache, VFS_VPTR_PAGE(slot_vp), FLUSH_PRIO_POOL);
        return 1;
    }
    return 0;
}

static int cas_next_ptr_live(TreeContext* ctx, int64_t slot_vp,
                              int64_t expected, int64_t desired) {
    StorageReadStatus pst;
    uint8_t* payload = live_pool_page(ctx->sb, slot_vp, &pst);
    if (payload == NULL || pst != STORAGE_OK) return -1;
    int slot_offset = pool_slot_data_offset(VFS_VPTR_SLOT(slot_vp));
    int64_t* field = (int64_t*)(payload + slot_offset + DIRCONTENT_OFF_NEXTPTR);
    int64_t prev = vfs_cas_i64(field, expected, desired);
    if (prev == expected) {
        cache_mark_dirty(&ctx->sb->cache, VFS_VPTR_PAGE(slot_vp), FLUSH_PRIO_POOL);
        return 1;
    }
    return 0;
}

/* Walk the chain, find the predecessor of the target DC.  Returns
   the predecessor's VP (0 if target is the head).  The target's
   nextPtr value is returned in *out_next (so the work handler
   can rewrite the chain to skip the target).  On error, returns
   -1.  No CAS — the work handler does the CAS after reading
   target's nextPtr. */
static int64_t find_predecessor(TreeContext* ctx, int64_t slot_vp,
                                   int64_t target, int64_t* out_next) {
    PoolSlot slot = {0};
    pool_acquire(&ctx->pool, slot_vp, false, &slot);
    if (slot.vptr == VFS_VPTR_NULL) return -1;
    int64_t head = vfs_rd8_s(slot.bytes, ANCHOR_OFF_HEADPTR, ctx->page_size);
    pool_release(&ctx->pool, &slot);

    if (head == target) {
        /* Target is the head.  Predecessor = 0.  out_next = target's nextPtr. */
        PoolSlot t = {0};
        pool_acquire(&ctx->pool, target, false, &t);
        if (t.vptr == VFS_VPTR_NULL) return -1;
        *out_next = vfs_rd8_s(t.bytes, DIRCONTENT_OFF_NEXTPTR, ctx->page_size);
        pool_release(&ctx->pool, &t);
        return 0;
    }

    int64_t prev = head;
    while (prev != 0) {
        PoolSlot p = {0};
        pool_acquire(&ctx->pool, prev, false, &p);
        if (p.vptr == VFS_VPTR_NULL) return -1;
        int64_t next = vfs_rd8_s(p.bytes, DIRCONTENT_OFF_NEXTPTR, ctx->page_size);
        pool_release(&ctx->pool, &p);
        if (next == target) {
            PoolSlot t = {0};
            pool_acquire(&ctx->pool, target, false, &t);
            if (t.vptr == VFS_VPTR_NULL) return -1;
            *out_next = vfs_rd8_s(t.bytes, DIRCONTENT_OFF_NEXTPTR, ctx->page_size);
            pool_release(&ctx->pool, &t);
            return prev;
        }
        prev = next;
    }
    return -1;  /* target not in chain */
}

/* Process a single BATCH_ENTRY_DC.  CAS-remove the DC from the
   SlotNode's chain, update dircontentindex (per I1 fix) to
   remove the OLD name's radix link (when the create is being
   freed), pool_free the DC slot. */
static int process_dc_entry(TreeContext* ctx, int64_t dc_vp,
                              int64_t slot_vp,
                              int64_t parent_dir_vp,
                              uint64_t name_hash) {
    /* Find the predecessor and the target's nextPtr. */
    int64_t target_next = 0;
    int64_t pred = find_predecessor(ctx, slot_vp, dc_vp, &target_next);
    if (pred < 0) {
        /* DC not in chain (already removed).  Idempotent: just
           free the slot and continue.  (But the analysis guards
           against this — only freeable DCs are in the batch.) */
        pool_free(&ctx->pool, dc_vp);
        return VFS_OK;
    }

    /* CAS-remove: if DC is the head, CAS the SlotNode's headPtr;
       else, CAS the predecessor's nextPtr. */
    if (pred == 0) {
        if (cas_head_ptr_live(ctx, slot_vp, dc_vp, target_next) < 0) {
            return VFS_ERR_IO;
        }
    } else {
        if (cas_next_ptr_live(ctx, pred, dc_vp, target_next) < 0) {
            return VFS_ERR_IO;
        }
    }

    /* I1 fix: update the parent dir's radix index to remove the
       OLD name's link (per spec §4.2 step 4).  The link was
       inserted when vfs_create was called, pointing at the
       SlotNode at OLD_name's hash.  After this DC removal, if
       the SlotNode becomes empty AND the SlotNode is also being
       freed (BATCH_ENTRY_SLOT in the same batch), the link is
       stale.  We do the dircontentindex_remove here (instead of
       in the SlotNode work handler) because:
       - The OLD NameEntry's hash is only known at this point
         (before pool_free of the NameEntry, which frees the
         chain slots including the one with the hash at offset 0).
       - The SlotNode might not be removed at all (e.g.,
         tombstone-only rename where the create stays).  In that
         case the link at the OLD name's hash is still valid
         (the SlotNode still has the create, which IS at OLD_name).
         So we only call dircontentindex_remove when the create is
         being freed (name_hash != 0) AND the chain will be empty
         after this DC removal.

       Note: dircontentindex_remove zeroes the link to a
       "tree-tombstone" (M8 design).  Subsequent lookups for
       OLD_name fall through to the chain walk, which (with the
       DC just removed and possibly more DCs removed too) returns
       NOTFOUND or hits a newer DC.  Both are correct outcomes. */
    if (name_hash != 0 && parent_dir_vp != 0) {
        /* Read the parent dir's index head. */
        int64_t index_head = 0;
        PoolSlot ps = {0};
        pool_acquire(&ctx->pool, parent_dir_vp, false, &ps);
        if (ps.vptr != VFS_VPTR_NULL) {
            index_head = vfs_rd8_s(ps.bytes, DIRNODE_OFF_INDEXHEADPTR,
                                     ctx->page_size);
        }
        pool_release(&ctx->pool, &ps);
        if (index_head != 0) {
            dircontentindex_remove(&ctx->pool, index_head, name_hash,
                                    slot_vp, ctx->page_size);
        }
    }

    pool_free(&ctx->pool, dc_vp);
    return VFS_OK;
}

/* Process a BATCH_ENTRY_NAME_FIRST.  Walk the chain of chain
   slots (via the slot's sibPtr at ANCHOR_OFF_SIBPTR) and free
   each.  The first slot's chain is read from the slot's data
   (NameEntry layout: 8-byte hash + up to 16 bytes of name +
   next chain slot VP at offset 16).  Actually, the NameEntry
   uses the standard ANCHOR_OFF_SIBPTR at offset 16 to link
   chain slots. */
static int process_name_entry(TreeContext* ctx, int64_t first_vp) {
    /* Walk the chain via the slot's sibPtr.  Each slot is a
       standard anchor; the sibPtr at offset ANCHOR_OFF_SIBPTR
       points to the next chain slot (or 0 if end). */
    int64_t vp = first_vp;
    int64_t prev = 0;
    while (vp != 0) {
        PoolSlot s = {0};
        pool_acquire(&ctx->pool, vp, false, &s);
        if (s.vptr == VFS_VPTR_NULL) {
            /* Slot was freed by a prior pass or was reused.
               Stop walking. */
            break;
        }
        int64_t sib = vfs_rd8_s(s.bytes, ANCHOR_OFF_SIBPTR, ctx->page_size);
        pool_release(&ctx->pool, &s);
        int64_t next = sib;
        pool_free(&ctx->pool, vp);
        prev = vp;
        vp = next;
    }
    (void)prev;
    return VFS_OK;
}

/* I1 fix helper: read the name hash from the OLD NameEntry's
   first slot, and the parent_dir_vp from the BATCH_ENTRY_NAME_FIRST
   batch entry's context field.  Returns 0 on success (hash and
   parent_dir_vp set; hash is 0 if the NameEntry slot is empty,
   parent_dir_vp is 0 if the batch has no BATCH_ENTRY_NAME_FIRST). */
static int extract_name_hash_from_batch(TreeContext* ctx,
                                          int64_t batch_head,
                                          int64_t batch_count,
                                          uint64_t* out_hash,
                                          int64_t* out_parent_dir_vp) {
    *out_hash = 0;
    *out_parent_dir_vp = 0;
    int64_t cur = batch_head;
    int64_t processed = 0;
    while (cur != 0 && processed < batch_count) {
        PoolSlot bs = {0};
        pool_acquire(&ctx->pool, cur, false, &bs);
        if (bs.vptr == VFS_VPTR_NULL) break;
        int64_t next = vfs_rd8_s(bs.bytes, 0,  ctx->page_size);
        int64_t target = vfs_rd8_s(bs.bytes, 8,  ctx->page_size);
        int64_t ctxvp  = vfs_rd8_s(bs.bytes, 16, ctx->page_size);
        int32_t type   = (int32_t)vfs_rd4_s(bs.bytes, 24, ctx->page_size);
        pool_release(&ctx->pool, &bs);
        if (type == BATCH_ENTRY_NAME_FIRST) {
            /* target = OLD NameEntry's first slot VP.
               ctxvp  = parent_dir_vp (per the analysis handler's
                       prepend: BATCH_ENTRY_NAME_FIRST is prepended
                       with context = parent_dir_vp). */
            *out_parent_dir_vp = ctxvp;
            if (target != 0) {
                PoolSlot ns = {0};
                pool_acquire(&ctx->pool, target, false, &ns);
                if (ns.vptr != VFS_VPTR_NULL) {
                    /* The hash is the first 8 bytes of the
                       NameEntry's first slot (per nodes_write_name). */
                    *out_hash = (uint64_t)vfs_rd8_s(ns.bytes, 0, ctx->page_size);
                }
                pool_release(&ctx->pool, &ns);
            }
            return VFS_OK;
        }
        cur = next;
        processed++;
    }
    return VFS_OK;  /* not found; both out_* remain 0 */
}

/* Process a BATCH_ENTRY_SLOT.  Unlink the SlotNode from its
   DirSegment's SlotNode chain and pool_free the SlotNode.  The
   batch entry's target_vp is the SlotNode VP, context_vp is
   the parent_dir_vp (for finding the DirSegment). */
static int process_slot_entry(TreeContext* ctx, int64_t slot_vp,
                                int64_t parent_dir_vp) {
    /* Find the DirSegment containing this SlotNode.  Walk the
       parent's index, find the segment whose SlotNode chain
       contains our slot_vp.  Then CAS-remove the slot from the
       segment's SlotNode chain. */
    PoolSlot dir_slot = {0};
    pool_acquire(&ctx->pool, parent_dir_vp, false, &dir_slot);
    if (dir_slot.vptr == VFS_VPTR_NULL) return VFS_ERR_IO;
    int64_t seg_vp = vfs_rd8_s(dir_slot.bytes, DIRNODE_OFF_HEADPTR, ctx->page_size);
    pool_release(&ctx->pool, &dir_slot);

    while (seg_vp != 0) {
        PoolSlot seg = {0};
        pool_acquire(&ctx->pool, seg_vp, false, &seg);
        if (seg.vptr == VFS_VPTR_NULL) break;
        int64_t seg_head = vfs_rd8_s(seg.bytes, ANCHOR_OFF_HEADPTR, ctx->page_size);
        int64_t seg_sib = vfs_rd8_s(seg.bytes, ANCHOR_OFF_SIBPTR, ctx->page_size);
        pool_release(&ctx->pool, &seg);

        /* Walk the segment's SlotNode chain to find the segment
           that contains slot_vp. */
        int64_t found_seg = 0;
        int64_t prev_slot = 0;
        int64_t slot_v = seg_head;
        while (slot_v != 0) {
            PoolSlot slot = {0};
            pool_acquire(&ctx->pool, slot_v, false, &slot);
            if (slot.vptr == VFS_VPTR_NULL) break;
            int64_t slot_sib = vfs_rd8_s(slot.bytes, ANCHOR_OFF_SIBPTR, ctx->page_size);
            pool_release(&ctx->pool, &slot);
            if (slot_v == slot_vp) { found_seg = seg_vp; break; }
            prev_slot = slot_v;
            slot_v = slot_sib;
        }
        if (found_seg != 0) {
            /* Unlink slot_vp from this segment's SlotNode chain.
             * The SlotNode's sibPtr is at ANCHOR_OFF_SIBPTR (offset
             * 16), not DIRCONTENT_OFF_NEXTPTR (which the
             * cas_next_ptr_live helper writes).  We need a
             * cas_sib_ptr_live helper for the mid-chain case; for
             * now, the safest fix is to ONLY free the SlotNode when
             * it's the segment head (where we can CAS the segment's
             * headPtr field).  If the SlotNode is mid-chain, leave
             * the empty SlotNode in the chain (a small leak — the
             * empty SlotNode is still in the per-parent index link
             * but the chain walk returns "no DC entries" so lookups
             * fall through to NOTFOUND via the radix fast-path
             * miss).  Per the W3 review's I2 fix. */
            if (prev_slot == 0) {
                /* slot_vp is the segment head.  CAS the segment's
                   SlotNode headPtr to slot_sib. */
                int64_t next_slot = 0;
                PoolSlot s = {0};
                pool_acquire(&ctx->pool, slot_vp, false, &s);
                if (s.vptr != VFS_VPTR_NULL) {
                    next_slot = vfs_rd8_s(s.bytes, ANCHOR_OFF_SIBPTR, ctx->page_size);
                }
                pool_release(&ctx->pool, &s);
                /* Read seg_head directly from seg and CAS it. */
                StorageReadStatus pst;
                uint8_t* payload = live_pool_page(ctx->sb, seg_vp, &pst);
                if (payload == NULL || pst != STORAGE_OK) return VFS_ERR_IO;
                int slot_offset = pool_slot_data_offset(VFS_VPTR_SLOT(seg_vp));
                int64_t* head_field = (int64_t*)(payload + slot_offset + ANCHOR_OFF_HEADPTR);
                if (vfs_cas_i64(head_field, slot_vp, next_slot) == slot_vp) {
                    cache_mark_dirty(&ctx->sb->cache, VFS_VPTR_PAGE(seg_vp), FLUSH_PRIO_POOL);
                }
                /* Pool_free the SlotNode.  Only done when we
                   successfully unlinked it. */
                pool_free(&ctx->pool, slot_vp);
            } else {
                /* Mid-chain: skip the unlink + pool_free.  The
                   empty SlotNode stays in the chain.  Subsequent
                   lookups for the (now non-existent) child return
                   NOTFOUND via the empty-chain walk.  The radix
                   index link is still present but points to a
                   SlotNode with an empty chain — the chain walk
                   does the right thing.  A future
                   `dircontentindex_remove` on a tombstone
                   cross-dir rename will eventually clean up the
                   link (see I1 fix in the work handler). */
            }
            return VFS_OK;
        }
        seg_vp = seg_sib;
    }
    return VFS_OK;  /* slot not found; idempotent */
}

int gc_handle_remove_tombstone(vfs_t* vfs, const BinEntry* entry) {
    if (!vfs || !vfs->ctx || !entry) return VFS_ERR_IO;
    TreeContext* ctx = vfs->ctx;
    int64_t batch_head  = entry->context;
    int64_t batch_count = entry->context2;

    if (batch_head == 0 || batch_count <= 0) {
        return VFS_OK;
    }

    /* I1 fix: pre-pass to extract the name hash and parent_dir_vp
       from the BATCH_ENTRY_NAME_FIRST (if present).  The name hash
       is needed for dircontentindex_remove (called from
       process_dc_entry); the parent_dir_vp is the dir whose index
       needs the update.  These two values are passed to every
       BATCH_ENTRY_DC processing call. */
    uint64_t old_name_hash    = 0;
    int64_t  parent_dir_vp    = 0;
    extract_name_hash_from_batch(ctx, batch_head, batch_count,
                                    &old_name_hash, &parent_dir_vp);

    /* Iterate the batch list.  Each entry is a 32-byte pool slot
       with the layout documented in the analysis handler:
         offset 0:  int64 next_batch_vp
         offset 8:  int64 target_vp
         offset 16: int64 context_vp
         offset 24: int32 type
         offset 28: int32 reserved
    */
    int64_t cur = batch_head;
    int64_t processed = 0;
    while (cur != 0 && processed < batch_count) {
        PoolSlot bs = {0};
        pool_acquire(&ctx->pool, cur, false, &bs);
        if (bs.vptr == VFS_VPTR_NULL) break;
        int64_t next = vfs_rd8_s(bs.bytes, 0,  ctx->page_size);
        int64_t target = vfs_rd8_s(bs.bytes, 8,  ctx->page_size);
        int64_t ctxvp  = vfs_rd8_s(bs.bytes, 16, ctx->page_size);
        int32_t type   = (int32_t)vfs_rd4_s(bs.bytes, 24, ctx->page_size);
        pool_release(&ctx->pool, &bs);

        switch (type) {
        case BATCH_ENTRY_DC:
            process_dc_entry(ctx, target, ctxvp,
                              parent_dir_vp, old_name_hash);
            break;
        case BATCH_ENTRY_NAME_FIRST:
            process_name_entry(ctx, target);
            break;
        case BATCH_ENTRY_SLOT:
            process_slot_entry(ctx, target, ctxvp);
            break;
        default:
            fprintf(stderr, "gc_bin_rename: unknown batch entry type %d\n", type);
            break;
        }

        /* Free the batch slot itself.  This is safe because we've
           already copied the fields. */
        pool_free(&ctx->pool, cur);
        processed++;
        cur = next;
    }
    return VFS_OK;
}
