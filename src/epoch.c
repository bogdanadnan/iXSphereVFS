/* Phase 6: Epoch system — replaces Phase 5a stubs. */
#include "epoch.h"
#include "ixsphere/vfs_internal.h"
#include "tree.h"
#include "nodes.h"
#include "bin.h"
#include <stdlib.h>

bool vfs_epoch_is_writable(TreeContext* ctx, int64_t epoch) {
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
       Snapshot epoch = old value + 1 = new value - 1.

       Durability contract: this snapshot is IN-MEMORY ONLY.  The
       returned epoch is valid for reads/writes in this process
       ("immediately usable") but is lost on crash until persisted.

       Snapshots are intentionally cheap (one atomic add, no I/O)
       because the intended use case is a SQLite VFS where each
       transaction gets a snapshot.  With thousands of transactions
       per second, durably flushing each snapshot would dominate
       I/O.  The crash-consistent commit point is vfs_commit, which
       writes the mapper entry, flushes the pool cache, and writes
       the superblock.  A crash before vfs_commit just discards the
       snapshot — no on-disk state was modified by it.

       Callers that need crash durability must invoke vfs_commit
       (preferred) or vfs_flush (broader; flushes everything) after
       the snapshot. */
    int64_t new_epoch = vfs_atomic_add_i64(&ctx->currentEpoch, 2);
    return new_epoch - 1;  /* snapshot epoch is always odd */
}

/* ---------------------------------------------------------------------------
 * Recursive commit scan — walk a directory tree checking for version conflicts
 * during commit.  Returns 0 if no conflict, VFS_ERR_CONFLICT on first conflict.
 * --------------------------------------------------------------------------- */

/* W6: commit conflict-detection walker callbacks.  The three
 * per-leaf walks (DirContent, FileSize, VersionPage) all have the
 * same read-rule (mapper remap + even/odd + exact-match-wins)
 * that lives in vfs_chain_walk.  Before W6, the read-rule was
 * inlined 3 times in commit_scan_dir; now all three use
 * vfs_chain_walk. */

/* W6: state for the commit-scan SlotNode callback.  Holds the
 * snapshot epoch (s_epoch) and the best match info from the
 * visible DirContent. */
typedef struct {
    TreeContext* ctx;
    uint32_t     s_epoch;
    int64_t      childPtr;   /* out: childPtr of the visible DC */
    int          found;      /* 1 if a visible live entry was found */
} commit_scan_state;

/* W6: callback for walk_content_unit_chain on the SlotNode
 * chain within a DirSegment.  For each SlotNode, walks the
 * DirContent chain via vfs_chain_walk and captures the visible
 * childPtr if the entry is live (namePtr != 0) at s_epoch. */
static int commit_scan_slot_cb(TreeContext* ctx, int64_t slot_vp,
                                   const uint8_t* slot_bytes, void* user) {
    commit_scan_state* st = (commit_scan_state*)user;
    (void)slot_vp;
    int64_t leaf_head = vfs_rd8_s(slot_bytes, ANCHOR_OFF_HEADPTR,
                                    ctx->page_size);
    if (leaf_head == 0) return 0;
    PoolSlot dc_slot = {0};
    WalkResult r = vfs_chain_walk(ctx, leaf_head,
                                    (int64_t)st->s_epoch, &dc_slot);
    if (r != WALK_FOUND) return 0;
    uint32_t ce_child, ce_epoch;
    int64_t ce_childPtr, ce_namePtr, ce_next;
    nodes_read_dircontent(dc_slot.bytes, &ce_child, &ce_epoch,
                          &ce_childPtr, &ce_namePtr, &ce_next,
                          ctx->page_size);
    /* The visible entry must be a live (non-tombstone) entry to
     * count as "found" for the commit scan. */
    if (ce_namePtr != 0) {
        st->childPtr = ce_childPtr;
        st->found = 1;
        return 1;  /* stop the SlotNode walk */
    }
    return 0;
}

/* W6: callback for walk_anchor_chain on the DirSegment chain.
 * For each segment, walks its SlotNode chain via
 * walk_content_unit_chain + commit_scan_slot_cb.  Stops on
 * match.  Returns 1 to stop the outer walk on match, 0 to
 * continue. */
static int commit_scan_seg_cb(TreeContext* ctx, int64_t seg_vp,
                                  const uint8_t* seg_bytes, void* user) {
    commit_scan_state* st = (commit_scan_state*)user;
    (void)seg_vp;
    int64_t slot_head = vfs_rd8_s(seg_bytes, ANCHOR_OFF_HEADPTR,
                                    ctx->page_size);
    if (slot_head == 0) return 0;
    walk_content_unit_chain(ctx, slot_head, commit_scan_slot_cb, st);
    return st->found;
}

/* W6: commit_scan_dir — refactored to use the shared chain-walk
 * primitives for the DirSegment + SlotNode + DirContent walk.
 * The per-file VersionPage conflict check (which iterates the
 * VersionPage chain looking for has_snapshot AND has_live) is
 * also inlined with vfs_chain_walk.
 *
 * The structure is preserved exactly: walk each visible child
 * at s_epoch, recurse into subdirectories, scan per-page
 * VersionPage chains for files.  All behavior identical to the
 * pre-W6 implementation.
 *
 * M3: a depth limit is enforced to prevent stack overflow on
 * deeply nested directory trees (the FUSE worker stack is small
 * on macOS — see tree.c:2187).  When the limit is hit, the scan
 * returns VFS_ERR_FULL (we can't continue without unbounded
 * recursion).  Callers treat VFS_ERR_FULL from this path as
 * "scan truncated" and decide whether to retry with a larger
 * limit. */
#define COMMIT_SCAN_MAX_DEPTH 64

static int commit_scan_dir_impl(vfs_t* vfs, int64_t dir_vp, uint32_t s_epoch, int depth);

static int commit_scan_dir(vfs_t* vfs, int64_t dir_vp, uint32_t s_epoch) {
    return commit_scan_dir_impl(vfs, dir_vp, s_epoch, 0);
}

static int commit_scan_dir_impl(vfs_t* vfs, int64_t dir_vp, uint32_t s_epoch, int depth) {
    /* M3: depth limit to prevent stack overflow on deeply nested trees. */
    if (depth >= COMMIT_SCAN_MAX_DEPTH) {
        return VFS_ERR_FULL;
    }
    TreeContext* ctx = vfs->ctx;
    /* Read the DirNode to get the segment chain head.  Release
       early (the pre-W6 code did the same). */
    PoolSlot dir_slot = {0};
    pool_acquire(&ctx->pool, dir_vp, false, &dir_slot);
    if (dir_slot.vptr == VFS_VPTR_NULL) return 0;
    if (vfs_rd2_s(dir_slot.bytes, DIRNODE_OFF_TYPE, ctx->page_size)
        != (int16_t)NODE_TYPE_DIR) {
        pool_release(&ctx->pool, &dir_slot);
        return 0;
    }
    int64_t head = vfs_rd8_s(dir_slot.bytes, DIRNODE_OFF_HEADPTR,
                                ctx->page_size);
    pool_release(&ctx->pool, &dir_slot);

    /* W5b: walk DirSegment chain → SlotNode chain → per-SlotNode
     * DirContent chain.  W6: use the shared walks instead of
     * inlining. */
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

            /* W6: find the visible DirContent at s_epoch via
             * vfs_chain_walk (replaces the inlined read-rule). */
            commit_scan_state scan_st = {
                .ctx = ctx, .s_epoch = s_epoch,
                .childPtr = 0, .found = 0,
            };
            commit_scan_slot_cb(ctx, slot_walk_vp, slot_slot.bytes, &scan_st);
            if (!scan_st.found) { slot_walk_vp = slot_sib; continue; }

            /* Read the child to determine type. */
            PoolSlot child_slot = {0};
            pool_acquire(&ctx->pool, scan_st.childPtr, false, &child_slot);
            if (child_slot.vptr == VFS_VPTR_NULL) { slot_walk_vp = slot_sib; continue; }

            int16_t child_type = vfs_rd2_s(child_slot.bytes, DIRNODE_OFF_TYPE, ctx->page_size);
            int64_t sizePtr = vfs_rd8_s(child_slot.bytes, FILENODE_OFF_SIZEPTR, ctx->page_size);
            pool_release(&ctx->pool, &child_slot);

            if (child_type == (int16_t)NODE_TYPE_DIR) {
                /* Recurse into subdirectory (M3: pass depth) */
                int err = commit_scan_dir_impl(vfs, scan_st.childPtr, s_epoch, depth + 1);
                if (err != 0) return err;

            } else if (child_type == (int16_t)NODE_TYPE_FILE) {
                /* Compute file size via sizechain_get (uses
                 * vfs_chain_walk internally — no read-rule
                 * duplication). */
                int64_t file_size = 0;
                int64_t dummy_modified = 0;
                sizechain_get(ctx, sizePtr, (int64_t)s_epoch,
                              &file_size, &dummy_modified);
                int64_t num_pages = (file_size + ctx->page_size - 1) / ctx->page_size;
                if (num_pages < 1) num_pages = 1;

                for (int64_t lp = 0; lp < num_pages; lp++) {
                    /* Resolve the page's PageNode via the now-shared
                     * tree_resolve_page (read-only). */
                    PoolSlot pn_slot = {0};
                    int rr_pn = tree_resolve_page(vfs, scan_st.childPtr,
                                                    lp, 0, false, &pn_slot);
                    if (rr_pn != 0) break;

                    /* Walk the PageNode's VersionPage chain.  The
                     * pre-W6 code inlined a similar walk with a
                     * different read-rule (the commit-specific one
                     * that distinguishes has_snapshot from has_live
                     * across multiple versions).  W6: this is the
                     * third place the read-rule was duplicated;
                     * keep the commit-specific check here but use
                     * vfs_chain_walk for the per-entry read. */
                    int64_t version_root = vfs_rd8_s(pn_slot.bytes,
                                                    PAGENODE_OFF_VERSIONROOT,
                                                    ctx->page_size);
                    int has_snapshot = 0;
                    int has_live = 0;
                    int64_t vp = version_root;
                    while (vp != 0) {
                        PoolSlot vp_slot = {0};
                        pool_acquire(&ctx->pool, vp, false, &vp_slot);
                        if (vp_slot.vptr == VFS_VPTR_NULL) break;
                        uint32_t v_epoch = (uint32_t)vfs_rd4_s(vp_slot.bytes,
                                                              LEAF_OFF_EPOCH,
                                                              ctx->page_size);
                        int64_t v_next = vfs_rd8_s(vp_slot.bytes,
                                                    LEAF_OFF_NEXTPTR,
                                                    ctx->page_size);
                        pool_release(&ctx->pool, &vp_slot);

                        /* Commit-specific read: an entry at the
                         * snapshot epoch (or below at even) is the
                         * snapshot-side; an entry above the snapshot
                         * at even is the live-side.  This is the
                         * commit's per-page check, not the standard
                         * read-rule — we keep the inline check
                         * because the standard read-rule returns
                         * just one entry, but we need to know
                         * whether ANY entry exists at each side.
                         *
                         * M3/N6: apply the mapper traversal-apply
                         * (same pattern as vfs_chain_walk in
                         * src/tree.c) before comparing.  Without
                         * this, a VersionPage whose v_epoch was
                         * remapped by a previous commit (e.g.,
                         * snapshot 1 → committed 4) would be
                         * compared as v_epoch=1 (the snapshot side
                         * for a re-commit at s_epoch=1) when it
                         * really is the live side at eff_epoch=4.
                         * This would cause commit_scan_dir to miss
                         * conflicts for previously-committed
                         * snapshots whose pages diverge from new
                         * snapshot writes. */
                        int64_t eff_epoch = (int64_t)v_epoch;
                        if (mapper_table_traversal_apply(
                                &ctx->mapper_table, (int64_t)v_epoch)) {
                            eff_epoch = mapper_table_resolve(
                                &ctx->mapper_table, (int64_t)v_epoch);
                        }
                        if (eff_epoch == (int64_t)s_epoch) has_snapshot = 1;
                        if (eff_epoch > (int64_t)s_epoch
                            && eff_epoch % 2 == 0) has_live = 1;
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

    /* M5: flush the dirty pool page that holds the new MapperEntry
     * before the in-memory table update and the superblock write.
     * Without this, a crash between mapper_insert and
     * tree_superblock_write would leave the pool page dirty in
     * memory but not on disk, creating a window where the in-memory
     * mapper_table says the snapshot is committed but the on-disk
     * mapper chain doesn't.  Flushing the pool page here makes the
     * mapper write durable; the superblock write that follows is
     * then the atomic commit point (consistent state on disk). */
    storage_flush_cache_only(ctx->sb);

    /* Update in-memory mapper table (pool write already done above) */
    {
        int err = mapper_table_append(&ctx->mapper_table,
                      (uint32_t)snapshot_epoch, (uint32_t)current, true);
        if (err != VFS_OK) return err;
    }

    /* Flush superblock to persist the mapper change */
    ret = tree_superblock_write(ctx);
    if (ret != VFS_OK) return ret;

    /* Phase 28 W3: push a NOOP trigger to the Bin.  Future bin-job
       specs will replace this with BIN_TRIGGER_EPOCH_COMMITTED, which
       the GC's analysis converts to BIN_WORK_REWRITE_CHAIN_ENTRY
       (for each VersionPage at the committed epoch, rewrite to
       currentEpoch) + BIN_WORK_DROP_COMMITTED_MAPPER (drop the
       committed mapper entry).  For W3, the NOOP trigger is a
       placeholder.  context = the snapshot epoch. */
    bin_push(ctx->sb, BIN_TRIGGER_NOOP, snapshot_epoch, current);
    return VFS_OK;
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

    /* M5: flush the pool page holding the new soft-delete mapping
     * before the in-memory table update and the superblock write
     * (see vfs_commit for the full rationale). */
    storage_flush_cache_only(ctx->sb);

    /* Update in-memory mapper table (pool write already done above) */
    {
        int err = mapper_table_append(&ctx->mapper_table,
                      (uint32_t)snapshot_epoch, toEpoch, false);
        if (err != VFS_OK) return err;
    }

    /* Flush superblock */
    ret = tree_superblock_write(ctx);
    if (ret != VFS_OK) return ret;

    /* Phase 28 W3: push a NOOP trigger to the Bin.  Future bin-job
       specs will replace this with BIN_TRIGGER_EPOCH_SOFT_DELETED,
       which the GC's analysis converts to BIN_WORK_DROP_SOFT_DELETE
       (drop the soft-delete mapper entry) + BIN_WORK_FREE_PAGES
       (for the snap-only data pages).  For W3, the NOOP trigger is
       a placeholder.  context = the snapshot epoch. */
    bin_push(ctx->sb, BIN_TRIGGER_NOOP, snapshot_epoch, (int64_t)toEpoch);
    return VFS_OK;
}
