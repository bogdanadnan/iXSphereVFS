/* Phase 6: Epoch system — replaces Phase 5a stubs. */
#include "epoch.h"
#include "ixsphere/vfs_internal.h"
#include "tree.h"
#include <stdlib.h>

/* Test override: -1 = use real implementation (Phase 6 default).
   0 = frozen, 1 = all writable (backward compat for existing tests). */
static int _test_epoch_writable = 1;

void test_set_epoch_writable(int writable) {
    _test_epoch_writable = writable;
}

bool vfs_epoch_is_writable(TreeContext* ctx, int64_t epoch) {
    /* Test override: if set to 0 or 1, use that value directly */
    if (_test_epoch_writable >= 0)
        return _test_epoch_writable != 0;

    /* epoch == -1 means current live head */
    if (epoch == -1) epoch = ctx->currentEpoch;

    /* Live head (current even epoch) is always writable */
    if (epoch == ctx->currentEpoch) return true;

    /* Odd epoch (snapshot): writable if NOT in the mapper chain.
       Being in the mapper means it was committed or soft-deleted. */
    if (epoch % 2 == 1) {
        int64_t resolved = mapper_table_resolve(&ctx->mapper_table, epoch);
        return resolved == epoch;
    }

    /* Even epoch that isn't currentEpoch → not writable (frozen past) */
    return false;
}

int64_t vfs_snapshot(vfs_t* vfs) {
    if (!vfs || !vfs->ctx) return -1;
    TreeContext* ctx = vfs->ctx;

    /* Atomically increment currentEpoch by 2 (add_fetch returns new value).
       Snapshot epoch = old value + 1 = new value - 1. */
    int64_t new_epoch = vfs_atomic_add_i64(&ctx->currentEpoch, 2);
    return new_epoch - 1;  /* snapshot epoch is always odd */
}

/* ---------------------------------------------------------------------------
 * Recursive commit scan — walk a directory tree checking for version conflicts
 * during commit.  Returns 0 if no conflict, VFS_ERR_CONFLICT on first conflict.
 * --------------------------------------------------------------------------- */

static int commit_scan_dir(vfs_t* vfs, int64_t dir_vp, uint32_t s_epoch) {
    TreeContext* ctx = vfs->ctx;
    /* Phase 25: by-value pool slot (read-only). */
    PoolSlot dir_slot = {0};
    pool_acquire(&ctx->pool, dir_vp, false, &dir_slot);
    if (dir_slot.vptr == VFS_VPTR_NULL) return 0;
    if (vfs_rd2_s(dir_slot.bytes, DIRNODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_DIR) {
        pool_release(&ctx->pool, &dir_slot);
        return 0;
    }

    int64_t head = vfs_rd8_s(dir_slot.bytes, DIRNODE_OFF_HEADPTR, ctx->page_size);
    pool_release(&ctx->pool, &dir_slot);

    /* W5b: headPtr is a DirSegment chain.  Walk each Segment, then
     * each Segment's SlotNode chain, then each SlotNode's DirContent
     * chain (first applicable entry per SlotNode, per the read-rule).
     * This mirrors dirchain_list's walk structure. */
    int64_t seg_walk_vp = head;
    while (seg_walk_vp != 0) {
        PoolSlot seg_slot = {0};
        pool_acquire(&ctx->pool, seg_walk_vp, false, &seg_slot);
        if (seg_slot.vptr == VFS_VPTR_NULL) break;
        AnchorKind seg_ak;
        uint32_t seg_id, seg_cnt;
        int64_t seg_head, seg_sib;
        nodes_read_anchor(seg_slot.bytes, &seg_ak, &seg_id, &seg_head,
                          &seg_sib, &seg_cnt, ctx->page_size);
        pool_release(&ctx->pool, &seg_slot);

        int64_t slot_walk_vp = seg_head;
        while (slot_walk_vp != 0) {
        PoolSlot slot_slot = {0};
        pool_acquire(&ctx->pool, slot_walk_vp, false, &slot_slot);
        if (slot_slot.vptr == VFS_VPTR_NULL) break;
        AnchorKind ak;
        uint32_t slot_id;
        int64_t slot_head, slot_sib;
        uint32_t slot_count;
        nodes_read_anchor(slot_slot.bytes, &ak, &slot_id, &slot_head,
                          &slot_sib, &slot_count, ctx->page_size);
        pool_release(&ctx->pool, &slot_slot);

        int64_t dc_childPtr = 0;
        int found_visible = 0;
        int64_t dc_walk_vp = slot_head;
        while (dc_walk_vp != 0 && !found_visible) {
            PoolSlot dc_slot = {0};
            pool_acquire(&ctx->pool, dc_walk_vp, false, &dc_slot);
            if (dc_slot.vptr == VFS_VPTR_NULL) break;
            uint32_t dc_child, dc_epoch;
            int64_t dc_namePtr, dc_next;
            nodes_read_dircontent(dc_slot.bytes, &dc_child, &dc_epoch,
                                  &dc_childPtr, &dc_namePtr, &dc_next,
                                  ctx->page_size);
            pool_release(&ctx->pool, &dc_slot);

            int applies = (dc_epoch == s_epoch) ||
                          (dc_epoch < s_epoch && dc_epoch % 2 == 0);
            if (applies && dc_namePtr != 0) {
                found_visible = 1;
            }
            dc_walk_vp = dc_next;
        }
        if (!found_visible) { slot_walk_vp = slot_sib; continue; }

        /* Phase 25: by-value pool slot (read-only) for child type check. */
        PoolSlot child_slot = {0};
        pool_acquire(&ctx->pool, dc_childPtr, false, &child_slot);
        if (child_slot.vptr == VFS_VPTR_NULL) { slot_walk_vp = slot_sib; continue; }

        int16_t child_type = vfs_rd2_s(child_slot.bytes, DIRNODE_OFF_TYPE, ctx->page_size);
        int64_t sizePtr = vfs_rd8_s(child_slot.bytes, FILENODE_OFF_SIZEPTR, ctx->page_size);
        pool_release(&ctx->pool, &child_slot);

        if (child_type == (int16_t)NODE_TYPE_DIR) {
            /* Recurse into subdirectory */
            int err = commit_scan_dir(vfs, dc_childPtr, s_epoch);
            if (err != 0) return err;

        } else if (child_type == (int16_t)NODE_TYPE_FILE) {
            /* Compute file size by walking FileNode's sizePtr chain */
            int64_t fsize = 0;
            int64_t fs_walk = sizePtr;
            while (fs_walk != 0) {
                /* Phase 25: by-value pool slot (read-only). */
                PoolSlot fs_slot = {0};
                pool_acquire(&ctx->pool, fs_walk, false, &fs_slot);
                if (fs_slot.vptr == VFS_VPTR_NULL) break;
                uint32_t fs_epoch;
                int64_t fs_modified, fs_size, fs_next;
                nodes_read_filesize(fs_slot.bytes, &fs_epoch, &fs_modified, &fs_size, &fs_next, ctx->page_size);
                (void)fs_modified;
                pool_release(&ctx->pool, &fs_slot);
                if (fs_epoch == s_epoch || (fs_epoch < s_epoch && fs_epoch % 2 == 0))
                    fsize = fs_size;
                fs_walk = fs_next;
            }
            int64_t num_pages = (fsize + ctx->page_size - 1) / ctx->page_size;
            if (num_pages < 1) num_pages = 1;

            for (int64_t lp = 0; lp < num_pages; lp++) {
                /* Phase 25: by-value pool slot (read-only). */
                PoolSlot pn_slot = {0};
                int rr_pn = tree_resolve_page(vfs, dc_childPtr, lp, 0, false, &pn_slot);
                if (rr_pn != 0) break;

                int64_t vp = vfs_atomic_load_i64(
                    (const int64_t*)(pn_slot.bytes + PAGENODE_OFF_VERSIONROOT));
                int has_snapshot = 0;
                int has_live = 0;

                while (vp != 0) {
                    /* Phase 25: by-value pool slot (read-only). */
                    PoolSlot vp_slot = {0};
                    pool_acquire(&ctx->pool, vp, false, &vp_slot);
                    if (vp_slot.vptr == VFS_VPTR_NULL) break;
                    uint32_t v_epoch;
                    int64_t v_dataPage, v_next;
                    nodes_read_versionpage(vp_slot.bytes, &v_epoch, &v_dataPage,
                                           &v_next, ctx->page_size);
                    (void)v_dataPage;
                    pool_release(&ctx->pool, &vp_slot);

                    if (v_epoch == s_epoch) has_snapshot = 1;
                    if (v_epoch > s_epoch && v_epoch % 2 == 0) has_live = 1;
                    vp = v_next;
                }
                if (has_snapshot && has_live) return VFS_ERR_CONFLICT;
            }
        }

        slot_walk_vp = slot_sib;
        }
        seg_walk_vp = seg_sib;
    }
    return 0;
}

int vfs_commit(vfs_t* vfs, int64_t snapshot_epoch) {
    if (!vfs || !vfs->ctx) return VFS_ERR_IO;
    TreeContext* ctx = vfs->ctx;
    uint32_t s_epoch = (uint32_t)snapshot_epoch;

    /* Validate snapshot_epoch is odd */

    if (snapshot_epoch % 2 == 0) { vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }

    /* Validate snapshot_epoch is still active (no MapperEntry for it) */
    if (mapper_resolve(&ctx->mapper, snapshot_epoch) != snapshot_epoch) {
        vfs->ctx->last_error = VFS_ERR_IO;
        return VFS_ERR_IO;
        }

    /* Conflict detection: walk the live directory tree (commit_scan_dir)
       at the snapshot epoch.  For each FILE visible at s_epoch, scan
       its per-page VersionPage chains and flag a conflict if BOTH
       have a snapshot-epoch entry AND a higher live-epoch entry. */
    {
        int err = commit_scan_dir(vfs, vfs->ctx->rootNodeOffset, s_epoch);
        if (err != 0) {
            vfs->ctx->last_error = err;
            return err;
        }
    }

    /* Insert commit mapping: snapshot_epoch → currentEpoch with traversalApply */
    int64_t current = ctx->currentEpoch;
    int ret = mapper_insert(&ctx->mapper, (uint32_t)snapshot_epoch,
                            (uint32_t)current, MAPPER_FLAG_TRAVERSAL_APPLY);
    if (ret != VFS_OK) return ret;

    /* Update in-memory mapper table (pool write already done above) */
    {
        int err = mapper_table_append(&ctx->mapper_table,
                      (uint32_t)snapshot_epoch, (uint32_t)current, true);
        if (err != VFS_OK) return err;
    }

    /* Flush superblock to persist the mapper change */
    ret = tree_superblock_write(ctx);

    return ret;
}

int vfs_delete_snapshot(vfs_t* vfs, int64_t snapshot_epoch) {
    if (!vfs || !vfs->ctx) return VFS_ERR_IO;
    TreeContext* ctx = vfs->ctx;

    /* Validate snapshot_epoch is odd */

    if (snapshot_epoch % 2 == 0) { vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }

    /* Validate snapshot_epoch is still active */
    if (mapper_resolve(&ctx->mapper, snapshot_epoch) != snapshot_epoch) {
        vfs->ctx->last_error = VFS_ERR_IO;
        return VFS_ERR_IO;
        }

    /* Insert soft-delete mapping: snapshot_epoch → (snapshot_epoch - 1)
       without traversalApply flag.  snapshot_epoch - 1 is the even epoch
       (live head) that the snapshot was taken from — readers at odd
       snapshot epochs fall through to this base via the read-rule. */
    uint32_t toEpoch = (uint32_t)snapshot_epoch - 1;
    int ret = mapper_insert(&ctx->mapper, (uint32_t)snapshot_epoch,
                            toEpoch, 0);
    if (ret != VFS_OK) return ret;

    /* Update in-memory mapper table (pool write already done above) */
    {
        int err = mapper_table_append(&ctx->mapper_table,
                      (uint32_t)snapshot_epoch, toEpoch, false);
        if (err != VFS_OK) return err;
    }

    /* Flush superblock */
    ret = tree_superblock_write(ctx);

    return ret;
}
