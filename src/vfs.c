#include "ixsphere/vfs.h"
#include "platform.h"
#include "ixsphere/vfs_internal.h"
#include "tree.h"
#include <stdlib.h>
#include <string.h>

const char* vfs_error_string(vfs_error_t err) {
    switch (err) {
        case VFS_OK:            return "OK";
        case VFS_ERR_IO:        return "I/O error";
        case VFS_ERR_NOTFOUND:  return "Not found";
        case VFS_ERR_EXISTS:    return "Already exists";
        case VFS_ERR_NOTDIR:    return "Not a directory";
        case VFS_ERR_NOTEMPTY:  return "Directory not empty";
        case VFS_ERR_CONFLICT:  return "Conflict";
        case VFS_ERR_FULL:      return "No space left";
        case VFS_ERR_NOMEM:     return "Out of memory";
        case VFS_ERR_EPOCH:     return "Epoch not writable";
        default:                return "Unknown error";
    }
}

vfs_t* vfs_open(const char* path, int64_t page_size) {
    vfs_t* vfs = (vfs_t*)calloc(1, sizeof(vfs_t));
    if (!vfs) return NULL;

    TreeContext* ctx = (TreeContext*)calloc(1, sizeof(TreeContext));
    if (!ctx) {
        free(vfs);
        return NULL;
    }

    ctx->sb = storage_open(path, page_size);
    if (!ctx->sb) {
        free(ctx);
        free(vfs);
        return NULL;
    }

    ctx->page_size = ctx->sb->page_size;

    /* Initialize pool allocator — list_head points into TreeContext */
    pool_init(&ctx->pool, ctx->sb, &ctx->pool_list_head_value);

    /* Initialize epoch mapper */
    mapper_init(&ctx->mapper, &ctx->pool, &ctx->epochMapperPtr);

    /* Bootstrap or reinitialize the tree.
       tree_bootstrap_superblock handles both fresh and reopen internally. */
    int err = tree_bootstrap_superblock(ctx);
    if (err != VFS_OK) {
        storage_close(ctx->sb);
        free(ctx);
        free(vfs);
        return NULL;
    }

    vfs->ctx = ctx;
    return vfs;
}

void vfs_close(vfs_t* vfs) {
    if (!vfs) return;
    if (vfs->ctx) {
        /* Flush superblock to persist any pending changes */
        tree_superblock_write(vfs->ctx);
        /* Destroy page array cache if built */
        if (vfs->ctx->seg_array_fc_vp != 0)
            segment_array_destroy(&vfs->ctx->seg_array_cache);
        storage_close(vfs->ctx->sb);
        free(vfs->ctx);
    }
    free(vfs);
}

int vfs_flush(vfs_t* vfs) {
    if (!vfs || !vfs->ctx) return VFS_ERR_IO;
    storage_flush(vfs->ctx->sb, -1);
    return VFS_OK;
}

vfs_error_t vfs_last_error(vfs_t* vfs) {
    if (!vfs || !vfs->ctx) return VFS_ERR_IO;
    return vfs->ctx->last_error;
}
