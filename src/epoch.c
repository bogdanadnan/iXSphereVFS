/* Phase 6: Epoch system — replaces Phase 5a stubs. */
#include "epoch.h"
#include "ixsphere/vfs_internal.h"
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

/* ---------------------------------------------------------------------------
 * Recursive commit scan — walk a directory tree checking for version conflicts
 * during commit.  Returns 0 if no conflict, VFS_ERR_CONFLICT on first conflict.
 * --------------------------------------------------------------------------- */

static int commit_scan_dir(TreeContext* ctx, int64_t dir_vp, uint32_t s_epoch) {
    uint8_t* dir_slot = pool_resolve(&ctx->pool, dir_vp);
    if (!dir_slot) return 0;
    if (vfs_rd2_s(dir_slot, DIRNODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_DIR)
        return 0;

    int64_t head = vfs_rd8_s(dir_slot, DIRNODE_OFF_HEADPTR, ctx->page_size);
    int64_t walk_vp = head;
    while (walk_vp != 0) {
        uint8_t* dc_slot = pool_resolve(&ctx->pool, walk_vp);
        if (!dc_slot) break;
        uint32_t dc_child, dc_epoch;
        int64_t dc_childPtr, dc_namePtr, dc_next;
        nodes_read_dircontent(dc_slot, &dc_child, &dc_epoch, &dc_childPtr,
                              &dc_namePtr, &dc_next, ctx->page_size);
        (void)dc_child;

        /* Check read-rule: does this DirContent entry apply? */
        int applies = (dc_epoch == s_epoch) ||
                      (dc_epoch < s_epoch && dc_epoch % 2 == 0);
        if (!applies || dc_namePtr == 0) { walk_vp = dc_next; continue; }

        uint8_t* child_slot = pool_resolve(&ctx->pool, dc_childPtr);
        if (!child_slot) { walk_vp = dc_next; continue; }

        int16_t child_type = vfs_rd2_s(child_slot, DIRNODE_OFF_TYPE, ctx->page_size);

        if (child_type == (int16_t)NODE_TYPE_DIR) {
            /* Recurse into subdirectory */
            int err = commit_scan_dir(ctx, dc_childPtr, s_epoch);
            if (err != 0) return err;

        } else if (child_type == (int16_t)NODE_TYPE_FILE) {
            /* Compute file size by walking FileNode's sizePtr chain */
            int64_t fsize = 0;
            int64_t sizePtr = vfs_rd8_s(child_slot, FILENODE_OFF_SIZEPTR, ctx->page_size);
            int64_t fs_walk = sizePtr;
            while (fs_walk != 0) {
                uint8_t* fs_slot = pool_resolve(&ctx->pool, fs_walk);
                if (!fs_slot) break;
                uint32_t fs_epoch;
                int64_t fs_modified, fs_size, fs_next;
                nodes_read_filesize(fs_slot, &fs_epoch, &fs_modified, &fs_size, &fs_next, ctx->page_size);
                (void)fs_modified;
                if (fs_epoch == s_epoch || (fs_epoch < s_epoch && fs_epoch % 2 == 0))
                    fsize = fs_size;
                fs_walk = fs_next;
            }
            int64_t num_pages = (fsize + ctx->page_size - 1) / ctx->page_size;
            if (num_pages < 1) num_pages = 1;

            for (int64_t lp = 0; lp < num_pages; lp++) {
                uint8_t* pn_slot = tree_resolve_page(ctx, dc_childPtr, lp, 0);
                if (!pn_slot) break;

                int64_t vp = vfs_atomic_load_i64(
                    (const int64_t*)(pn_slot + PAGENODE_OFF_VERSIONROOT));
                int has_snapshot = 0;
                int has_live = 0;

                while (vp != 0) {
                    uint8_t* vp_slot = pool_resolve(&ctx->pool, vp);
                    if (!vp_slot) break;
                    uint32_t v_epoch;
                    int64_t v_dataPage, v_next;
                    nodes_read_versionpage(vp_slot, &v_epoch, &v_dataPage,
                                           &v_next, ctx->page_size);
                    (void)v_dataPage;

                    if (v_epoch == s_epoch) has_snapshot = 1;
                    if (v_epoch > s_epoch && v_epoch % 2 == 0) has_live = 1;
                    vp = v_next;
                }
                if (has_snapshot && has_live) return VFS_ERR_CONFLICT;
            }
        }

        walk_vp = dc_next;
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

    /* Conflict detection: walk the TouchedFile chain directly (no fixed buffer).
       For each file modified in this snapshot epoch, scan its version chains
       looking for conflicts at even epochs > snapshot_epoch. */
    int64_t tf_vp = ctx->touchedFilesPtr;
    while (tf_vp != 0) {
        uint8_t* tf_slot = pool_resolve(&ctx->pool, tf_vp);
        if (!tf_slot) break;
        uint32_t tf_epoch, tf_nodeId;
        int64_t tf_next;
        nodes_read_touchedfile(tf_slot, &tf_epoch, &tf_nodeId, &tf_next, ctx->page_size);

        if (tf_epoch == s_epoch) {
            /* Get the file's VirtualPtr from the root DirContent chain.
               commit_scan_dir handles recursive subdirectory walks. */
            int err = commit_scan_dir(ctx, ctx->rootNodeOffset, s_epoch);
            if (err != 0) return err;
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

    /* Drop TouchedFile chain for this epoch */
    touchedfile_drop(&ctx->pool, &ctx->touchedFilesPtr, (uint32_t)snapshot_epoch);

    /* Flush superblock */
    ret = tree_superblock_write(ctx);

    return ret;
}
