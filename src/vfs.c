/*
 * src/vfs.c — Spec 30c VFS entry point
 */
#include "vfs_internal.h"
#include <stdlib.h>
#include <string.h>

vfs_t* vfs_open(const char* path) {
    vfs_t* vfs = calloc(1, sizeof(vfs_t));
    if (!vfs) return NULL;
    strncpy(vfs->path, path, sizeof(vfs->path) - 1);
    /* TODO: mount superblock, init bitmap, init storage */
    return vfs;
}

void vfs_close(vfs_t* vfs) {
    if (vfs) {
        /* TODO: flush, free resources */
        free(vfs);
    }
}

vfs_error_t vfs_last_error(vfs_t* vfs) {
    return vfs ? vfs->last_error : VFS_ERR_NOMEM;
}

const char* vfs_error_string(vfs_error_t err) {
    switch (err) {
        case VFS_OK:        return "OK";
        case VFS_ERR_NOMEM: return "No memory";
        case VFS_ERR_NOTFOUND: return "Not found";
        case VFS_ERR_CONFLICT: return "Conflict";
        case VFS_ERR_IO:    return "I/O error";
        case VFS_ERR_CORRUPT: return "Data corrupt";
        default:            return "Unknown error";
    }
}
