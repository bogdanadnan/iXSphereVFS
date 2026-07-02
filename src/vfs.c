#include "ixsphere_vfs.h"
#include "platform.h"
#include "vfs_internal.h"
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
        default:                return "Unknown error";
    }
}

vfs_t* vfs_open(const char* path) {
    vfs_t* vfs = (vfs_t*)calloc(1, sizeof(vfs_t));
    if (!vfs) return NULL;

    TreeContext* ctx = (TreeContext*)calloc(1, sizeof(TreeContext));
    if (!ctx) {
        free(vfs);
        return NULL;
    }

    ctx->sb = storage_open(path, VFS_PAGE_SIZE);
    if (!ctx->sb) {
        free(ctx);
        free(vfs);
        return NULL;
    }

    /* Initialize pool allocator — list_head points into TreeContext */
    pool_init(&ctx->pool, ctx->sb, &ctx->pool_list_head_value);

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
        storage_close(vfs->ctx->sb);
        free(vfs->ctx);
    }
    free(vfs);
}
