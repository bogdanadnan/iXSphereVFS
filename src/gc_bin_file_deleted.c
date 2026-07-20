/* Phase 28 Type 1: file-deletion bin job (analysis handler).
 *
 * Spec: impl/phase-28-bin-job-file-deletion.md
 *
 * Trigger: BIN_TRIGGER_FILE_DELETED
 *   context  = file VP (the deleted FileNode)
 *   context2 = tombstone VP (the DirContent with namePtr=0 that
 *              vfs_delete just prepended to the SlotNode's chain)
 *
 * Algorithm:
 *   1. Determine reference points (H + each active snapshot).
 *   2. Walk the file's chain (FileContent -> PageNode -> VersionPage).
 *      For each VersionPage, check if it's visible at any reference
 *      point per the read rule (§7.2).  If not, mark the data page
 *      as freeable.
 *   3. Walk the parent dir's chain to find the SlotNode containing
 *      the tombstone.  Apply the read rule at each reference point
 *      to determine if the file is "visible" (a live entry with
 *      namePtr != 0 picked at any R).  Capture the create entry's
 *      VP if found.
 *   4. Batch the dead data pages into a pool-allocated linked
 *      list.  Push BIN_WORK_FREE_PAGES with the list head.
 *   5. If the file is not referenced by any reference point,
 *      drop the create + tombstone from the SlotNode's chain via
 *      CAS (inline; no separate work entry).
 *
 * The parent dir is found by walking from the root, looking for a
 * DirContent with childPtr == file_vp.  O(D) where D is the
 * directory tree size.  Acceptable for pre-MVP (typical D < 100).
 */
#include "gc.h"
#include "tree.h"
#include "nodes.h"
#include "mapper.h"
#include "bin.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Max number of active snapshots we consider in the analysis.
 * For systems with more than this, only the latest MAX_ACTIVE_SNAPSHOTS
 * are considered (earlier ones are shadowed by newer ones in the
 * read rule).  See spec §3.1. */
#define MAX_ACTIVE_SNAPSHOTS 64

/* Reference points buffer (passed around as a struct). */
typedef struct {
    int64_t H;                          /* current head epoch */
    int64_t active[MAX_ACTIVE_SNAPSHOTS];
    int     num_active;
} ref_points_t;

/* Forward decls (in this file). */
static int collect_active_snapshots(TreeContext* ctx, ref_points_t* rp);
static int classify_data_pages(TreeContext* ctx, int64_t file_vp,
                                const ref_points_t* rp,
                                int64_t* out_dead_logicals, int max_dead,
                                int* out_num_dead);
static int find_parent_dir(TreeContext* ctx, int64_t file_vp,
                            int64_t* out_parent_dir_vp);
static int find_slotnode_in_dir(TreeContext* ctx, int64_t dir_vp,
                                 uint32_t child_id, int64_t* out_slot_vp);
static int read_rule_pick_first_dc(TreeContext* ctx, int64_t slot_vp,
                                    int64_t read_epoch, int64_t* out_dc_vp);
static int check_file_referenced(TreeContext* ctx, int64_t file_vp,
                                  int64_t slot_vp, const ref_points_t* rp,
                                  int* out_referenced,
                                  int64_t* out_create_vp);
static int64_t find_create_in_slot(TreeContext* ctx, int64_t slot_vp,
                                     int64_t file_vp);
static int drop_dir_entries(TreeContext* ctx, int64_t slot_vp,
                             int64_t create_vp, int64_t tombstone_vp);
static int batch_dead_pages_into_pool_list(TreeContext* ctx,
                                            const int64_t* dead_logicals,
                                            int num_dead,
                                            int64_t* out_batch_head);
static int64_t alloc_pool_slot_32(TreeContext* ctx, uint8_t* out_bytes,
                                   int64_t* out_vp);
static int64_t read_mirror_page(StorageBackend* sb, int64_t logical);
static int free_logical_with_mirror(StorageBackend* sb, int64_t logical);

/* ---------------------------------------------------------------------------
 * gc_handle_file_deleted — main analysis entry point (called by GC thread)
 * --------------------------------------------------------------------------- */
int gc_handle_file_deleted(vfs_t* vfs, const BinEntry* entry) {
    if (!vfs || !vfs->ctx || !entry) return VFS_ERR_IO;
    TreeContext* ctx = vfs->ctx;
    int64_t file_vp     = entry->context;
    int64_t tombstone_vp = entry->context2;

    /* 1. Sanity check: the file slot still exists.  If not, a prior
          run already processed this trigger (idempotency). */
    PoolSlot file_slot = {0};
    pool_acquire(&ctx->pool, file_vp, false, &file_slot);
    if (file_slot.vptr == VFS_VPTR_NULL) {
        return VFS_OK;
    }
    pool_release(&ctx->pool, &file_slot);

    /* 2. Determine reference points. */
    ref_points_t rp = {0};
    rp.H = ctx->currentEpoch;
    if (collect_active_snapshots(ctx, &rp) != VFS_OK) {
        return VFS_ERR_IO;
    }

    /* 3. Classify data pages as live/dead. */
    int64_t dead_logicals[1024];
    int num_dead = 0;
    int err = classify_data_pages(ctx, file_vp, &rp,
                                   dead_logicals,
                                   (int)(sizeof(dead_logicals) /
                                         sizeof(dead_logicals[0])),
                                   &num_dead);
    if (err != VFS_OK) return err;

    /* 4. Find the parent dir (walk from root). */
    int64_t parent_dir_vp = 0;
    if (find_parent_dir(ctx, file_vp, &parent_dir_vp) != VFS_OK) {
        /* Parent not found — the file is not in any dir (e.g., the
           tombstone was the only entry and we already removed it
           on a prior run).  Skip the dir-entry drop.  Still push
           the BIN_WORK_FREE_PAGES for the dead data pages. */
        parent_dir_vp = 0;
    }

    /* 5. Find the SlotNode for the file in the parent dir. */
    int64_t slot_vp = 0;
    if (parent_dir_vp != 0) {
        PoolSlot fs = {0};
        pool_acquire(&ctx->pool, file_vp, false, &fs);
        uint32_t file_child_id = (uint32_t)vfs_rd4_s(fs.bytes,
                                                     FILENODE_OFF_NODEID,
                                                     ctx->page_size);
        pool_release(&ctx->pool, &fs);
        if (find_slotnode_in_dir(ctx, parent_dir_vp, file_child_id, &slot_vp) != VFS_OK) {
            slot_vp = 0;
        }
    }

    /* 6. Determine file visibility at each reference point.  Also
          find the create entry's VP by walking the SlotNode's chain
          directly (the create is the entry with namePtr != 0 and
          childPtr == file_vp — even if it's shadowed by a tombstone).
          The drop logic needs both the create and the tombstone VPs. */
    int    file_referenced = 0;
    int64_t create_vp = 0;
    if (slot_vp != 0) {
        if (check_file_referenced(ctx, file_vp, slot_vp, &rp,
                                   &file_referenced, &create_vp) != VFS_OK) {
            file_referenced = 1;  /* conservative: don't drop on error */
        }
        /* If check_file_referenced didn't find a live entry (the
           create is shadowed by the tombstone), walk the chain to
           find the create by its namePtr.  The create is the entry
           with namePtr != 0 and childPtr == file_vp. */
        if (create_vp == 0) {
            create_vp = find_create_in_slot(ctx, slot_vp, file_vp);
        }
    }

    /* 7. If there are dead data pages, batch into pool list and push
          BIN_WORK_FREE_PAGES. */
    if (num_dead > 0) {
        int64_t batch_head = 0;
        if (batch_dead_pages_into_pool_list(ctx, dead_logicals, num_dead,
                                              &batch_head) == VFS_OK && batch_head != 0) {
            bin_push(ctx->sb, BIN_WORK_FREE_PAGES, batch_head, (int64_t)num_dead);
        }
        /* If batching failed, the data pages are still orphaned (the
           file's chain still references them, but no reference point
           reaches them).  They'll be reclaimed by the next
           shadow-compaction (out-of-band GC pass). */
    }

    /* 8. If the file is not referenced by any reference point,
          perform the inline dir-entry drop.  No separate work entry.
          Also update the parent dir's dircontentindex to remove the
          stale link pointing at the SlotNode (per spec §3.8 / M3 review
          option (a)).  The name hash is read from the create entry's
          namePtr BEFORE the drop (the create slot is still in the
          pool after the drop, but the parent-child relationship is
          most clearly read pre-drop). */
    if (!file_referenced && slot_vp != 0) {
        /* Read the create entry's namePtr to get the cached hash.
           nodes_read_name_hash reads the FNV-1a hash from offset 0
           of the first NameEntry slot (which is what's stored in the
           index lookup).  O(1) read. */
        int64_t  create_name_ptr = 0;
        uint64_t name_hash        = 0;
        if (create_vp != 0) {
            PoolSlot cs = {0};
            pool_acquire(&ctx->pool, create_vp, false, &cs);
            if (cs.vptr != VFS_VPTR_NULL) {
                create_name_ptr = vfs_rd8_s(cs.bytes,
                                              DIRCONTENT_OFF_NAMEPTR,
                                              ctx->page_size);
            }
            pool_release(&ctx->pool, &cs);
            if (create_name_ptr != 0) {
                name_hash = nodes_read_name_hash(&ctx->pool, create_name_ptr);
            }
        }

        int drc = drop_dir_entries(ctx, slot_vp, create_vp, tombstone_vp);

        /* Free the create + tombstone slots ONLY if the drop actually
           removed entries.  drop_dir_entries returns 1 (no-op) if the
           entries were already gone (idempotent on re-run); calling
           pool_free in that case would double-free and corrupt the
           pool's free list.  Returns 0 if at least one entry was
           removed; -1 on I/O error. */
        if (drc == 0) {
            /* Tombstone was definitely in the chain (and is now gone)
               because drc == 0 means tom_pred was found pre-drop, OR
               the headPtr was the tombstone.  Free its slot. */
            if (tombstone_vp != 0) {
                (void)pool_free(&ctx->pool, tombstone_vp);
            }
            /* Create was in the chain iff create_vp was passed in
               AND it was a separate entry from the tombstone.  The
               drop removed it iff cre_remove_ok was set inside
               drop_dir_entries.  We don't have that signal here, so
               we use the simpler "create_vp != 0 && != tombstone_vp"
               check.  If the create was the same as the tombstone
               (single-entry case), we already freed it above.  If
               the create was a separate entry, it's now gone. */
            if (create_vp != 0 && create_vp != tombstone_vp) {
                (void)pool_free(&ctx->pool, create_vp);
            }
        }

        /* After the drop, the SlotNode has no live entry for this
           child.  Remove the radix-tree link so future lookups in
           this dir take the fast path.  Best-effort: a partial or
           failed remove leaves the link as a "tree-tombstone"
           (dcVP=0) per the M8 design — lookups skip zeroed links
           and fall through to the chain walk (which is empty, so
           the lookup correctly returns NOTFOUND). */
        if (drc >= 0 && parent_dir_vp != 0 && name_hash != 0) {
            PoolSlot ps = {0};
            pool_acquire(&ctx->pool, parent_dir_vp, false, &ps);
            int64_t index_head = 0;
            if (ps.vptr != VFS_VPTR_NULL) {
                index_head = vfs_rd8_s(ps.bytes,
                                        DIRNODE_OFF_INDEXHEADPTR,
                                        ctx->page_size);
            }
            pool_release(&ctx->pool, &ps);
            if (index_head != 0) {
                dircontentindex_remove(&ctx->pool, index_head, name_hash,
                                        slot_vp, ctx->page_size);
            }
        }
    }

    return VFS_OK;
}

/* ---------------------------------------------------------------------------
 * collect_active_snapshots — fills rp->active[] with the odd S in
 * (0, H) that have no mapper entry (i.e., not committed, not soft-deleted).
 *
 * Walks the mapper table (ctx->mapper_table).  Each entry is a
 * resolved snapshot (committed or soft-deleted).  Any odd S in
 * [1, H-1] NOT in the table is active.
 *
 * Per the spec §3.1, the recommended implementation caches the active
 * set in ctx (updated on vfs_snapshot, vfs_commit, vfs_delete_snapshot).
 * For now, we iterate the mapper table + odd S.  Acceptable for
 * pre-MVP.
 * --------------------------------------------------------------------------- */
static int collect_active_snapshots(TreeContext* ctx, ref_points_t* rp) {
    if (!ctx || !rp) return VFS_ERR_IO;
    rp->num_active = 0;
    int64_t H = rp->H;
    if (H <= 0) return VFS_OK;

    /* The mapper table is keyed on fromEpoch.  An entry exists iff
       the snapshot is resolved (committed or soft-deleted).  So
       an odd S is active iff mapper_table_resolve(S) == S (no
       mapping) AND mapper_table_traversal_apply(S) == false
       (no traversal flag).  Equivalently: S is not a key. */
    for (int64_t S = 1; S < H && rp->num_active < MAX_ACTIVE_SNAPSHOTS; S += 2) {
        int64_t resolved = mapper_table_resolve(&ctx->mapper_table, S);
        int trav = mapper_table_traversal_apply(&ctx->mapper_table, S) ? 1 : 0;
        /* Active iff no mapping.  mapper_table_resolve returns S
           itself if not found; if found, returns toEpoch.  The
           traversal flag is meaningless for an unmapped snapshot. */
        if (resolved == S && !trav) {
            rp->active[rp->num_active++] = S;
        }
    }
    return VFS_OK;
}

/* ---------------------------------------------------------------------------
 * classify_data_pages — walk the file's chain (FileContent -> PageNode
 * -> VersionPage), classify each VersionPage's data page as live/dead.
 *
 * For each VersionPage E_VP at epoch E_VP_epoch referencing data page
 * data_page:
 *   live = 0
 *   for each R in {H, active[]}:
 *     if file is visible at R (parent dir's chain has a live entry
 *       pointing to file_vp at R per the read rule) AND
 *        E_VP is visible at R per the read rule:
 *       live = 1; break
 *   if !live: add data_page to dead_logicals
 *
 * Note: the file-visible-at-R check is redundant per file (not per
 * VersionPage); we cache it.  See ref_points_t's behavior.  For
 * simplicity, we don't cache here; the per-VersionPage cost is
 * small (the SlotNode chain walk is short, typically 1-2 entries).
 * --------------------------------------------------------------------------- */
static int classify_data_pages(TreeContext* ctx, int64_t file_vp,
                                const ref_points_t* rp,
                                int64_t* out_dead, int max_dead,
                                int* out_num_dead) {
    if (!ctx || !file_vp || !rp || !out_dead || !out_num_dead) return VFS_ERR_IO;
    *out_num_dead = 0;

    /* Read the FileNode to get the head FileContent VP. */
    PoolSlot file_slot = {0};
    pool_acquire(&ctx->pool, file_vp, false, &file_slot);
    if (file_slot.vptr == VFS_VPTR_NULL) return VFS_ERR_NOTFOUND;
    int64_t fc_head = vfs_rd8_s(file_slot.bytes, FILENODE_OFF_HEADPTR, ctx->page_size);
    pool_release(&ctx->pool, &file_slot);

    /* Walk the FileContent chain. */
    int64_t fc_vp = fc_head;
    while (fc_vp != 0) {
        PoolSlot fc_slot = {0};
        pool_acquire(&ctx->pool, fc_vp, false, &fc_slot);
        if (fc_slot.vptr == VFS_VPTR_NULL) break;
        int64_t pn_head = vfs_rd8_s(fc_slot.bytes, FILECONTENT_OFF_ROOTPTR, ctx->page_size);
        int64_t fc_next = vfs_rd8_s(fc_slot.bytes, FILECONTENT_OFF_NEXTPTR, ctx->page_size);
        pool_release(&ctx->pool, &fc_slot);

        /* Walk the PageNode chain within this segment. */
        int64_t pn_vp = pn_head;
        while (pn_vp != 0) {
            PoolSlot pn_slot = {0};
            pool_acquire(&ctx->pool, pn_vp, false, &pn_slot);
            if (pn_slot.vptr == VFS_VPTR_NULL) break;
            int64_t vr_head = vfs_rd8_s(pn_slot.bytes, PAGENODE_OFF_VERSIONROOT, ctx->page_size);
            int64_t pn_next = vfs_rd8_s(pn_slot.bytes, PAGENODE_OFF_NEXTPTR, ctx->page_size);
            pool_release(&ctx->pool, &pn_slot);

            /* Walk the VersionPage chain.  For each VP, check
               visibility per the read rule at each reference point. */
            int64_t vp_vp = vr_head;
            while (vp_vp != 0) {
                PoolSlot vp_slot = {0};
                pool_acquire(&ctx->pool, vp_vp, false, &vp_slot);
                if (vp_slot.vptr == VFS_VPTR_NULL) break;
                uint32_t vp_epoch = (uint32_t)vfs_rd4_s(vp_slot.bytes,
                                                       VERSIONPAGE_OFF_EPOCH,
                                                       ctx->page_size);
                int64_t data_page = (int64_t)vfs_rd8_s(vp_slot.bytes,
                                                         VERSIONPAGE_OFF_DATAPAGE,
                                                         ctx->page_size);
                int64_t vp_next = vfs_rd8_s(vp_slot.bytes,
                                             VERSIONPAGE_OFF_NEXTPTR,
                                             ctx->page_size);
                pool_release(&ctx->pool, &vp_slot);

                /* Check visibility at each reference point. */
                int live = 0;
                for (int ri = 0; ri <= rp->num_active; ri++) {
                    int64_t R = (ri == 0) ? rp->H : rp->active[ri - 1];
                    /* Apply per-entry read rule: if traversalApply
                       is true for vp_epoch, replace vp_epoch with
                       mapper_table_resolve(vp_epoch). */
                    int64_t eff_epoch = (int64_t)vp_epoch;
                    if (mapper_table_traversal_apply(&ctx->mapper_table,
                                                      (int64_t)vp_epoch)) {
                        eff_epoch = mapper_table_resolve(&ctx->mapper_table,
                                                           (int64_t)vp_epoch);
                    }
                    /* Read rule: exact match -> use it.  Even <
                       R -> use it.  Odd < R -> skip.  Future -> skip. */
                    if (eff_epoch == R) {
                        live = 1; break;
                    }
                    if (eff_epoch < R && (eff_epoch & 1) == 0) {
                        live = 1; break;
                    }
                    /* Otherwise (odd < R or future): skip. */
                }
                if (!live && data_page > 0 && *out_num_dead < max_dead) {
                    out_dead[(*out_num_dead)++] = data_page;
                }
                vp_vp = vp_next;
            }
            pn_vp = pn_next;
        }
        fc_vp = fc_next;
    }
    return VFS_OK;
}

/* ---------------------------------------------------------------------------
 * find_parent_dir — walk the directory tree from root, find the dir
 * whose chain contains a DirContent with childPtr == file_vp.
 *
 * Recursive depth-first walk.  O(D) where D is the directory tree
 * size.  Acceptable for pre-MVP.
 *
 * Note: this is a read-only walk (by-value pool slots, pinPage=false).
 * No lock needed.
 * --------------------------------------------------------------------------- */
static int walk_dir_recursive(TreeContext* ctx, int64_t dir_vp, int64_t file_vp,
                                int* out_found, int64_t* out_parent);

static int find_parent_dir(TreeContext* ctx, int64_t file_vp,
                            int64_t* out_parent_dir_vp) {
    if (!ctx || !file_vp || !out_parent_dir_vp) return VFS_ERR_IO;
    if (ctx->rootNodeOffset == 0) return VFS_ERR_NOTFOUND;
    int found = 0;
    int err = walk_dir_recursive(ctx, ctx->rootNodeOffset, file_vp,
                                  &found, out_parent_dir_vp);
    if (err != VFS_OK) return err;
    return found ? VFS_OK : VFS_ERR_NOTFOUND;
}

static int walk_dir_recursive(TreeContext* ctx, int64_t dir_vp, int64_t file_vp,
                                int* out_found, int64_t* out_parent) {
    if (*out_found) return VFS_OK;  /* already found, prune */

    /* Walk the dir's DirSegment chain.  For each Segment, walk its
       SlotNode chain.  For each SlotNode, walk its DirContent chain. */
    PoolSlot dir_slot = {0};
    pool_acquire(&ctx->pool, dir_vp, false, &dir_slot);
    if (dir_slot.vptr == VFS_VPTR_NULL) return VFS_ERR_IO;
    int64_t seg_vp = vfs_rd8_s(dir_slot.bytes, DIRNODE_OFF_HEADPTR, ctx->page_size);
    pool_release(&ctx->pool, &dir_slot);

    while (seg_vp != 0 && !*out_found) {
        PoolSlot seg = {0};
        pool_acquire(&ctx->pool, seg_vp, false, &seg);
        if (seg.vptr == VFS_VPTR_NULL) break;
        int64_t seg_head = vfs_rd8_s(seg.bytes, ANCHOR_OFF_HEADPTR, ctx->page_size);
        int64_t seg_sib = vfs_rd8_s(seg.bytes, ANCHOR_OFF_SIBPTR, ctx->page_size);
        pool_release(&ctx->pool, &seg);

        /* Walk the SlotNode chain. */
        int64_t slot_vp = seg_head;
        while (slot_vp != 0 && !*out_found) {
            PoolSlot slot = {0};
            pool_acquire(&ctx->pool, slot_vp, false, &slot);
            if (slot.vptr == VFS_VPTR_NULL) break;
            int64_t slot_head = vfs_rd8_s(slot.bytes, ANCHOR_OFF_HEADPTR, ctx->page_size);
            int64_t slot_sib = vfs_rd8_s(slot.bytes, ANCHOR_OFF_SIBPTR, ctx->page_size);
            pool_release(&ctx->pool, &slot);

            /* Walk the DirContent chain. */
            int64_t dc_vp = slot_head;
            while (dc_vp != 0 && !*out_found) {
                PoolSlot dc = {0};
                pool_acquire(&ctx->pool, dc_vp, false, &dc);
                if (dc.vptr == VFS_VPTR_NULL) break;
                int64_t child_ptr = vfs_rd8_s(dc.bytes, DIRCONTENT_OFF_CHILDPTR, ctx->page_size);
                int64_t name_ptr = vfs_rd8_s(dc.bytes, DIRCONTENT_OFF_NAMEPTR, ctx->page_size);
                int64_t dc_next = vfs_rd8_s(dc.bytes, DIRCONTENT_OFF_NEXTPTR, ctx->page_size);
                int16_t dc_type = (int16_t)vfs_rd2_s(dc.bytes,
                                                       DIRCONTENT_OFF_EPOCH,
                                                       ctx->page_size);
                pool_release(&ctx->pool, &dc);

                if (child_ptr == file_vp && name_ptr != 0) {
                    /* Found a live entry for this file.  This is
                       the parent.  Don't recurse further. */
                    *out_found = 1;
                    *out_parent = dir_vp;
                    return VFS_OK;
                }
                /* If this is a subdir (FileNode-style? actually
                   type is in epoch; need to check node type from
                   the child), recurse into it.  The child type
                   can be determined by reading the child slot
                   (FileNode has type=0x03, DirNode has type=0x01).
                   For simplicity, we attempt to recurse into any
                   child with child_ptr != 0 and a non-zero
                   DirContent (i.e., not a tombstone).  If the
                   child is a FileNode, the recursion's
                   pool_acquire fails and we skip. */
                if (name_ptr != 0 && child_ptr != 0) {
                    int err = walk_dir_recursive(ctx, child_ptr, file_vp,
                                                  out_found, out_parent);
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

/* ---------------------------------------------------------------------------
 * find_slotnode_in_dir — find the SlotNode VP for a given child_id in
 * the given dir.  Mirrors dirchain_find_slotnode in tree.c.
 * --------------------------------------------------------------------------- */
static int find_slotnode_in_dir(TreeContext* ctx, int64_t dir_vp,
                                 uint32_t child_id, int64_t* out_slot_vp) {
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
            uint32_t slot_id = (uint32_t)vfs_rd4_s(slot.bytes,
                                                    ANCHOR_OFF_ID,
                                                    ctx->page_size);
            int64_t slot_sib = vfs_rd8_s(slot.bytes, ANCHOR_OFF_SIBPTR, ctx->page_size);
            pool_release(&ctx->pool, &slot);
            if (slot_id == child_id) {
                *out_slot_vp = slot_vp;
                return VFS_OK;
            }
            slot_vp = slot_sib;
        }
        seg_vp = seg_sib;
    }
    return VFS_ERR_NOTFOUND;
}

/* ---------------------------------------------------------------------------
 * read_rule_pick_first_dc — apply the read rule at R to the SlotNode's
 * DirContent chain, return the first applicable DirContent VP.
 *
 * Per the read rule:
 *   1. If traversalApply is true for entry's epoch: replace epoch
 *      with mapper_table_resolve(epoch).
 *   2. exact match (eff_epoch == R) -> use it.
 *   3. eff_epoch < R AND even -> use it (committed base).
 *   4. eff_epoch < R AND odd -> skip.
 *   5. eff_epoch > R -> skip.
 * --------------------------------------------------------------------------- */
static int read_rule_pick_first_dc(TreeContext* ctx, int64_t slot_vp,
                                    int64_t read_epoch, int64_t* out_dc_vp) {
    if (!ctx || !slot_vp || !out_dc_vp) return VFS_ERR_IO;
    *out_dc_vp = 0;

    PoolSlot slot = {0};
    pool_acquire(&ctx->pool, slot_vp, false, &slot);
    if (slot.vptr == VFS_VPTR_NULL) return VFS_ERR_NOTFOUND;
    int64_t dc_head = vfs_rd8_s(slot.bytes, ANCHOR_OFF_HEADPTR, ctx->page_size);
    pool_release(&ctx->pool, &slot);

    int64_t dc_vp = dc_head;
    int64_t first_dc = 0;
    while (dc_vp != 0) {
        PoolSlot dc = {0};
        pool_acquire(&ctx->pool, dc_vp, false, &dc);
        if (dc.vptr == VFS_VPTR_NULL) break;
        uint32_t dc_epoch = (uint32_t)vfs_rd4_s(dc.bytes,
                                                  DIRCONTENT_OFF_EPOCH,
                                                  ctx->page_size);
        int64_t dc_next = vfs_rd8_s(dc.bytes, DIRCONTENT_OFF_NEXTPTR, ctx->page_size);
        pool_release(&ctx->pool, &dc);

        int64_t eff_epoch = (int64_t)dc_epoch;
        if (mapper_table_traversal_apply(&ctx->mapper_table, (int64_t)dc_epoch)) {
            eff_epoch = mapper_table_resolve(&ctx->mapper_table, (int64_t)dc_epoch);
        }
        if (eff_epoch == read_epoch) {
            *out_dc_vp = dc_vp;
            return VFS_OK;
        }
        if (eff_epoch < read_epoch && (eff_epoch & 1) == 0) {
            /* Committed base.  Since chains are descending, the
               FIRST such is the highest even below R.  Use it. */
            *out_dc_vp = dc_vp;
            return VFS_OK;
        }
        /* Otherwise: odd < R (skip) or future (skip). */
        dc_vp = dc_next;
    }
    return VFS_OK;  /* not found is not an error; *out_dc_vp == 0 */
}

/* ---------------------------------------------------------------------------
 * check_file_referenced — determine if the file is "visible" at any
 * reference point, and find the create entry's VP.
 *
 * A file is visible at R iff the read rule at R picks a DirContent
 * with namePtr != 0 AND childPtr == file_vp.  If any R is visible,
 * the file is referenced.
 *
 * The create entry is the one with namePtr != 0 picked by the read
 * rule.  We pick the create entry from the H view (or any R's view,
 * but H is canonical).
 * --------------------------------------------------------------------------- */
static int check_file_referenced(TreeContext* ctx, int64_t file_vp,
                                  int64_t slot_vp, const ref_points_t* rp,
                                  int* out_referenced,
                                  int64_t* out_create_vp) {
    if (!ctx || !file_vp || !slot_vp || !rp || !out_referenced || !out_create_vp) {
        return VFS_ERR_IO;
    }
    *out_referenced = 0;
    *out_create_vp = 0;

    /* For each reference point, apply the read rule to the SlotNode's
       chain.  If the picked DirContent has namePtr != 0 AND childPtr
       == file_vp, the file is visible at R. */
    for (int ri = 0; ri <= rp->num_active; ri++) {
        int64_t R = (ri == 0) ? rp->H : rp->active[ri - 1];
        int64_t dc_vp = 0;
        if (read_rule_pick_first_dc(ctx, slot_vp, R, &dc_vp) != VFS_OK) {
            continue;
        }
        if (dc_vp == 0) continue;
        PoolSlot dc = {0};
        pool_acquire(&ctx->pool, dc_vp, false, &dc);
        if (dc.vptr == VFS_VPTR_NULL) continue;
        int64_t name_ptr = vfs_rd8_s(dc.bytes, DIRCONTENT_OFF_NAMEPTR, ctx->page_size);
        int64_t child_ptr = vfs_rd8_s(dc.bytes, DIRCONTENT_OFF_CHILDPTR, ctx->page_size);
        pool_release(&ctx->pool, &dc);
        if (name_ptr != 0 && child_ptr == file_vp) {
            *out_referenced = 1;
            *out_create_vp = dc_vp;
            return VFS_OK;
        }
    }
    return VFS_OK;
}

/* ---------------------------------------------------------------------------
 * find_create_in_slot — walk the SlotNode's DirContent chain and find
 * the create entry (the one with namePtr != 0 and childPtr == file_vp).
 * Returns the create entry's VP, or 0 if not found.
 *
 * Used when the create is shadowed by a tombstone at the current
 * reference points (so the read-rule walk doesn't pick it).  The
 * drop logic needs the create's VP to remove both entries.
 * --------------------------------------------------------------------------- */
static int64_t find_create_in_slot(TreeContext* ctx, int64_t slot_vp,
                                     int64_t file_vp) {
    if (!ctx || !slot_vp || !file_vp) return 0;

    PoolSlot slot = {0};
    pool_acquire(&ctx->pool, slot_vp, false, &slot);
    if (slot.vptr == VFS_VPTR_NULL) return 0;
    int64_t dc_head = vfs_rd8_s(slot.bytes, ANCHOR_OFF_HEADPTR, ctx->page_size);
    pool_release(&ctx->pool, &slot);

    int64_t dc_vp = dc_head;
    while (dc_vp != 0) {
        PoolSlot dc = {0};
        pool_acquire(&ctx->pool, dc_vp, false, &dc);
        if (dc.vptr == VFS_VPTR_NULL) break;
        int64_t name_ptr = vfs_rd8_s(dc.bytes, DIRCONTENT_OFF_NAMEPTR, ctx->page_size);
        int64_t child_ptr = vfs_rd8_s(dc.bytes, DIRCONTENT_OFF_CHILDPTR, ctx->page_size);
        int64_t dc_next = vfs_rd8_s(dc.bytes, DIRCONTENT_OFF_NEXTPTR, ctx->page_size);
        pool_release(&ctx->pool, &dc);
        if (name_ptr != 0 && child_ptr == file_vp) {
            return dc_vp;
        }
        dc_vp = dc_next;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * drop_dir_entries — inline CAS-based removal of create + tombstone
 * from the SlotNode's DirContent chain.
 *
 * Returns:
 *   0  = at least one entry was removed
 *   1  = no-op (entries already gone, e.g. from a prior run)
 *   -1 = I/O error
 *
 * The caller uses the return value to decide whether to free the
 * slot VPs (only on actual removal — pool_free is not idempotent).
 *
 * The two removals are independent CAS operations.  Between them, a
 * concurrent reader sees one entry removed and the other still
 * present.  This is safe: the read rule at any R (where R is not
 * equal to the tombstone's epoch) does not pick the tombstone
 * anyway, so the reader's view is unchanged.  The create entry, if
 * picked, correctly reflects the file's visibility.
 *
 * Idempotent: if entries are already gone, the walk finds nothing
 * and the function is a no-op.
 * --------------------------------------------------------------------------- */
static int drop_dir_entries(TreeContext* ctx, int64_t slot_vp,
                             int64_t create_vp, int64_t tombstone_vp) {
    if (!ctx || !slot_vp || !tombstone_vp) return VFS_ERR_IO;

    /* Phase A: walk the chain to find the predecessor of the
       tombstone and (if applicable) the create.  The create's
       predecessor is the entry whose nextPtr == create_vp.  The
       tombstone's predecessor is the entry whose nextPtr ==
       tombstone_vp.  In the common case, the create is the
       predecessor of the tombstone (the tombstone was just
       prepended to the SlotNode, and the create is the previous
       head). */
    int64_t tom_pred = 0;     /* predecessor of tombstone (VP) */
    int64_t tom_pred_next = 0; /* its nextPtr at time of read */
    int64_t cre_pred = 0;     /* predecessor of create (VP, if create exists and != tom_pred) */
    int64_t cre_pred_next = 0; /* its nextPtr at time of read */

    int64_t prev __attribute__((unused)) = 0;
    int64_t cur = 0;
    PoolSlot slot = {0};
    pool_acquire(&ctx->pool, slot_vp, false, &slot);
    if (slot.vptr == VFS_VPTR_NULL) return VFS_ERR_NOTFOUND;
    int64_t head = vfs_rd8_s(slot.bytes, ANCHOR_OFF_HEADPTR, ctx->page_size);
    pool_release(&ctx->pool, &slot);

    /* Special case: tombstone is the head.  tom_pred = 0 (we'll
       update SlotNode's headPtr). */
    if (head == tombstone_vp) {
        tom_pred = 0;
    } else {
        /* Walk to find the predecessor of the tombstone. */
        prev = 0;
        cur = head;
        while (cur != 0) {
            PoolSlot c = {0};
            pool_acquire(&ctx->pool, cur, false, &c);
            if (c.vptr == VFS_VPTR_NULL) break;
            int64_t next_vp = vfs_rd8_s(c.bytes, DIRCONTENT_OFF_NEXTPTR, ctx->page_size);
            pool_release(&ctx->pool, &c);
            if (next_vp == tombstone_vp) {
                tom_pred = cur;
                tom_pred_next = tombstone_vp;
                break;
            }
            prev = cur;
            cur = next_vp;
        }
        if (tom_pred == 0) {
            /* Tombstone not in chain (already removed).  Idempotent:
               no entry was removed, the caller should not free the
               tombstone slot. */
            return 1;
        }
    }

    /* If create_vp != 0 AND create_vp != tombstone_vp, find its
       predecessor.  create_vp may be tom_pred (the create was the
       predecessor of the tombstone), in which case dropping the
       tombstone alone removes both. */
    if (create_vp != 0 && create_vp != tombstone_vp) {
        if (head == create_vp) {
            cre_pred = 0;
        } else {
            prev = 0;
            cur = head;
            while (cur != 0) {
                PoolSlot c = {0};
                pool_acquire(&ctx->pool, cur, false, &c);
                if (c.vptr == VFS_VPTR_NULL) break;
                int64_t next_vp = vfs_rd8_s(c.bytes, DIRCONTENT_OFF_NEXTPTR, ctx->page_size);
                pool_release(&ctx->pool, &c);
                if (next_vp == create_vp) {
                    cre_pred = cur;
                    cre_pred_next = create_vp;
                    break;
                }
                prev = cur;
                cur = next_vp;
            }
            /* If cre_pred == 0, create was not in chain (already removed). */
        }
    }

    /* Phase C: CAS-based removal.
       Order matters for race safety: drop the CREATE first, then
       the tombstone.  Reasoning: between the two CAS operations,
       a concurrent reader walking the chain may see the chain in
       an intermediate state.  If the create is dropped first,
       the intermediate state has only the tombstone (file hidden).
       If the tombstone is dropped first, the intermediate state
       has only the create (file visible, BAD).
       So: create first, then tombstone. */

    /* Get the create's nextPtr (its successor). */
    int64_t cre_next = 0;
    int64_t cre_remove_ok = 0;
    if (create_vp != 0 && create_vp != tombstone_vp && cre_pred != 0) {
        PoolSlot c = {0};
        pool_acquire(&ctx->pool, create_vp, false, &c);
        if (c.vptr != VFS_VPTR_NULL) {
            cre_next = vfs_rd8_s(c.bytes, DIRCONTENT_OFF_NEXTPTR, ctx->page_size);
        }
        pool_release(&ctx->pool, &c);

        if (cre_pred == 0) {
            /* Create is the head.  Update SlotNode's headPtr. */
            PoolSlot s = {0};
            pool_acquire(&ctx->pool, slot_vp, true, &s);
            if (s.vptr != VFS_VPTR_NULL) {
                int64_t* head_ptr_field = (int64_t*)(s.bytes + ANCHOR_OFF_HEADPTR);
                if (vfs_cas_i64(head_ptr_field, create_vp, cre_next) == create_vp) {
                    cre_remove_ok = 1;
                }
                pool_release(&ctx->pool, &s);
            }
        } else {
            PoolSlot p = {0};
            pool_acquire(&ctx->pool, cre_pred, true, &p);
            if (p.vptr != VFS_VPTR_NULL) {
                int64_t* next_ptr_field = (int64_t*)(p.bytes + DIRCONTENT_OFF_NEXTPTR);
                if (vfs_cas_i64(next_ptr_field, create_vp, cre_next) == create_vp) {
                    cre_remove_ok = 1;
                }
                pool_release(&ctx->pool, &p);
            }
        }
    }

    /* Get the tombstone's nextPtr (its successor). */
    int64_t tom_next = 0;
    PoolSlot tom = {0};
    pool_acquire(&ctx->pool, tombstone_vp, false, &tom);
    if (tom.vptr != VFS_VPTR_NULL) {
        tom_next = vfs_rd8_s(tom.bytes, DIRCONTENT_OFF_NEXTPTR, ctx->page_size);
    }
    pool_release(&ctx->pool, &tom);

    /* CAS-remove the tombstone.  The SlotNode's headPtr may have
       changed if cre_remove_ok set it (if create was the head).
       Re-read the headPtr in that case. */
    if (tom_pred == 0 && cre_remove_ok) {
        /* The create was the head and was just removed.  The
           SlotNode's headPtr is now cre_next.  The tombstone
           (which was after the create) is at cre_next, NOT at
           the SlotNode's head.  We need to find the tombstone's
           predecessor in the chain, which is just whatever points
           to the tombstone now.  This is complex — for simplicity,
           walk the chain to find tom_pred fresh. */
        tom_pred = 0;
        int64_t cur = cre_next;
        while (cur != 0) {
            PoolSlot c2 = {0};
            pool_acquire(&ctx->pool, cur, false, &c2);
            if (c2.vptr == VFS_VPTR_NULL) break;
            int64_t nxt = vfs_rd8_s(c2.bytes, DIRCONTENT_OFF_NEXTPTR, ctx->page_size);
            pool_release(&ctx->pool, &c2);
            if (nxt == tombstone_vp) { tom_pred = cur; break; }
            cur = nxt;
        }
    }

    if (tom_pred == 0) {
        /* Update SlotNode's headPtr. */
        PoolSlot s = {0};
        pool_acquire(&ctx->pool, slot_vp, true, &s);
        if (s.vptr != VFS_VPTR_NULL) {
            int64_t* head_ptr_field = (int64_t*)(s.bytes + ANCHOR_OFF_HEADPTR);
            vfs_cas_i64(head_ptr_field, tombstone_vp, tom_next);
            pool_release(&ctx->pool, &s);
        }
    } else {
        /* CAS-update tom_pred's nextPtr. */
        PoolSlot p = {0};
        pool_acquire(&ctx->pool, tom_pred, true, &p);
        if (p.vptr != VFS_VPTR_NULL) {
            int64_t* next_ptr_field = (int64_t*)(p.bytes + DIRCONTENT_OFF_NEXTPTR);
            vfs_cas_i64(next_ptr_field, tombstone_vp, tom_next);
            pool_release(&ctx->pool, &p);
        }
    }

    /* Phase D: chain index update.  Per the spec §3.8 (M3 fix),
       update the dir's index via dircontentindex_remove.  The index
       is keyed on (name_hash, dirContentVP).  We don't have the
       name in this function (only the create_vp), so we walk the
       index.  For simplicity, we skip the index update here —
       the index will be rebuilt lazily on the next dir chain
       walk.  This is the conservative option (option b from the
       spec's M3 review) — option a (per-entry remove) is more
       efficient but requires the name hash, which we'd need to
       pass in.  TODO: thread the name through for full
       option-a support. */

    /* At least one entry was removed (either the create, the
       tombstone, or both).  The caller (gc_handle_file_deleted)
       uses this to decide whether to free the slot VPs. */
    return 0;
}

/* ---------------------------------------------------------------------------
 * batch_dead_pages_into_pool_list — allocate a pool-allocated linked
 * list of dead pages, return the head VP.
 *
 * Per-batch linked list, allocated as a chain of pool slots:
 *   Each slot: int64 next_batch_slot; int32 logical_page; int32 padding
 *   (16 bytes used per slot = 1 pool slot since pool slots are 32 bytes)
 *
 * The list is consumed by the work handler, which iterates and frees.
 * --------------------------------------------------------------------------- */
static int batch_dead_pages_into_pool_list(TreeContext* ctx,
                                            const int64_t* dead_logicals,
                                            int num_dead,
                                            int64_t* out_batch_head) {
    if (!ctx || !dead_logicals || num_dead <= 0 || !out_batch_head) return VFS_ERR_IO;
    *out_batch_head = 0;
    if (num_dead == 0) return VFS_OK;

    /* Allocate the first slot. */
    int64_t head = pool_alloc(&ctx->pool);
    if (head == VFS_VPTR_NULL) return VFS_ERR_NOMEM;
    PoolSlot slot = {0};
    pool_acquire(&ctx->pool, head, true, &slot);
    if (slot.vptr == VFS_VPTR_NULL) return VFS_ERR_IO;
    vfs_wr8_s(slot.bytes, 0, 0, ctx->page_size);   /* next = 0 */
    vfs_wr4_s(slot.bytes, 8, (int32_t)dead_logicals[0], ctx->page_size);  /* logical */
    vfs_wr4_s(slot.bytes, 12, 0, ctx->page_size);  /* padding */
    pool_release(&ctx->pool, &slot);
    int64_t prev = head;
    *out_batch_head = head;

    /* Allocate the rest. */
    for (int i = 1; i < num_dead; i++) {
        int64_t cur = pool_alloc(&ctx->pool);
        if (cur == VFS_VPTR_NULL) {
            /* Partial list — the work handler will free the pages
               up to the broken link, then return.  Remaining dead
               pages are reclaimed by the next shadow-compaction. */
            return VFS_ERR_NOMEM;
        }
        PoolSlot s = {0};
        pool_acquire(&ctx->pool, cur, true, &s);
        if (s.vptr == VFS_VPTR_NULL) return VFS_ERR_IO;
        vfs_wr8_s(s.bytes, 0, 0, ctx->page_size);   /* next = 0 (will be set by prev link below) */
        vfs_wr4_s(s.bytes, 8, (int32_t)dead_logicals[i], ctx->page_size);
        vfs_wr4_s(s.bytes, 12, 0, ctx->page_size);
        pool_release(&ctx->pool, &s);
        /* Link prev -> cur. */
        PoolSlot p = {0};
        pool_acquire(&ctx->pool, prev, true, &p);
        if (p.vptr != VFS_VPTR_NULL) {
            vfs_wr8_s(p.bytes, 0, cur, ctx->page_size);
            pool_release(&ctx->pool, &p);
        }
        prev = cur;
    }
    return VFS_OK;
}

/* ---------------------------------------------------------------------------
 * read_mirror_page — read the PageHeader of a logical page and return
 * its mirror sibling's logical page index (-1 if none).
 * --------------------------------------------------------------------------- */
static int64_t read_mirror_page(StorageBackend* sb, int64_t logical) {
    if (!sb || logical < 2) return -1;
    int64_t phys = indir_lookup(sb, logical);
    if (phys == 0) return -1;
    PageHeader ph;
    ssize_t n = pread(sb->fd, &ph, PAGE_HEADER_SIZE, phys);
    if (n != PAGE_HEADER_SIZE) return -1;
    return (int64_t)ph.mirror_page;
}

/* ---------------------------------------------------------------------------
 * free_logical_with_mirror — free a logical page AND its mirror sibling.
 * Idempotent.  Used by the work handler (§4.1).
 * --------------------------------------------------------------------------- */
static int free_logical_with_mirror(StorageBackend* sb, int64_t logical) {
    if (!sb || logical < 2) return VFS_OK;
    int64_t mirror = read_mirror_page(sb, logical);
    storage_free(sb, logical);
    if (mirror >= 0) storage_free(sb, mirror);
    return VFS_OK;
}

/* ---------------------------------------------------------------------------
 * gc_handle_free_pages — work handler (Phase 28 Type 1).
 *
 * Iterates the per-batch linked list, calls storage_free on each
 * logical page + mirror.  Implemented in src/gc_bin_free_pages.c
 * (this is a forward decl so the trigger's analysis can be in this
 * file; the actual work handler is small enough to live in either).
 *
 * Defined below in src/gc_bin_free_pages.c; this is just the decl
 * to keep the analysis self-contained.
 * --------------------------------------------------------------------------- */
extern int gc_handle_free_pages(vfs_t* vfs, const BinEntry* entry);
