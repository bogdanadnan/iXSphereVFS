/* Phase 5: Tree Operations — Bootstrap, Init, Superblock I/O */
#include "tree.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------------------
 * Superblock I/O helpers
 * --------------------------------------------------------------------------- */

int tree_superblock_read(TreeContext* ctx) {
    uint8_t* payload = storage_read(ctx->sb, SUPERBLOCK_PAGE);
    if (!payload) return VFS_ERR_IO;

    ctx->rootNodeOffset   = vfs_rd8(payload, SB_OFF_ROOT_OFFSET);
    ctx->currentEpoch     = vfs_rd8(payload, SB_OFF_CURRENT_EPOCH);
    ctx->epochMapperPtr   = vfs_rd8(payload, SB_OFF_EPOCH_MAPPER_PTR);
    ctx->treeLockState    = vfs_rd8(payload, SB_OFF_TREE_LOCK_STATE);
    ctx->nextNodeId       = (uint32_t)vfs_rd4(payload, SB_OFF_NEXT_NODE_ID);
    ctx->touchedFilesPtr  = vfs_rd8(payload, SB_OFF_TOUCHED_FILES_PTR);

    /* poolListHead — wire into pool allocator */
    int64_t pool_list_head = vfs_rd8(payload, SB_OFF_POOL_LIST_HEAD);
    if (ctx->pool.list_head) *ctx->pool.list_head = pool_list_head;

    return VFS_OK;
}

int tree_superblock_write(TreeContext* ctx) {
    int64_t ps = ctx->sb->page_size;
    uint8_t* buf = (uint8_t*)malloc((size_t)ps);
    if (!buf) return VFS_ERR_NOMEM;
    memset(buf, 0, (size_t)ps);

    vfs_wr8(buf, SB_OFF_ROOT_OFFSET,       ctx->rootNodeOffset);
    vfs_wr8(buf, SB_OFF_CURRENT_EPOCH,     ctx->currentEpoch);
    vfs_wr8(buf, SB_OFF_EPOCH_MAPPER_PTR,  ctx->epochMapperPtr);
    vfs_wr8(buf, SB_OFF_POOL_LIST_HEAD,    ctx->pool.list_head ? *ctx->pool.list_head : 0);
    vfs_wr8(buf, SB_OFF_TREE_LOCK_STATE,   ctx->treeLockState);
    vfs_wr4(buf, SB_OFF_NEXT_NODE_ID,      (int32_t)ctx->nextNodeId);
    vfs_wr8(buf, SB_OFF_TOUCHED_FILES_PTR, ctx->touchedFilesPtr);

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
        ctx->segment_size = (uint32_t)vfs_rd4(hdr, HDR_OFF_SEGMENT_SIZE);
    } else {
        ctx->segment_size = 1024;  /* default */
    }

    /* Allocate pool already exists in ctx->pool from vfs_open */
    int64_t root_vp = pool_alloc(&ctx->pool);
    if (root_vp == VFS_VPTR_NULL) return VFS_ERR_FULL;

    uint8_t* root_slot = pool_resolve(&ctx->pool, root_vp);
    if (!root_slot) return VFS_ERR_IO;

    /* Write root DirNode: nodeId=0, no children */
    nodes_write_dirnode(root_slot, 0, 0);

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
    int16_t type = vfs_rd2(root_slot, DIRNODE_OFF_TYPE);
    if (type != (int16_t)NODE_TYPE_DIR) return VFS_ERR_IO;

    /* Read segment_size from StorageBackend header */
    uint8_t* hdr = storage_read(ctx->sb, 0);
    if (hdr) {
        ctx->segment_size = (uint32_t)vfs_rd4(hdr, HDR_OFF_SEGMENT_SIZE);
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
        nodes_read_mapperentry(slot, &fromEpoch, &toEpoch, &flags, &next);
        (void)fromEpoch; (void)toEpoch; (void)flags;
        mapper_vp = next;
    }

    return VFS_OK;
}

/* ---------------------------------------------------------------------------
 * Page Resolution — resolve a logical page to its PageNode
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

    nodes_read_filenode(file_slot, NULL, &fc_vp, NULL, NULL);

    /* Walk FileContent chain to find the target segment */
    for (int64_t i = 0; i <= segment_idx; i++) {
        if (fc_vp == VFS_VPTR_NULL) {
            /* Segment doesn't exist yet — file growth:
               allocate new FileContent + PageNodes */
            int64_t page_root_vp = VFS_VPTR_NULL;
            int64_t prev_pn_vp = 0;

            /* Allocate PageNodes for this segment (reverse order) */
            for (int64_t p = seg_size - 1; p >= 0; p--) {
                int64_t pn_vp = pool_alloc(&ctx->pool);
                if (pn_vp == VFS_VPTR_NULL) return NULL;
                uint8_t* pn_slot = pool_resolve(&ctx->pool, pn_vp);
                if (!pn_slot) return NULL;
                nodes_write_pagenode(pn_slot, 0, prev_pn_vp);
                prev_pn_vp = pn_vp;
                if (p == 0) page_root_vp = pn_vp;
            }

            /* Allocate FileContent entry */
            int64_t new_fc_vp = pool_alloc(&ctx->pool);
            if (new_fc_vp == VFS_VPTR_NULL) return NULL;
            uint8_t* fc_slot = pool_resolve(&ctx->pool, new_fc_vp);
            if (!fc_slot) return NULL;
            nodes_write_filecontent(fc_slot, page_root_vp, 0);

            /* Link: if this is the first segment, CAS into FileNode.
               Otherwise, CAS-append to previous FileContent's nextPtr. */
            if (i == 0) {
                /* For now, simple store — CAS will be needed for concurrency */
                vfs_wr8(file_slot, FILENODE_OFF_HEADPTR, new_fc_vp);
                fc_vp = new_fc_vp;
            } else {
                vfs_wr8(fc_slot, FILECONTENT_OFF_NEXTPTR, new_fc_vp);
                fc_vp = new_fc_vp;
            }
            break;
        }

        uint8_t* fc_slot = pool_resolve(&ctx->pool, fc_vp);
        if (!fc_slot) return NULL;

        if (i == segment_idx) {
            /* Walk PageNode chain to build in-memory array (or find target) */
            int64_t pn_vp = vfs_rd8(fc_slot, FILECONTENT_OFF_ROOTPTR);
            for (int64_t p = 0; p < page_in_segment; p++) {
                if (pn_vp == VFS_VPTR_NULL) return NULL;
                uint8_t* pn_slot = pool_resolve(&ctx->pool, pn_vp);
                if (!pn_slot) return NULL;
                pn_vp = vfs_rd8(pn_slot, PAGENODE_OFF_NEXTPTR);
            }
            if (pn_vp == VFS_VPTR_NULL) return NULL;
            return pool_resolve(&ctx->pool, pn_vp);
        }

        fc_vp = vfs_rd8(fc_slot, FILECONTENT_OFF_NEXTPTR);
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

    /* Validate epoch is writable (Phase 6 stub: always writable) */
    if (!vfs_epoch_is_writable(ctx, (int64_t)epoch, NULL)) return VFS_ERR_IO;

    /* Read parent DirNode, verify type */
    uint8_t* parent_slot = pool_resolve(&ctx->pool, (int64_t)parent);
    if (!parent_slot) return VFS_ERR_NOTFOUND;
    if (vfs_rd2(parent_slot, DIRNODE_OFF_TYPE) != (int16_t)NODE_TYPE_DIR)
        return VFS_ERR_NOTDIR;

    /* Walk parent's DirContent chain, checking for name collision */
    int64_t headPtr = vfs_rd8(parent_slot, DIRNODE_OFF_HEADPTR);
    int64_t walk_vp = headPtr;
    while (walk_vp != 0) {
        uint8_t* dc_slot = pool_resolve(&ctx->pool, walk_vp);
        if (!dc_slot) break;
        uint32_t ce_child, ce_epoch;
        int64_t ce_namePtr, ce_next;
        nodes_read_dircontent(dc_slot, &ce_child, &ce_epoch, NULL,
                              &ce_namePtr, &ce_next);
        if (ce_epoch == (uint32_t)epoch && ce_namePtr != 0) {
            /* Read the name and compare */
            char entry_name[256];
            int name_len = nodes_read_name(&ctx->pool, ce_namePtr,
                                            entry_name, (int)sizeof(entry_name));
            if (name_len > 0 && strcmp(entry_name, name) == 0)
                return VFS_ERR_EXISTS;
        }
        walk_vp = ce_next;
    }

    /* Atomically increment nextNodeId */
    uint32_t new_nodeId = (uint32_t)vfs_atomic_add_i32((int32_t*)&ctx->nextNodeId, 1);
    /* nextNodeId starts at 1, first add yields nodeId=1 */

    /* Allocate FileNode slot and write it */
    int64_t file_vp = pool_alloc(&ctx->pool);
    if (file_vp == VFS_VPTR_NULL) return VFS_ERR_FULL;
    uint8_t* file_slot = pool_resolve(&ctx->pool, file_vp);
    if (!file_slot) return VFS_ERR_IO;
    nodes_write_filenode(file_slot, new_nodeId, 0, 0, (int64_t)time(NULL));

    /* Allocate NameEntry chain for the file name */
    int64_t name_vp;
    int name_slots = nodes_write_name(&ctx->pool, name, &name_vp);
    if (name_slots == 0) return VFS_ERR_IO;

    /* CAS-prepend DirContent to parent's headPtr */
    int64_t old_head, dc_vp;
    uint8_t* dc_slot;
    do {
        old_head = vfs_atomic_load_i64(
            (const int64_t*)(parent_slot + DIRNODE_OFF_HEADPTR));
        dc_vp = pool_alloc(&ctx->pool);
        if (dc_vp == VFS_VPTR_NULL) return VFS_ERR_FULL;
        dc_slot = pool_resolve(&ctx->pool, dc_vp);
        if (!dc_slot) return VFS_ERR_IO;

        nodes_write_dircontent(dc_slot, new_nodeId, (uint32_t)epoch,
                               file_vp, name_vp, old_head);
        vfs_mb_release();
    } while (vfs_cas_i64((int64_t*)(parent_slot + DIRNODE_OFF_HEADPTR),
                         old_head, dc_vp) != old_head);

    return (int)new_nodeId;
}

/* ---------------------------------------------------------------------------
 * vfs_delete — delete a file by prepending a tombstone DirContent
 * --------------------------------------------------------------------------- */

int vfs_delete(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch) {
    if (!vfs || !vfs->ctx || !name || name[0] == '\0') return VFS_ERR_IO;
    TreeContext* ctx = vfs->ctx;

    if (!vfs_epoch_is_writable(ctx, (int64_t)epoch, NULL)) return VFS_ERR_IO;

    /* Read parent DirNode, verify type */
    uint8_t* parent_slot = pool_resolve(&ctx->pool, (int64_t)parent);
    if (!parent_slot) return VFS_ERR_NOTFOUND;
    if (vfs_rd2(parent_slot, DIRNODE_OFF_TYPE) != (int16_t)NODE_TYPE_DIR)
        return VFS_ERR_NOTDIR;

    /* Walk parent's DirContent chain to find the entry with matching name
       at epoch ≤ query_epoch. Chain is in descending epoch order,
       so the first match is the most recent at or before query_epoch. */
    int64_t headPtr = vfs_rd8(parent_slot, DIRNODE_OFF_HEADPTR);
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
                              &ce_namePtr, &ce_next);
        /* Only match non-tombstone entries at or before query epoch */
        if (ce_namePtr != 0 && ce_epoch <= (uint32_t)epoch) {
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

    if (found_vp == 0) return VFS_ERR_NOTFOUND;

    /* Allocate tombstone DirContent and CAS-prepend */
    int64_t old_head, dc_vp;
    uint8_t* dc_slot;
    do {
        old_head = vfs_atomic_load_i64(
            (const int64_t*)(parent_slot + DIRNODE_OFF_HEADPTR));
        dc_vp = pool_alloc(&ctx->pool);
        if (dc_vp == VFS_VPTR_NULL) return VFS_ERR_FULL;
        dc_slot = pool_resolve(&ctx->pool, dc_vp);
        if (!dc_slot) return VFS_ERR_IO;

        /* Tombstone: namePtr=0 means deleted */
        nodes_write_dircontent(dc_slot, found_childId, (uint32_t)epoch,
                               found_childPtr, 0, old_head);
        vfs_mb_release();
    } while (vfs_cas_i64((int64_t*)(parent_slot + DIRNODE_OFF_HEADPTR),
                         old_head, dc_vp) != old_head);

    return VFS_OK;
}
