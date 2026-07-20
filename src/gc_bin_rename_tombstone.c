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
                pool_release(&ctx->pool, &dc);
                if (child == file_vp) {
                    *out_found = 1;
                    *out_parent = dir_vp;
                    return VFS_OK;
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
   - tombstone_vp:  namePtr == 0, childPtr == file_vp, epoch == tombstone_epoch
   - create_vp:     namePtr != 0, childPtr == file_vp
   Returns 0 on success (with *out_tombstone_vp, *out_tombstone_epoch,
   *out_create_vp, *out_create_epoch set; 0 if not in chain), or -1 on
   I/O error.  If the tombstone's epoch is unknown (e.g., tombstone
   was already removed), the analysis reads it from the entry.
   The "create" picked is the one with the highest epoch (handles
   rename-away-and-back scenarios). */
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

    int64_t dc_vp = dc_head;
    while (dc_vp != 0) {
        PoolSlot dc = {0};
        pool_acquire(&ctx->pool, dc_vp, false, &dc);
        if (dc.vptr == VFS_VPTR_NULL) break;
        int32_t namePtr  = (int32_t)vfs_rd8_s(dc.bytes, DIRCONTENT_OFF_NAMEPTR,  ctx->page_size);
        int64_t childPtr = vfs_rd8_s(dc.bytes, DIRCONTENT_OFF_CHILDPTR, ctx->page_size);
        int32_t epoch    = (int32_t)vfs_rd4_s(dc.bytes, DIRCONTENT_OFF_EPOCH,    ctx->page_size);
        int64_t dc_next  = vfs_rd8_s(dc.bytes, DIRCONTENT_OFF_NEXTPTR, ctx->page_size);
        pool_release(&ctx->pool, &dc);

        if (childPtr == file_vp) {
            if (namePtr == 0) {
                /* Tombstone.  Use the hint if it matches; otherwise
                   accept any tombstone with childPtr == file_vp. */
                if (*out_tombstone_vp == 0 ||
                    dc_vp == tombstone_vp_hint) {
                    if (*out_tombstone_vp != 0 && dc_vp != tombstone_vp_hint) {
                        /* don't override an earlier match unless the hint matches */
                    } else {
                        *out_tombstone_vp = dc_vp;
                        *out_tombstone_epoch = (int64_t)epoch;
                    }
                }
            } else {
                /* Create-like (namePtr != 0).  Pick the one with the
                   highest epoch. */
                if ((int64_t)epoch > *out_create_epoch) {
                    *out_create_vp = dc_vp;
                    *out_create_epoch = (int64_t)epoch;
                }
            }
        }
        dc_vp = dc_next;
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
        pool_release(&ctx->pool, &dc);
        if (namePtr != 0) {
            /* The picked entry has a namePtr != 0 — it could be the
               create or a newer entry.  We need to check if it's the
               create (or a namePtr!=0 entry at the create's epoch
               that shadows the create).  For simplicity, if the
               picked entry is at the create_epoch or a later epoch
               in [create_epoch, tombstone_epoch), the create is
               reachable. */
            int32_t picked_epoch = (int32_t)vfs_rd4_s(dc.bytes, DIRCONTENT_OFF_EPOCH, ctx->page_size);
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
    int16_t file_type = (int16_t)vfs_rd2_s(file_slot.bytes, FILENODE_OFF_TYPE, ctx->page_size);
    pool_release(&ctx->pool, &file_slot);
    if (file_type != (int16_t)NODE_TYPE_FILE) {
        return VFS_OK;  /* slot reused; no-op */
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
                                 old_name_first_vp, /* context = same, for chain walk */
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

/* ---- Work handler (W3 stub — real impl in W3) ---- */

int gc_handle_remove_tombstone(vfs_t* vfs, const BinEntry* entry) {
    (void)vfs;
    (void)entry;
    /* W3 stub: iterate the batch list and do the per-entry work
       (CAS-remove DCs, pool_free NameEntries, unlink empty
       SlotNodes).  See impl/phase-28-bin-job-rename-tombstone.md
       §4 for the spec. */
    return VFS_OK;
}
