/* Phase 6: Epoch system — replaces Phase 5a stubs. */
#include "epoch.h"
#include "vfs_internal.h"
#include "touched.h"
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
        int64_t resolved = mapper_resolve(&ctx->mapper, epoch);
        return resolved == epoch;
    }

    /* Even epoch that isn't currentEpoch → not writable (frozen past) */
    return false;
}

void epoch_touchedfile_add(TreeContext* ctx, int64_t epoch, uint32_t nodeId) {
    if (!ctx) return;
    touchedfile_add(&ctx->pool, &ctx->touchedFilesPtr, (uint32_t)epoch, nodeId);
}

int64_t vfs_snapshot(vfs_t* vfs) {
    if (!vfs || !vfs->ctx) return -1;
    TreeContext* ctx = vfs->ctx;

    /* Atomically increment currentEpoch by 2 (add_fetch returns new value).
       Snapshot epoch = old value + 1 = new value - 1. */
    int64_t new_epoch = vfs_atomic_add_i64(&ctx->currentEpoch, 2);
    return new_epoch - 1;  /* snapshot epoch is always odd */
}

int vfs_commit(vfs_t* vfs, int64_t snapshot_epoch) {
    if (!vfs || !vfs->ctx) return VFS_ERR_IO;
    TreeContext* ctx = vfs->ctx;
    uint32_t s_epoch = (uint32_t)snapshot_epoch;

    /* Validate snapshot_epoch is odd */
    if (snapshot_epoch % 2 == 0) return VFS_ERR_IO;

    /* Validate snapshot_epoch is still active (no MapperEntry for it) */
    if (mapper_resolve(&ctx->mapper, snapshot_epoch) != snapshot_epoch)
        return VFS_ERR_IO;

    /* Conflict detection: walk the TouchedFile chain directly (no fixed buffer).
       For each file modified in this snapshot epoch, scan its version chains
       looking for conflicts at even epochs > snapshot_epoch. */
    int64_t tf_vp = ctx->touchedFilesPtr;
    while (tf_vp != 0) {
        uint8_t* tf_slot = pool_resolve(&ctx->pool, tf_vp);
        if (!tf_slot) break;
        uint32_t tf_epoch, tf_nodeId;
        int64_t tf_next;
        nodes_read_touchedfile(tf_slot, &tf_epoch, &tf_nodeId, &tf_next);

        if (tf_epoch == s_epoch) {
            /* Walk root DirContent chain to find the file's VirtualPtr by nodeId.
               (Subdirectory support deferred — all files are root-level until
               recursive directory scan is implemented.) */
            int64_t root_vp = ctx->rootNodeOffset;
            uint8_t* root_slot = pool_resolve(&ctx->pool, root_vp);
            if (!root_slot) { tf_vp = tf_next; continue; }

            int64_t walk_vp = vfs_rd8(root_slot, DIRNODE_OFF_HEADPTR);
            while (walk_vp != 0) {
                uint8_t* dc_slot = pool_resolve(&ctx->pool, walk_vp);
                if (!dc_slot) break;
                uint32_t dc_child, dc_epoch;
                int64_t dc_childPtr, dc_namePtr, dc_next;
                nodes_read_dircontent(dc_slot, &dc_child, &dc_epoch, &dc_childPtr,
                                      &dc_namePtr, &dc_next);
                (void)dc_epoch; (void)dc_namePtr;

                if (dc_child == tf_nodeId) {
                    /* Found the file — walk its version chains using
                       tree_resolve_page to get PageNode slots. */
                    for (int64_t lp = 0; ; lp++) {
                        uint8_t* pn_slot = tree_resolve_page(ctx, dc_childPtr,
                                                              lp, 0);
                        if (!pn_slot) break;  /* beyond file growth */

                        int64_t vp = vfs_atomic_load_i64(
                            (const int64_t*)(pn_slot + PAGENODE_OFF_VERSIONROOT));
                        int has_snapshot = 0;
                        int has_live = 0;

                        while (vp != 0) {
                            uint8_t* vp_slot = pool_resolve(&ctx->pool, vp);
                            if (!vp_slot) break;
                            uint32_t v_epoch;
                            int64_t v_dataPage, v_next;
                            nodes_read_versionpage(vp_slot, &v_epoch,
                                                    &v_dataPage, &v_next);
                            (void)v_dataPage;

                            if (v_epoch == s_epoch) has_snapshot = 1;
                            if (v_epoch > s_epoch && v_epoch % 2 == 0)
                                has_live = 1;

                            vp = v_next;
                        }

                        if (has_snapshot && has_live)
                            return VFS_ERR_CONFLICT;
                    }
                }
                walk_vp = dc_next;
            }
        }
        tf_vp = tf_next;
    }

    /* Insert commit mapping: snapshot_epoch → currentEpoch with traversalApply */
    int64_t current = ctx->currentEpoch;
    int ret = mapper_insert(&ctx->mapper, (uint32_t)snapshot_epoch,
                            (uint32_t)current, MAPPER_FLAG_TRAVERSAL_APPLY);
    if (ret != VFS_OK) return ret;

    /* Drop TouchedFile chain for this epoch */
    touchedfile_drop(&ctx->pool, &ctx->touchedFilesPtr, s_epoch);

    /* Flush superblock to persist the mapper change */
    ret = tree_superblock_write(ctx);

    return ret;
}

int vfs_delete_snapshot(vfs_t* vfs, int64_t snapshot_epoch) {
    if (!vfs || !vfs->ctx) return VFS_ERR_IO;
    TreeContext* ctx = vfs->ctx;

    /* Validate snapshot_epoch is odd */
    if (snapshot_epoch % 2 == 0) return VFS_ERR_IO;

    /* Validate snapshot_epoch is still active */
    if (mapper_resolve(&ctx->mapper, snapshot_epoch) != snapshot_epoch)
        return VFS_ERR_IO;

    /* Insert soft-delete mapping: snapshot_epoch → currentEpoch without traversalApply */
    int64_t current = ctx->currentEpoch;
    int ret = mapper_insert(&ctx->mapper, (uint32_t)snapshot_epoch,
                            (uint32_t)current, 0);
    if (ret != VFS_OK) return ret;

    /* Drop TouchedFile chain for this epoch */
    touchedfile_drop(&ctx->pool, &ctx->touchedFilesPtr, (uint32_t)snapshot_epoch);

    /* Flush superblock */
    ret = tree_superblock_write(ctx);

    return ret;
}
