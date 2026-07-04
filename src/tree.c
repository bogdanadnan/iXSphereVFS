/* Phase 5: Tree Operations — Bootstrap, Init, Superblock I/O */
#include "tree.h"
#include "page_array.h"
#include "touched.h"
#include "gc.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------------------
 * Superblock I/O helpers
 * --------------------------------------------------------------------------- */

int tree_superblock_read(TreeContext* ctx) {
    uint8_t* payload = storage_read(ctx->sb, SUPERBLOCK_PAGE);
    if (!payload) return VFS_ERR_IO;

    ctx->rootNodeOffset   = vfs_rd8_s(payload, SB_OFF_ROOT_OFFSET, ctx->page_size);
    ctx->currentEpoch     = vfs_rd8_s(payload, SB_OFF_CURRENT_EPOCH, ctx->page_size);
    ctx->epochMapperPtr   = vfs_rd8_s(payload, SB_OFF_EPOCH_MAPPER_PTR, ctx->page_size);
    ctx->treeLockState    = vfs_rd8_s(payload, SB_OFF_TREE_LOCK_STATE, ctx->page_size);
    ctx->nextNodeId       = (uint32_t)vfs_rd4_s(payload, SB_OFF_NEXT_NODE_ID, ctx->page_size);
    ctx->touchedFilesPtr  = vfs_rd8_s(payload, SB_OFF_TOUCHED_FILES_PTR, ctx->page_size);

    /* poolListHead — wire into pool allocator */
    int64_t pool_list_head = vfs_rd8_s(payload, SB_OFF_POOL_LIST_HEAD, ctx->page_size);
    if (ctx->pool.list_head) *ctx->pool.list_head = pool_list_head;

    return VFS_OK;
}

int tree_superblock_write(TreeContext* ctx) {
    int64_t ps = ctx->sb->page_size;
    uint8_t* buf = (uint8_t*)malloc((size_t)ps);
    if (!buf) return VFS_ERR_NOMEM;
    memset(buf, 0, (size_t)ps);

    vfs_wr8_s(buf, SB_OFF_ROOT_OFFSET,       ctx->rootNodeOffset, ctx->page_size);
    vfs_wr8_s(buf, SB_OFF_CURRENT_EPOCH,     ctx->currentEpoch, ctx->page_size);
    vfs_wr8_s(buf, SB_OFF_EPOCH_MAPPER_PTR,  ctx->epochMapperPtr, ctx->page_size);
    vfs_wr8_s(buf, SB_OFF_POOL_LIST_HEAD,    ctx->pool.list_head ? *ctx->pool.list_head : 0, ctx->page_size);
    vfs_wr8_s(buf, SB_OFF_TREE_LOCK_STATE,   ctx->treeLockState, ctx->page_size);
    vfs_wr4_s(buf, SB_OFF_NEXT_NODE_ID,      (int32_t)ctx->nextNodeId, ctx->page_size);
    vfs_wr8_s(buf, SB_OFF_TOUCHED_FILES_PTR, ctx->touchedFilesPtr, ctx->page_size);

    storage_write(ctx->sb, SUPERBLOCK_PAGE, buf, 3);
    storage_flush(ctx->sb, -1);
    free(buf);
    return VFS_OK;
}

/* ---------------------------------------------------------------------------
 * Bootstrap — initialize superblock on a fresh file
 * --------------------------------------------------------------------------- */

int tree_bootstrap_superblock(TreeContext* ctx) {
    /* Try reading the superblock — if a valid root exists, this is a reopen. */
    if (tree_superblock_read(ctx) == VFS_OK && ctx->rootNodeOffset != 0) {
        return tree_init(ctx);
    }

    /* Prepare superblock payload in-memory */
    ctx->rootNodeOffset   = 0;
    ctx->currentEpoch     = 0;
    ctx->epochMapperPtr   = 0;
    ctx->touchedFilesPtr  = 0;
    ctx->nextNodeId       = 0;  /* first vfs_atomic_add_i32 returns 1 */
    ctx->treeLockState    = 0;

    /* Write superblock with initial state */
    int err = tree_superblock_write(ctx);
    if (err != VFS_OK) return err;

    /* Read segment_size from StorageBackend header page */
    uint8_t* hdr = storage_read(ctx->sb, 0);
    if (hdr) {
        ctx->segment_size = (uint32_t)vfs_rd4_s(hdr, HDR_OFF_SEGMENT_SIZE, ctx->page_size);
    } else {
        ctx->segment_size = 1024;  /* default */
    }

    /* Allocate pool already exists in ctx->pool from vfs_open */
    int64_t root_vp = pool_alloc(&ctx->pool);
    if (root_vp == VFS_VPTR_NULL) return VFS_ERR_FULL;

    uint8_t* root_slot = pool_resolve(&ctx->pool, root_vp);
    if (!root_slot) return VFS_ERR_IO;

    /* Write root DirNode: nodeId=0, no children */
    nodes_write_dirnode(root_slot, 0, 0, ctx->page_size);

    /* Update superblock with root pointer */
    ctx->rootNodeOffset = root_vp;
    return tree_superblock_write(ctx);
}

/* ---------------------------------------------------------------------------
 * Init — re-initialize from an existing file
 * --------------------------------------------------------------------------- */

int tree_init(TreeContext* ctx) {
    int err = tree_superblock_read(ctx);
    if (err != VFS_OK) return err;

    /* Verify root DirNode */
    if (ctx->rootNodeOffset == 0) return VFS_ERR_IO;

    uint8_t* root_slot = pool_resolve(&ctx->pool, ctx->rootNodeOffset);
    if (!root_slot) return VFS_ERR_IO;

    /* Verify type is DirNode (0x01) */
    int16_t type = vfs_rd2_s(root_slot, DIRNODE_OFF_TYPE, ctx->page_size);
    if (type != (int16_t)NODE_TYPE_DIR) return VFS_ERR_IO;

    /* Crash recovery: if exclusive lock (bit 63) was held at crash time,
       zero the field so the lock is released on mount.  The GC operation
       that was in progress may be incomplete, but all data is consistent
       because GC uses shadow-compaction (writes to new pages, frees old
       ones only after commit).  An incomplete GC simply leaves some
       unreclaimed pages. */
    if (ctx->treeLockState & (int64_t)TREE_LOCK_EXCLUSIVE_BIT) {
#ifndef NDEBUG
        fprintf(stderr, "vfs: tree_init: stale exclusive lock detected "
                "(treeLockState=0x%016llx), clearing\n",
                (unsigned long long)ctx->treeLockState);
#endif
        ctx->treeLockState = 0;
    }

    /* Read segment_size from StorageBackend header */
    uint8_t* hdr = storage_read(ctx->sb, 0);
    if (hdr) {
        ctx->segment_size = (uint32_t)vfs_rd4_s(hdr, HDR_OFF_SEGMENT_SIZE, ctx->page_size);
    } else {
        ctx->segment_size = 1024;
    }

    /* Walk epoch mapper chain (stub — just validate chain is readable).
       Full mapper resolution is implemented in Phase 6. */
    int64_t mapper_vp = ctx->epochMapperPtr;
    while (mapper_vp != 0) {
        uint8_t* slot = pool_resolve(&ctx->pool, mapper_vp);
        if (!slot) return VFS_ERR_IO;
        uint32_t fromEpoch, toEpoch;
        uint16_t flags;
        int64_t next;
        nodes_read_mapperentry(slot, &fromEpoch, &toEpoch, &flags, &next, ctx->page_size);
        (void)fromEpoch; (void)toEpoch; (void)flags;
        mapper_vp = next;
    }

    return VFS_OK;
}

/* ---------------------------------------------------------------------------
 * Page Resolution — resolve a logical page to its PageNode
 *
 * Walks the FileContent chain to find the segment containing this page.
 * Creates missing FileContent + PageNode entries on file growth.
 * Builds the in-memory VirtualPtr array on first access to a segment.
 *
 * Returns a pointer to the PageNode slot (via pool_resolve), or NULL on error.
 * --------------------------------------------------------------------------- */

uint8_t* tree_resolve_page(TreeContext* ctx, int64_t file_vp,
                           int64_t logical_page, int64_t epoch) {
    (void)epoch;  /* not yet used — future: segment growth decisions */

    uint32_t seg_size = ctx->segment_size;
    int64_t segment_idx = logical_page / seg_size;
    int64_t page_in_segment = logical_page % seg_size;

    /* Read FileNode to get headPtr (first FileContent) */
    uint8_t* file_slot = pool_resolve(&ctx->pool, file_vp);
    if (!file_slot) return NULL;

    int64_t fc_vp;   /* VirtualPtr to current FileContent */

    uint32_t tmp_nodeId;
    int64_t tmp_headPtr, tmp_sizePtr, tmp_createdAt;

    nodes_read_filenode(file_slot, &tmp_nodeId, &tmp_headPtr, &tmp_sizePtr, &tmp_createdAt, ctx->page_size);
    fc_vp = tmp_headPtr;

    /* Walk FileContent chain to find the target segment */
    int64_t prev_fc_vp = 0;  /* previous FileContent's VirtualPtr, for linking */

    for (int64_t i = 0; i <= segment_idx; i++) {
        if (fc_vp == VFS_VPTR_NULL) {
            /* Segment doesn't exist yet — file growth:
               allocate new FileContent + all PageNodes */
            int64_t page_root_vp = VFS_VPTR_NULL;
            int64_t prev_pn_vp = 0;

            /* Allocate PageNodes in reverse order (last→first) */
            for (int64_t p = seg_size - 1; p >= 0; p--) {
                int64_t pn_vp = pool_alloc(&ctx->pool);
                if (pn_vp == VFS_VPTR_NULL) return NULL;
                uint8_t* pn_slot = pool_resolve(&ctx->pool, pn_vp);
                if (!pn_slot) return NULL;
                nodes_write_pagenode(pn_slot, 0, prev_pn_vp, ctx->page_size);
                prev_pn_vp = pn_vp;
                if (p == 0) page_root_vp = pn_vp;
            }

            /* Allocate FileContent entry */
            int64_t new_fc_vp = pool_alloc(&ctx->pool);
            if (new_fc_vp == VFS_VPTR_NULL) return NULL;
            uint8_t* fc_slot = pool_resolve(&ctx->pool, new_fc_vp);
            if (!fc_slot) return NULL;
            nodes_write_filecontent(fc_slot, page_root_vp, 0, ctx->page_size);

            /* CAS-link into chain with release barrier */
            vfs_mb_release();
            if (i == 0) {
                /* First segment — CAS into FileNode headPtr */
                int64_t expected = 0;
                int64_t desired = new_fc_vp;
                int64_t old = vfs_cas_i64(
                    (int64_t*)(file_slot + FILENODE_OFF_HEADPTR),
                    expected, desired);
                if (old != expected) {
                    /* CAS failed — another thread already set headPtr.
                       Our orphaned FileContent+PageNodes will be GC'd.
                       Fall through to walk the existing chain. */
                    fc_vp = old;
                    i--;  /* retry this segment */
                    continue;
                }
                fc_vp = new_fc_vp;
            } else {
                /* Subsequent segment — CAS into previous FC's nextPtr.
                   Expected value is 0 (nextPtr not yet set). */
                uint8_t* prev_slot = pool_resolve(&ctx->pool, prev_fc_vp);
                if (prev_slot) {
                    int64_t expected = 0;
                    int64_t desired = new_fc_vp;
                    int64_t off = FILECONTENT_OFF_NEXTPTR;
                    int64_t old = vfs_cas_i64(
                        (int64_t*)(prev_slot + off), expected, desired);
                    if (old != expected) {
                        /* CAS failed — another thread linked a segment.
                           Our orphaned slots will be GC'd.
                           Walk the existing chain starting from old value. */
                        fc_vp = old;
                        i--;  /* retry this segment */
                        continue;
                    }
                }
                fc_vp = new_fc_vp;
            }
        }

        uint8_t* fc_slot = pool_resolve(&ctx->pool, fc_vp);
        if (!fc_slot) return NULL;

        if (i == segment_idx) {
            /* Build in-memory page array if not cached for this segment */
            if (ctx->seg_array_fc_vp != fc_vp) {
                /* Destroy old cache if any */
                if (ctx->seg_array_fc_vp != 0)
                    segment_array_destroy(&ctx->seg_array_cache);

                int64_t fc_page_root = vfs_rd8_s(fc_slot, FILECONTENT_OFF_ROOTPTR, ctx->page_size);
                int err = segment_array_build(&ctx->pool, fc_page_root,
                                               seg_size, &ctx->seg_array_cache);
                if (err != VFS_OK) {
                    ctx->seg_array_fc_vp = 0;
                    return NULL;
                }
                ctx->seg_array_fc_vp = fc_vp;
            }

            /* Resolve the specific page via the array */
            return segment_array_resolve(&ctx->pool, &ctx->seg_array_cache,
                                         (uint32_t)page_in_segment);
        }

        prev_fc_vp = fc_vp;
        fc_vp = vfs_rd8_s(fc_slot, FILECONTENT_OFF_NEXTPTR, ctx->page_size);
    }

    return NULL;
}

/* ---------------------------------------------------------------------------
 * vfs_create — create a file under a parent directory
 *
 * Returns new nodeId on success, or negative vfs_error_t on failure.
 * --------------------------------------------------------------------------- */

int vfs_create(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch) {
    if (!vfs || !vfs->ctx || !name || name[0] == '\0') return VFS_ERR_IO;
    TreeContext* ctx = vfs->ctx;

    /* Validate epoch is writable (Phase 6: uses real epoch validation) */
    if (!vfs_epoch_is_writable(ctx, (int64_t)epoch)) return VFS_ERR_IO;

    /* Read parent DirNode, verify type */
    uint8_t* parent_slot = pool_resolve(&ctx->pool, (int64_t)parent);

    if (!parent_slot) { vfs->ctx->last_error = VFS_ERR_NOTFOUND; return VFS_ERR_NOTFOUND; }
    if (vfs_rd2_s(parent_slot, DIRNODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_DIR) {
        vfs->ctx->last_error = VFS_ERR_NOTDIR;
        return VFS_ERR_NOTDIR;
        }

    /* Walk parent's DirContent chain, checking for name collision */
    int64_t headPtr = vfs_rd8_s(parent_slot, DIRNODE_OFF_HEADPTR, ctx->page_size);
    int64_t walk_vp = headPtr;
    while (walk_vp != 0) {
        uint8_t* dc_slot = pool_resolve(&ctx->pool, walk_vp);
        if (!dc_slot) break;
        uint32_t ce_child, ce_epoch;
        int64_t ce_childPtr, ce_namePtr, ce_next;
        nodes_read_dircontent(dc_slot, &ce_child, &ce_epoch, &ce_childPtr,
                              &ce_namePtr, &ce_next, ctx->page_size);
        (void)ce_child; (void)ce_childPtr;
        if (ce_epoch == (uint32_t)epoch && ce_namePtr != 0) {
            /* Read the name and compare */
            char entry_name[256];
            int name_len = nodes_read_name(&ctx->pool, ce_namePtr,
                                            entry_name, (int)sizeof(entry_name));
            if (name_len > 0 && strcmp(entry_name, name) == 0) {
                vfs->ctx->last_error = VFS_ERR_EXISTS;
                return VFS_ERR_EXISTS;
                }
        }
        walk_vp = ce_next;
    }

    /* Atomically increment nextNodeId */
    uint32_t new_nodeId = (uint32_t)vfs_atomic_add_i32((int32_t*)&ctx->nextNodeId, 1);
    /* nextNodeId starts at 0, first add yields nodeId=1 */
    if (vfs_lock(vfs, (int64_t)new_nodeId, epoch) != VFS_OK) return VFS_ERR_IO;

    /* Allocate FileNode slot and write it */
    int64_t file_vp = pool_alloc(&ctx->pool);

    if (file_vp == VFS_VPTR_NULL) { vfs_unlock(vfs, (int64_t)new_nodeId, epoch); vfs->ctx->last_error = VFS_ERR_FULL; return VFS_ERR_FULL; }
    uint8_t* file_slot = pool_resolve(&ctx->pool, file_vp);

    if (!file_slot) { vfs_unlock(vfs, (int64_t)new_nodeId, epoch); vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }
    nodes_write_filenode(file_slot, new_nodeId, 0, 0, (int64_t)time(NULL), ctx->page_size);

    /* Allocate NameEntry chain for the file name */
    int64_t name_vp;
    int name_slots = nodes_write_name(&ctx->pool, name, &name_vp);

    if (name_slots == 0) { vfs_unlock(vfs, (int64_t)new_nodeId, epoch); vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }

    /* Allocate DirContent slot outside the CAS loop to avoid leaks on retry */
    int64_t dc_vp = pool_alloc(&ctx->pool);

    if (dc_vp == VFS_VPTR_NULL) { vfs_unlock(vfs, (int64_t)new_nodeId, epoch); vfs->ctx->last_error = VFS_ERR_FULL; return VFS_ERR_FULL; }
    uint8_t* dc_slot = pool_resolve(&ctx->pool, dc_vp);

    if (!dc_slot) { vfs_unlock(vfs, (int64_t)new_nodeId, epoch); vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }

    /* CAS-prepend DirContent to parent's headPtr */
    int64_t old_head;
    do {
        old_head = vfs_atomic_load_i64(
            (const int64_t*)(parent_slot + DIRNODE_OFF_HEADPTR));

        nodes_write_dircontent(dc_slot, new_nodeId, (uint32_t)epoch,
                               file_vp, name_vp, old_head, ctx->page_size);
        vfs_mb_release();
    } while (vfs_cas_i64((int64_t*)(parent_slot + DIRNODE_OFF_HEADPTR),
                         old_head, dc_vp) != old_head);

    dentry_cache_invalidate(&ctx->readdir_cache);
    vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
    return (int)new_nodeId;
}

int vfs_mkdir(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch) {
    if (!vfs || !vfs->ctx || !name || name[0] == '\0') return VFS_ERR_IO;
    TreeContext* ctx = vfs->ctx;

    if (!vfs_epoch_is_writable(ctx, (int64_t)epoch)) return VFS_ERR_IO;

    uint8_t* parent_slot = pool_resolve(&ctx->pool, (int64_t)parent);

    if (!parent_slot) { vfs->ctx->last_error = VFS_ERR_NOTFOUND; return VFS_ERR_NOTFOUND; }
    if (vfs_rd2_s(parent_slot, DIRNODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_DIR) {
        vfs->ctx->last_error = VFS_ERR_NOTDIR;
        return VFS_ERR_NOTDIR;
        }

    /* Walk DirContent chain, check for name collision */
    int64_t headPtr = vfs_rd8_s(parent_slot, DIRNODE_OFF_HEADPTR, ctx->page_size);
    int64_t walk_vp = headPtr;
    while (walk_vp != 0) {
        uint8_t* dc_slot = pool_resolve(&ctx->pool, walk_vp);
        if (!dc_slot) break;
        uint32_t cc, ce;
        int64_t cp, np, nx;
        nodes_read_dircontent(dc_slot, &cc, &ce, &cp, &np, &nx, ctx->page_size);
        (void)cc; (void)cp;
        if (ce == (uint32_t)epoch && np != 0) {
            char entry_name[256];
            int nl = nodes_read_name(&ctx->pool, np, entry_name,
                                     (int)sizeof(entry_name));
            if (nl > 0 && strcmp(entry_name, name) == 0) {
                vfs->ctx->last_error = VFS_ERR_EXISTS;
                return VFS_ERR_EXISTS;
                }
        }
        walk_vp = nx;
    }

    uint32_t new_nodeId = (uint32_t)vfs_atomic_add_i32(
        (int32_t*)&ctx->nextNodeId, 1);
    if (vfs_lock(vfs, (int64_t)new_nodeId, epoch) != VFS_OK) return VFS_ERR_IO;

    int64_t dir_vp = pool_alloc(&ctx->pool);

    if (dir_vp == VFS_VPTR_NULL) { vfs_unlock(vfs, (int64_t)new_nodeId, epoch); vfs->ctx->last_error = VFS_ERR_FULL; return VFS_ERR_FULL; }
    uint8_t* dir_slot = pool_resolve(&ctx->pool, dir_vp);

    if (!dir_slot) { vfs_unlock(vfs, (int64_t)new_nodeId, epoch); vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }
    nodes_write_dirnode(dir_slot, new_nodeId, 0, ctx->page_size);

    int64_t name_vp;
    int name_slots = nodes_write_name(&ctx->pool, name, &name_vp);

    if (name_slots == 0) { vfs_unlock(vfs, (int64_t)new_nodeId, epoch); vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }

    int64_t dc_vp = pool_alloc(&ctx->pool);

    if (dc_vp == VFS_VPTR_NULL) { vfs_unlock(vfs, (int64_t)new_nodeId, epoch); vfs->ctx->last_error = VFS_ERR_FULL; return VFS_ERR_FULL; }
    uint8_t* dc_slot = pool_resolve(&ctx->pool, dc_vp);

    if (!dc_slot) { vfs_unlock(vfs, (int64_t)new_nodeId, epoch); vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }

    int64_t old_head;
    do {
        old_head = vfs_atomic_load_i64(
            (const int64_t*)(parent_slot + DIRNODE_OFF_HEADPTR));
        nodes_write_dircontent(dc_slot, new_nodeId, (uint32_t)epoch,
                               dir_vp, name_vp, old_head, ctx->page_size);
        vfs_mb_release();
    } while (vfs_cas_i64((int64_t*)(parent_slot + DIRNODE_OFF_HEADPTR),
                         old_head, dc_vp) != old_head);

    dentry_cache_invalidate(&ctx->readdir_cache);
    vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
    return VFS_OK;
}

/* ---------------------------------------------------------------------------
 * vfs_delete — delete a file by prepending a tombstone DirContent
 * --------------------------------------------------------------------------- */

int vfs_delete(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch) {
    if (!vfs || !vfs->ctx || !name || name[0] == '\0') return VFS_ERR_IO;
    TreeContext* ctx = vfs->ctx;

    if (!vfs_epoch_is_writable(ctx, (int64_t)epoch)) return VFS_ERR_IO;

    /* Read parent DirNode, verify type */
    uint8_t* parent_slot = pool_resolve(&ctx->pool, (int64_t)parent);

    if (!parent_slot) { vfs->ctx->last_error = VFS_ERR_NOTFOUND; return VFS_ERR_NOTFOUND; }
    if (vfs_rd2_s(parent_slot, DIRNODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_DIR) {
        vfs->ctx->last_error = VFS_ERR_NOTDIR;
        return VFS_ERR_NOTDIR;
        }

    /* Walk parent's DirContent chain to find the entry with matching name
       at epoch ≤ query_epoch. Chain is in descending epoch order,
       so the first match is the most recent at or before query_epoch. */
    int64_t headPtr = vfs_rd8_s(parent_slot, DIRNODE_OFF_HEADPTR, ctx->page_size);
    int64_t read_epoch = mapper_table_resolve(&ctx->mapper_table, epoch);
    int64_t found_vp = 0;
    uint32_t found_childId = 0;
    int64_t found_childPtr = 0;

    int64_t walk_vp = headPtr;
    while (walk_vp != 0 && found_vp == 0) {
        uint8_t* dc_slot = pool_resolve(&ctx->pool, walk_vp);
        if (!dc_slot) break;
        uint32_t ce_child, ce_epoch;
        int64_t ce_childPtr, ce_namePtr, ce_next;
        nodes_read_dircontent(dc_slot, &ce_child, &ce_epoch, &ce_childPtr,
                              &ce_namePtr, &ce_next, ctx->page_size);

        int64_t effective_epoch = (int64_t)ce_epoch;
        if (mapper_table_traversal_apply(&ctx->mapper_table, (int64_t)ce_epoch))
            effective_epoch = mapper_table_resolve(&ctx->mapper_table, (int64_t)ce_epoch);

        int applies = (effective_epoch == read_epoch) ||
                      (effective_epoch < read_epoch && effective_epoch % 2 == 0);

        if (ce_namePtr != 0 && applies) {
            char entry_name[256];
            int name_len = nodes_read_name(&ctx->pool, ce_namePtr,
                                            entry_name, (int)sizeof(entry_name));
            if (name_len > 0 && strcmp(entry_name, name) == 0) {
                found_vp = walk_vp;
                found_childId = ce_child;
                found_childPtr = ce_childPtr;
            }
        }
        walk_vp = ce_next;
    }


    if (found_vp == 0) { vfs->ctx->last_error = VFS_ERR_NOTFOUND; return VFS_ERR_NOTFOUND; }
    if (vfs_lock(vfs, (int64_t)found_childId, epoch) != VFS_OK) return VFS_ERR_IO;

    /* Allocate tombstone DirContent slot outside CAS loop */
    int64_t dc_vp = pool_alloc(&ctx->pool);

    if (dc_vp == VFS_VPTR_NULL) { vfs_unlock(vfs, (int64_t)found_childId, epoch); vfs->ctx->last_error = VFS_ERR_FULL; return VFS_ERR_FULL; }
    uint8_t* dc_slot = pool_resolve(&ctx->pool, dc_vp);

    if (!dc_slot) { vfs_unlock(vfs, (int64_t)found_childId, epoch); vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }

    /* CAS-prepend tombstone to parent's headPtr */
    int64_t old_head;
    do {
        old_head = vfs_atomic_load_i64(
            (const int64_t*)(parent_slot + DIRNODE_OFF_HEADPTR));

        /* Tombstone: namePtr=0 means deleted */
        nodes_write_dircontent(dc_slot, found_childId, (uint32_t)epoch,
                               found_childPtr, 0, old_head, ctx->page_size);
        vfs_mb_release();
    } while (vfs_cas_i64((int64_t*)(parent_slot + DIRNODE_OFF_HEADPTR),
                         old_head, dc_vp) != old_head);

    dentry_cache_invalidate(&ctx->readdir_cache);
    vfs_unlock(vfs, (int64_t)found_childId, epoch);
    return VFS_OK;
}

int vfs_rmdir(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch) {
    if (!vfs || !vfs->ctx || !name || name[0] == '\0') return VFS_ERR_IO;
    TreeContext* ctx = vfs->ctx;

    if (!vfs_epoch_is_writable(ctx, (int64_t)epoch)) return VFS_ERR_IO;

    uint8_t* parent_slot = pool_resolve(&ctx->pool, (int64_t)parent);

    if (!parent_slot) { vfs->ctx->last_error = VFS_ERR_NOTFOUND; return VFS_ERR_NOTFOUND; }
    if (vfs_rd2_s(parent_slot, DIRNODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_DIR) {
        vfs->ctx->last_error = VFS_ERR_NOTDIR;
        return VFS_ERR_NOTDIR;
        }

    int64_t headPtr = vfs_rd8_s(parent_slot, DIRNODE_OFF_HEADPTR, ctx->page_size);
    int64_t found_vp = 0;
    uint32_t found_childId = 0;
    int64_t found_childPtr = 0;

    int64_t walk_vp = headPtr;
    while (walk_vp != 0 && found_vp == 0) {
        uint8_t* dc_slot = pool_resolve(&ctx->pool, walk_vp);
        if (!dc_slot) break;
        uint32_t cc, ce;
        int64_t cp, np, nx;
        nodes_read_dircontent(dc_slot, &cc, &ce, &cp, &np, &nx, ctx->page_size);
        if (np != 0 && ce <= (uint32_t)epoch) {
            char en[256];
            int nl = nodes_read_name(&ctx->pool, np, en, (int)sizeof(en));
            if (nl > 0 && strcmp(en, name) == 0) {
                found_vp = walk_vp;
                found_childId = cc;
                found_childPtr = cp;
            }
        }
        walk_vp = nx;
    }

    if (found_vp == 0) { vfs->ctx->last_error = VFS_ERR_NOTFOUND; return VFS_ERR_NOTFOUND; }
    if (vfs_lock(vfs, (int64_t)found_childId, epoch) != VFS_OK) return VFS_ERR_IO;

    uint8_t* child_slot = pool_resolve(&ctx->pool, found_childPtr);

    if (!child_slot) { vfs_unlock(vfs, (int64_t)found_childId, epoch); vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }
    if (vfs_rd2_s(child_slot, DIRNODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_DIR) {
        vfs_unlock(vfs, (int64_t)found_childId, epoch);
        vfs->ctx->last_error = VFS_ERR_NOTDIR;
        return VFS_ERR_NOTDIR;
        }

    /* Check directory is empty using read-rule: for each childNodeId, find the
       entry at the highest epoch ≤ query_epoch.  If any such entry has
       namePtr ≠ 0, the directory is not empty (tombstones with namePtr=0
       indicate deleted entries). */
    int64_t child_head = vfs_rd8_s(child_slot, DIRNODE_OFF_HEADPTR, ctx->page_size);
    {
        /* Simple approach: walk chain and collect unique childNodeIds.
           For each child, track the highest epoch and whether the entry
           at that epoch has namePtr != 0. */
        #define MAX_RMDIR_CHILDREN 1024
        uint32_t child_ids[MAX_RMDIR_CHILDREN];
        uint32_t child_best_epoch[MAX_RMDIR_CHILDREN];
        int     child_has_name[MAX_RMDIR_CHILDREN];
        int child_count = 0;

        int64_t cw = child_head;
        while (cw != 0 && child_count < MAX_RMDIR_CHILDREN) {
            uint8_t* cs = pool_resolve(&ctx->pool, cw);
            if (!cs) break;
            uint32_t ccc, cce;
            int64_t ccp, cnp, cnx;
            nodes_read_dircontent(cs, &ccc, &cce, &ccp, &cnp, &cnx,
                                  ctx->page_size);
            (void)ccp;

            if (cce <= (uint32_t)epoch) {
                int found = -1;
                for (int i = 0; i < child_count; i++) {
                    if (child_ids[i] == ccc) { found = i; break; }
                }
                if (found >= 0) {
                    if (cce > child_best_epoch[found]) {
                        child_best_epoch[found] = cce;
                        child_has_name[found] = (cnp != 0) ? 1 : 0;
                    }
                } else {
                    child_ids[child_count] = ccc;
                    child_best_epoch[child_count] = cce;
                    child_has_name[child_count] = (cnp != 0) ? 1 : 0;
                    child_count++;
                }
            }
            cw = cnx;
        }

        for (int i = 0; i < child_count; i++) {
            if (child_has_name[i]) {
                vfs_unlock(vfs, (int64_t)found_childId, epoch);
                vfs->ctx->last_error = VFS_ERR_NOTEMPTY;
                return VFS_ERR_NOTEMPTY;
            }
        }
    }

    int64_t dc_vp = pool_alloc(&ctx->pool);

    if (dc_vp == VFS_VPTR_NULL) { vfs_unlock(vfs, (int64_t)found_childId, epoch); vfs->ctx->last_error = VFS_ERR_FULL; return VFS_ERR_FULL; }
    uint8_t* dc_slot = pool_resolve(&ctx->pool, dc_vp);

    if (!dc_slot) { vfs_unlock(vfs, (int64_t)found_childId, epoch); vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }

    int64_t old_head;
    do {
        old_head = vfs_atomic_load_i64(
            (const int64_t*)(parent_slot + DIRNODE_OFF_HEADPTR));
        nodes_write_dircontent(dc_slot, found_childId, (uint32_t)epoch,
                               found_childPtr, 0, old_head, ctx->page_size);
        vfs_mb_release();
    } while (vfs_cas_i64((int64_t*)(parent_slot + DIRNODE_OFF_HEADPTR),
                         old_head, dc_vp) != old_head);

    dentry_cache_invalidate(&ctx->readdir_cache);
    vfs_unlock(vfs, (int64_t)found_childId, epoch);
    return VFS_OK;
}

int vfs_readdir(vfs_t* vfs, int64_t dir, vfs_dirent_t* entries,
                int max, int64_t epoch) {
    if (!vfs || !vfs->ctx || !entries || max <= 0) return VFS_ERR_IO;
    TreeContext* ctx = vfs->ctx;

    /* Verify directory type */
    uint8_t* dir_slot = pool_resolve(&ctx->pool, (int64_t)dir);

    if (!dir_slot) { vfs->ctx->last_error = VFS_ERR_NOTFOUND; return VFS_ERR_NOTFOUND; }
    if (vfs_rd2_s(dir_slot, DIRNODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_DIR) {
        vfs->ctx->last_error = VFS_ERR_NOTDIR;
        return VFS_ERR_NOTDIR;
        }

    /* Always rebuild the cache (optimization: check valid flag later) */
    int err = dentry_cache_build(&ctx->pool, &ctx->mapper, (int64_t)dir, epoch,
                                 &ctx->readdir_cache);
    if (err != VFS_OK) return err;

    /* Copy entries from cache to caller's buffer */
    int written = 0;
    int n = ctx->readdir_cache.count;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        entries[i].nodeId = ctx->readdir_cache.entries[i].childNodeId;
        entries[i].isDir  = ctx->readdir_cache.entries[i].isDir;
        size_t nlen = strlen(ctx->readdir_cache.entries[i].name);
        if (nlen >= sizeof(entries[i].name))
            nlen = sizeof(entries[i].name) - 1;
        memcpy(entries[i].name, ctx->readdir_cache.entries[i].name, nlen);
        entries[i].name[nlen] = '\0';
        written++;
    }
    return written;
}

int vfs_rename(vfs_t* vfs, int64_t src_parent, const char* src,
               int64_t dst_parent, const char* dst, int64_t epoch) {
    if (!vfs || !vfs->ctx) return VFS_ERR_IO;
    if (!src || !dst || src[0] == '\0' || dst[0] == '\0') {
        vfs->ctx->last_error = VFS_ERR_IO;
        return VFS_ERR_IO;
    }
    TreeContext* ctx = vfs->ctx;

    if (!vfs_epoch_is_writable(ctx, (int64_t)epoch)) return VFS_ERR_IO;

    /* Verify both parents are DirNodes */
    uint8_t* src_slot = pool_resolve(&ctx->pool, (int64_t)src_parent);

    if (!src_slot) { vfs->ctx->last_error = VFS_ERR_NOTFOUND; return VFS_ERR_NOTFOUND; }
    if (vfs_rd2_s(src_slot, DIRNODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_DIR) {
        vfs->ctx->last_error = VFS_ERR_NOTDIR;
        return VFS_ERR_NOTDIR;
        }

    uint8_t* dst_slot = pool_resolve(&ctx->pool, (int64_t)dst_parent);

    if (!dst_slot) { vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }
    if (vfs_rd2_s(dst_slot, DIRNODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_DIR) {
        vfs->ctx->last_error = VFS_ERR_NOTDIR;
        return VFS_ERR_NOTDIR;
        }

    /* Find source entry by walking src_parent's DirContent chain */
    int64_t src_head = vfs_rd8_s(src_slot, DIRNODE_OFF_HEADPTR, ctx->page_size);
    int64_t read_epoch_rn = mapper_table_resolve(&ctx->mapper_table, epoch);
    uint32_t found_childId = 0;
    int64_t found_childPtr = 0;
    uint32_t found_epoch = 0;

    int64_t walk_vp = src_head;
    while (walk_vp != 0 && found_childPtr == 0) {
        uint8_t* dc = pool_resolve(&ctx->pool, walk_vp);
        if (!dc) break;
        uint32_t cc, ce;
        int64_t cp, np, nx;
        nodes_read_dircontent(dc, &cc, &ce, &cp, &np, &nx, ctx->page_size);

        int64_t effective_epoch = (int64_t)ce;
        if (mapper_table_traversal_apply(&ctx->mapper_table, (int64_t)ce))
            effective_epoch = mapper_table_resolve(&ctx->mapper_table, (int64_t)ce);

        int applies_rn = (effective_epoch == read_epoch_rn) ||
                         (effective_epoch < read_epoch_rn && effective_epoch % 2 == 0);

        if (np != 0 && applies_rn) {
            char en[256];
            int nl = nodes_read_name(&ctx->pool, np, en, (int)sizeof(en));
            if (nl > 0 && strcmp(en, src) == 0) {
                found_childId = cc;
                found_childPtr = cp;
                found_epoch = ce;
            }
        }
        walk_vp = nx;
    }

    if (found_childPtr == 0) { vfs->ctx->last_error = VFS_ERR_NOTFOUND; return VFS_ERR_NOTFOUND; }
    if (vfs_lock(vfs, (int64_t)found_childId, epoch) != VFS_OK) return VFS_ERR_IO;

    if (src_parent == dst_parent && found_epoch == (uint32_t)epoch) {
        /* Allocate NameEntry for the destination name */
        int64_t new_name_vp;
        int ns = nodes_write_name(&ctx->pool, dst, &new_name_vp);

        if (ns == 0) { vfs_unlock(vfs, (int64_t)found_childId, epoch); vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }

        /* Atomically store new namePtr into the existing DirContent slot.
           We need to find the correct DirContent VP for the found entry.
           Walk again to get the VP of the DirContent to update. */
        walk_vp = src_head;
        while (walk_vp != 0) {
            uint8_t* dc = pool_resolve(&ctx->pool, walk_vp);
            if (!dc) break;
            uint32_t cc, ce;
            int64_t cp, np, nx;
            nodes_read_dircontent(dc, &cc, &ce, &cp, &np, &nx, ctx->page_size);
            if (cp == found_childPtr && np != 0 && ce <= (uint32_t)epoch) {
                /* Update namePtr at offset DIRCONTENT_OFF_NAMEPTR */
                vfs_mb_release();
                vfs_atomic_store_i64(
                    (int64_t*)(dc + DIRCONTENT_OFF_NAMEPTR), new_name_vp);
                dentry_cache_invalidate(&ctx->readdir_cache);
                vfs_unlock(vfs, (int64_t)found_childId, epoch);
                return VFS_OK;
            }
            walk_vp = nx;
        }
        vfs->ctx->last_error = VFS_ERR_IO;
        vfs_unlock(vfs, (int64_t)found_childId, epoch);
        return VFS_ERR_IO;
    }

    /* Cross-directory rename: create new entry at dst, tombstone at src */
    int64_t dst_name_vp;
    int dst_ns = nodes_write_name(&ctx->pool, dst, &dst_name_vp);

    if (dst_ns == 0) { vfs_unlock(vfs, (int64_t)found_childId, epoch); vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }

    /* Allocate DirContent for dst */
    int64_t dst_dc_vp = pool_alloc(&ctx->pool);

    if (dst_dc_vp == VFS_VPTR_NULL) { vfs_unlock(vfs, (int64_t)found_childId, epoch); vfs->ctx->last_error = VFS_ERR_FULL; return VFS_ERR_FULL; }
    uint8_t* dst_dc_slot = pool_resolve(&ctx->pool, dst_dc_vp);

    if (!dst_dc_slot) { vfs_unlock(vfs, (int64_t)found_childId, epoch); vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }

    /* CAS-prepend to dst_parent's headPtr */
    int64_t dst_old_head;
    do {
        dst_old_head = vfs_atomic_load_i64(
            (const int64_t*)(dst_slot + DIRNODE_OFF_HEADPTR));
        nodes_write_dircontent(dst_dc_slot, found_childId, (uint32_t)epoch,
                               found_childPtr, dst_name_vp, dst_old_head,
                               ctx->page_size);
        vfs_mb_release();
    } while (vfs_cas_i64((int64_t*)(dst_slot + DIRNODE_OFF_HEADPTR),
                         dst_old_head, dst_dc_vp) != dst_old_head);

    /* Create tombstone at src */
    int64_t src_dc_vp = pool_alloc(&ctx->pool);

    if (src_dc_vp == VFS_VPTR_NULL) { vfs_unlock(vfs, (int64_t)found_childId, epoch); vfs->ctx->last_error = VFS_ERR_FULL; return VFS_ERR_FULL; }
    uint8_t* src_dc_slot = pool_resolve(&ctx->pool, src_dc_vp);

    if (!src_dc_slot) { vfs_unlock(vfs, (int64_t)found_childId, epoch); vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }

    int64_t src_old_head;
    do {
        src_old_head = vfs_atomic_load_i64(
            (const int64_t*)(src_slot + DIRNODE_OFF_HEADPTR));
        nodes_write_dircontent(src_dc_slot, found_childId, (uint32_t)epoch,
                               found_childPtr, 0, src_old_head, ctx->page_size);
        vfs_mb_release();
    } while (vfs_cas_i64((int64_t*)(src_slot + DIRNODE_OFF_HEADPTR),
                         src_old_head, src_dc_vp) != src_old_head);

    dentry_cache_invalidate(&ctx->readdir_cache);
    vfs_unlock(vfs, (int64_t)found_childId, epoch);
    return VFS_OK;
}

/* ---------------------------------------------------------------------------
 * vfs_open_file — resolve name to nodeId by walking parent's DirContent chain
 *
 * Returns childNodeId on success, or VFS_ERR_NOTFOUND if not found.
 * Uses read-rule: matches if epoch == query_epoch, or epoch < query AND even.
 * --------------------------------------------------------------------------- */

int64_t vfs_open_file(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch) {
    if (!vfs || !vfs->ctx || !name || name[0] == '\0') return VFS_ERR_IO;
    TreeContext* ctx = vfs->ctx;

    uint8_t* parent_slot = pool_resolve(&ctx->pool, (int64_t)parent);

    if (!parent_slot) { vfs->ctx->last_error = VFS_ERR_NOTFOUND; return VFS_ERR_NOTFOUND; }
    if (vfs_rd2_s(parent_slot, DIRNODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_DIR) {
        vfs->ctx->last_error = VFS_ERR_NOTDIR;
        return VFS_ERR_NOTDIR;
        }

    int64_t headPtr = vfs_rd8_s(parent_slot, DIRNODE_OFF_HEADPTR, ctx->page_size);
    int64_t read_epoch = mapper_table_resolve(&ctx->mapper_table, epoch);
    int64_t best_child = 0;
    int64_t best_childPtr = 0;
    int64_t best_effective_epoch = 0;
    int best_name_match = 0;

    int64_t walk_vp = headPtr;
    while (walk_vp != 0) {
        uint8_t* dc_slot = pool_resolve(&ctx->pool, walk_vp);
        if (!dc_slot) break;
        uint32_t ce_child, ce_epoch;
        int64_t ce_childPtr, ce_namePtr, ce_next;
        nodes_read_dircontent(dc_slot, &ce_child, &ce_epoch, &ce_childPtr,
                              &ce_namePtr, &ce_next, ctx->page_size);

        /* Compute effective epoch via mapper remapping */
        int64_t effective_epoch = (int64_t)ce_epoch;
        if (mapper_table_traversal_apply(&ctx->mapper_table, (int64_t)ce_epoch))
            effective_epoch = mapper_table_resolve(&ctx->mapper_table, (int64_t)ce_epoch);

        /* Read-rule: does this entry apply at read_epoch? */
        int applies = (effective_epoch == read_epoch) ||
                      (effective_epoch < read_epoch && effective_epoch % 2 == 0);
        if (!applies) { walk_vp = ce_next; continue; }

        if ((int64_t)ce_child == best_child && best_name_match &&
            effective_epoch <= best_effective_epoch)
            { walk_vp = ce_next; continue; }

        if (effective_epoch > best_effective_epoch || (int64_t)ce_child != best_child ||
            (ce_namePtr != 0 && best_name_match == 0 && effective_epoch == best_effective_epoch)) {
            if (ce_namePtr != 0) {
                char entry_name[256];
                int nl = nodes_read_name(&ctx->pool, ce_namePtr,
                                          entry_name, (int)sizeof(entry_name));
                if (nl > 0 && strcmp(entry_name, name) == 0) {
                    best_child = (int64_t)ce_child;
                    best_childPtr = ce_childPtr;
                    best_effective_epoch = effective_epoch;
                    best_name_match = 1;
                }
            } else {
                if ((int64_t)ce_child != best_child) {
                    best_child = (int64_t)ce_child;
                    best_childPtr = ce_childPtr;
                    best_effective_epoch = effective_epoch;
                    best_name_match = 0;
                }
            }
        }
        walk_vp = ce_next;
    }

    (void)best_childPtr;


    if (!best_name_match) { vfs->ctx->last_error = VFS_ERR_NOTFOUND; return VFS_ERR_NOTFOUND; }
    return best_child;
}

/* ---------------------------------------------------------------------------
 * vfs_file_size — query file size at a given epoch
 *
 * Walks the FileNode's sizePtr chain.  Applies the read-rule: finds the
 * first FileSize entry with epoch ≤ query_epoch (even or exact match).
 * Returns 0 if the chain is empty.
 * --------------------------------------------------------------------------- */

int64_t vfs_file_size(vfs_t* vfs, int64_t file, int64_t epoch) {
    if (!vfs || !vfs->ctx) return -1;
    TreeContext* ctx = vfs->ctx;

    uint8_t* file_slot = pool_resolve(&ctx->pool, (int64_t)file);
    if (!file_slot) {
        vfs->ctx->last_error = VFS_ERR_NOTFOUND;
        return -1;
    }
    if (vfs_rd2_s(file_slot, FILENODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_FILE) {
        vfs->ctx->last_error = VFS_ERR_IO;
        return -1;
    }

    int64_t sizePtr = vfs_rd8_s(file_slot, FILENODE_OFF_SIZEPTR, ctx->page_size);
    int64_t read_epoch = mapper_table_resolve(&ctx->mapper_table, epoch);

    int64_t walk_vp = sizePtr;
    while (walk_vp != 0) {
        uint8_t* fs_slot = pool_resolve(&ctx->pool, walk_vp);
        if (!fs_slot) break;
        uint32_t fs_epoch;
        int64_t fs_modified, fs_size, fs_next;
        nodes_read_filesize(fs_slot, &fs_epoch, &fs_modified, &fs_size, &fs_next, ctx->page_size);
        (void)fs_modified;

        int64_t effective_epoch = (int64_t)fs_epoch;
        if (mapper_table_traversal_apply(&ctx->mapper_table, (int64_t)fs_epoch))
            effective_epoch = mapper_table_resolve(&ctx->mapper_table, (int64_t)fs_epoch);

        if (effective_epoch == read_epoch ||
            (effective_epoch < read_epoch && effective_epoch % 2 == 0))
            return fs_size;

        walk_vp = fs_next;
    }
    return 0;  /* empty chain */
}

/* ---------------------------------------------------------------------------
 * vfs_file_mtime — query file modification time at a given epoch
 * --------------------------------------------------------------------------- */

int64_t vfs_file_mtime(vfs_t* vfs, int64_t file, int64_t epoch) {
    if (!vfs || !vfs->ctx) return -1;
    TreeContext* ctx = vfs->ctx;

    uint8_t* file_slot = pool_resolve(&ctx->pool, (int64_t)file);
    if (!file_slot) {
        vfs->ctx->last_error = VFS_ERR_NOTFOUND;
        return -1;
    }
    if (vfs_rd2_s(file_slot, FILENODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_FILE) {
        vfs->ctx->last_error = VFS_ERR_IO;
        return -1;
    }

    int64_t sizePtr = vfs_rd8_s(file_slot, FILENODE_OFF_SIZEPTR, ctx->page_size);
    int64_t read_epoch = mapper_table_resolve(&ctx->mapper_table, epoch);

    int64_t walk_vp = sizePtr;
    while (walk_vp != 0) {
        uint8_t* fs_slot = pool_resolve(&ctx->pool, walk_vp);
        if (!fs_slot) break;
        uint32_t fs_epoch;
        int64_t fs_modified, fs_size, fs_next;
        nodes_read_filesize(fs_slot, &fs_epoch, &fs_modified, &fs_size, &fs_next, ctx->page_size);
        (void)fs_size;

        int64_t effective_epoch = (int64_t)fs_epoch;
        if (mapper_table_traversal_apply(&ctx->mapper_table, (int64_t)fs_epoch))
            effective_epoch = mapper_table_resolve(&ctx->mapper_table, (int64_t)fs_epoch);

        if (effective_epoch == read_epoch ||
            (effective_epoch < read_epoch && effective_epoch % 2 == 0))
            return fs_modified;

        walk_vp = fs_next;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * vfs_file_ctime — query file creation time (immutable, no epoch needed)
 * --------------------------------------------------------------------------- */

int64_t vfs_file_ctime(vfs_t* vfs, int64_t file) {
    if (!vfs || !vfs->ctx) return -1;
    TreeContext* ctx = vfs->ctx;

    uint8_t* file_slot = pool_resolve(&ctx->pool, (int64_t)file);
    if (!file_slot) {
        vfs->ctx->last_error = VFS_ERR_NOTFOUND;
        return -1;
    }
    if (vfs_rd2_s(file_slot, FILENODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_FILE) {
        vfs->ctx->last_error = VFS_ERR_IO;
        return -1;
    }
    return nodes_read_filenode_ctime(file_slot, ctx->page_size);
}

/* ---------------------------------------------------------------------------
 * vfs_write — write data to a file at given offset and epoch
 *
 * Per-page: COW on first write per epoch, in-place on subsequent.
 * Returns bytes written, or -1 on error.
 * --------------------------------------------------------------------------- */

int vfs_write(vfs_t* vfs, int64_t file, const void* data, int64_t offset,
              int64_t count, int64_t epoch) {
    if (!vfs || !vfs->ctx || !data || count <= 0 || offset < 0) return -1;
    TreeContext* ctx = vfs->ctx;

    if (!vfs_epoch_is_writable(ctx, epoch)) {
        vfs->ctx->last_error = VFS_ERR_EPOCH;
        return -1;
    }

    uint8_t* file_slot = pool_resolve(&ctx->pool, file);
    if (!file_slot) {
        vfs->ctx->last_error = VFS_ERR_NOTFOUND;
        return -1;
    }
    if (vfs_rd2_s(file_slot, FILENODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_FILE) {
        vfs->ctx->last_error = VFS_ERR_IO;
        return -1;
    }

    if (vfs_lock(vfs, file, epoch) != VFS_OK) return -1;

    int64_t page_size = ctx->page_size;
    int64_t first_page = offset / page_size;
    int64_t last_page  = (offset + count - 1) / page_size;
    const uint8_t* src = (const uint8_t*)data;
    int64_t remaining = count;

    /* Track file growth for FileSize update */
    int64_t old_size = vfs_file_size(vfs, file, epoch);
    int64_t new_size = old_size;
    if (offset + count > new_size) new_size = offset + count;
    int grew = (new_size > old_size);

    /* Track first write to this file in this epoch for TouchedFile */
    int touched_this_epoch = 0;
    uint32_t file_nodeId = (uint32_t)vfs_rd4_s(file_slot, FILENODE_OFF_NODEID, ctx->page_size);

    for (int64_t p = first_page; p <= last_page; p++) {
        /* Resolve or create PageNode for this page */
        uint8_t* pn_slot = tree_resolve_page(ctx, file, p, epoch);
        if (!pn_slot) { vfs_unlock(vfs, file, epoch); return -1; }

        /* Compute intra-page offset and count */
        int64_t page_offset = (p == first_page) ? offset % page_size : 0;
        int64_t page_count = (int64_t)page_size - page_offset;
        if (remaining < page_count) page_count = remaining;

        while (1) {  /* retry loop for CAS */
            /* Walk version chain searching for existing write at this epoch */
            int64_t version_root = vfs_atomic_load_i64(
                (const int64_t*)(pn_slot + PAGENODE_OFF_VERSIONROOT));
            int64_t vp = version_root;
            int64_t data_page = -1;
            int found_in_place = 0;

            while (vp != 0) {
                uint8_t* vp_slot = pool_resolve(&ctx->pool, vp);
                if (!vp_slot) break;
                uint32_t vp_epoch;
                int64_t vp_dataPage, vp_next;
                nodes_read_versionpage(vp_slot, &vp_epoch, &vp_dataPage, &vp_next, ctx->page_size);
                if (vp_epoch == (uint32_t)epoch) {
                    data_page = vp_dataPage;
                    found_in_place = 1;
                    break;
                }
                vp = vp_next;
            }

            if (found_in_place) {
                /* In-place write: read current page, overlay, write back */
                uint8_t* page_buf = storage_read(ctx->sb, data_page);
                if (!page_buf) { vfs_unlock(vfs, file, epoch); return -1; }
                memcpy(page_buf + page_offset, src, (size_t)page_count);
                storage_write(ctx->sb, data_page, page_buf, 0);
                break;  /* exit retry loop — in-place succeeded */
            }

            /* COW: find base page (highest even epoch < write_epoch).
               VersionPages are prepended (newest first), so the first
               match is the highest even epoch. */
            int64_t base_page = -1;
            vp = version_root;
            while (vp != 0) {
                uint8_t* vp_slot = pool_resolve(&ctx->pool, vp);
                if (!vp_slot) break;
                uint32_t vp_epoch;
                int64_t vp_dataPage, vp_next;
                nodes_read_versionpage(vp_slot, &vp_epoch, &vp_dataPage, &vp_next, ctx->page_size);
                if (vp_epoch < (uint32_t)epoch && vp_epoch % 2 == 0) {
                    base_page = vp_dataPage;
                    break;
                }
                vp = vp_next;
            }

            /* Allocate new data page */
            int64_t new_dp = storage_allocate(ctx->sb, 1);
            if (new_dp < 0) { vfs_unlock(vfs, file, epoch); return -1; }

            /* Read or zero-fill the full page */
            uint8_t* page_buf = (uint8_t*)malloc((size_t)page_size);
            if (!page_buf) { vfs_unlock(vfs, file, epoch); return -1; }

            if (base_page >= 0) {
                uint8_t* base_buf = storage_read(ctx->sb, base_page);
                if (base_buf) {
                    memcpy(page_buf, base_buf, (size_t)page_size);
                } else {
                    memset(page_buf, 0, (size_t)page_size);
                }
            } else {
                memset(page_buf, 0, (size_t)page_size);
            }

            /* Overlay new data */
            memcpy(page_buf + page_offset, src, (size_t)page_count);
            storage_write(ctx->sb, new_dp, page_buf, 0);
            free(page_buf);

            /* Create VersionPage */
            int64_t vp_new = pool_alloc(&ctx->pool);
            if (vp_new == VFS_VPTR_NULL) { vfs_unlock(vfs, file, epoch); return -1; }
            uint8_t* vp_new_slot = pool_resolve(&ctx->pool, vp_new);
            if (!vp_new_slot) { vfs_unlock(vfs, file, epoch); return -1; }
            nodes_write_versionpage(vp_new_slot, (uint32_t)epoch, new_dp,
                                    version_root, ctx->page_size);

            /* Release barrier then CAS */
            vfs_mb_release();
            int64_t old_root = vfs_cas_i64(
                (int64_t*)(pn_slot + PAGENODE_OFF_VERSIONROOT),
                version_root, vp_new);
            if (old_root == version_root) {
                break;  /* CAS succeeded — exit retry loop */
            }
            /* CAS failed — retry loop will re-read version_root and try again.
               Our orphaned data page and VersionPage will be reclaimed by GC. */
        }

        /* TouchedFile: record first write to this file in this epoch.
           Uses touchedfile_add which handles dedup (checks for existing
           (epoch, nodeId) pair before inserting) and CAS-prepend. */
        if (!touched_this_epoch) {
            touchedfile_add(&ctx->pool, &ctx->touchedFilesPtr,
                            (uint32_t)epoch, file_nodeId);
            touched_this_epoch = 1;
        }

        src += page_count;
        remaining -= page_count;
    }

    /* Update FileSize if file grew */
    if (grew) {
        while (1) {  /* retry loop for CAS */
            int64_t old_sizePtr = vfs_atomic_load_i64(
                (const int64_t*)(file_slot + FILENODE_OFF_SIZEPTR));
            int64_t fs_vp = pool_alloc(&ctx->pool);
            if (fs_vp == VFS_VPTR_NULL) { vfs_unlock(vfs, file, epoch); return -1; }
            uint8_t* fs_slot = pool_resolve(&ctx->pool, fs_vp);
            if (!fs_slot) { vfs_unlock(vfs, file, epoch); return -1; }
            nodes_write_filesize(fs_slot, (uint32_t)epoch, (int64_t)time(NULL),
                                 new_size, old_sizePtr, ctx->page_size);
            vfs_mb_release();
            int64_t cas_res = vfs_cas_i64(
                (int64_t*)(file_slot + FILENODE_OFF_SIZEPTR),
                old_sizePtr, fs_vp);
            if (cas_res == old_sizePtr) break;
            /* CAS failed — retry with fresh old_sizePtr */
        }
    }

    vfs_unlock(vfs, file, epoch);
    return (int)count;
}

/* ---------------------------------------------------------------------------
 * vfs_read — read data from a file at given offset and epoch
 *
 * Uses the read-rule: exact match at read_epoch, or highest even epoch
 * < read_epoch.  Unwritten pages return zero-filled data.
 * Returns bytes read, or -1 on error.
 * --------------------------------------------------------------------------- */

int vfs_read(vfs_t* vfs, int64_t file, void* buf, int64_t offset,
             int64_t count, int64_t epoch) {
    if (!vfs || !vfs->ctx || !buf || count <= 0 || offset < 0) return -1;
    TreeContext* ctx = vfs->ctx;

    int64_t read_epoch = mapper_table_resolve(&ctx->mapper_table, epoch);

    uint8_t* file_slot = pool_resolve(&ctx->pool, file);
    if (!file_slot) {
        vfs->ctx->last_error = VFS_ERR_NOTFOUND;
        return -1;
    }
    if (vfs_rd2_s(file_slot, FILENODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_FILE) {
        vfs->ctx->last_error = VFS_ERR_IO;
        return -1;
    }

    int64_t page_size = ctx->page_size;
    int64_t first_page = offset / page_size;
    int64_t last_page  = (offset + count - 1) / page_size;
    uint8_t* dst = (uint8_t*)buf;
    int64_t remaining = count;

    for (int64_t p = first_page; p <= last_page; p++) {
        /* Resolve PageNode for this page */
        uint8_t* pn_slot = tree_resolve_page(ctx, file, p, read_epoch);
        if (!pn_slot) {
            /* Page doesn't exist → zero-fill this portion */
            int64_t page_offset = (p == first_page) ? offset % page_size : 0;
            int64_t page_count = (int64_t)page_size - page_offset;
            if (remaining < page_count) page_count = remaining;
            memset(dst, 0, (size_t)page_count);
            dst += page_count;
            remaining -= page_count;
            continue;
        }

        /* Compute intra-page offset and count */
        int64_t page_offset = (p == first_page) ? offset % page_size : 0;
        int64_t page_count = (int64_t)page_size - page_offset;
        if (remaining < page_count) page_count = remaining;

        /* Walk version chain applying read-rule */
        int64_t vp = vfs_atomic_load_i64(
            (const int64_t*)(pn_slot + PAGENODE_OFF_VERSIONROOT));
        int64_t data_page = -1;
        int found = 0;

        /* Fast path: live-head read with no mapper → first entry is always
           the answer.  Version chains are prepended (newest first).
           Verify the first entry's epoch matches read_epoch. */
        if (ctx->epochMapperPtr == 0 && read_epoch == ctx->currentEpoch && vp != 0) {
            uint8_t* vp_slot = pool_resolve(&ctx->pool, vp);
            if (vp_slot) {
                uint32_t vp_epoch;
                int64_t vp_dataPage, vp_next;
                nodes_read_versionpage(vp_slot, &vp_epoch, &vp_dataPage, &vp_next, ctx->page_size);
                (void)vp_next;
                if (vp_epoch == (uint32_t)read_epoch) {
                    data_page = vp_dataPage;
                    found = 1;
                }
            }
        }

        while (!found && vp != 0) {
            uint8_t* vp_slot = pool_resolve(&ctx->pool, vp);
            if (!vp_slot) break;
            uint32_t vp_epoch;
            int64_t vp_dataPage, vp_next;
            nodes_read_versionpage(vp_slot, &vp_epoch, &vp_dataPage, &vp_next, ctx->page_size);

            /* Apply mapper traversal remapping: if a mapping exists for this
               entry's epoch and traversalApply is true, treat the entry as if
               its epoch were the mapped toEpoch.  If traversalApply is false,
               keep the entry's original epoch (odd → skipped by read-rule). */
            int64_t effective_epoch = (int64_t)vp_epoch;
            if (mapper_table_traversal_apply(&ctx->mapper_table, (int64_t)vp_epoch)) {
                effective_epoch = mapper_table_resolve(&ctx->mapper_table,
                                                       (int64_t)vp_epoch);
            }

            if (effective_epoch == read_epoch) {
                /* Exact match — use it regardless of even/odd */
                data_page = vp_dataPage;
                found = 1;
            } else if (effective_epoch < read_epoch) {
                if (effective_epoch % 2 == 0) {
                    /* Even epoch < read_epoch — committed base, use it */
                    data_page = vp_dataPage;
                    found = 1;
                }
                /* Odd epoch < read_epoch — skip (unrelated snapshot) */
            }
            /* vp_epoch > read_epoch — skip, continue walking */

            vp = vp_next;
        }

        if (found) {
            /* Read data page and copy intra-page portion */
            int64_t t_before = vfs_cache_total();
            int64_t h_before = vfs_cache_hits();
            uint8_t* page_data = storage_read(ctx->sb, data_page);
            vfs_data_inc_total();
            if (vfs_cache_total() > t_before && vfs_cache_hits() > h_before)
                vfs_data_inc_hits();
            if (page_data) {
                memcpy(dst, page_data + page_offset, (size_t)page_count);
            } else {
                memset(dst, 0, (size_t)page_count);
            }
        } else {
            /* No VersionPage found — page never written at this epoch */
            memset(dst, 0, (size_t)page_count);
        }

        dst += page_count;
        remaining -= page_count;
    }

    return (int)(dst - (uint8_t*)buf);
}
