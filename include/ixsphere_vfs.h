#ifndef IXSPHERE_VFS_H
#define IXSPHERE_VFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VFS_OK            =  0,
    VFS_ERR_IO        = -1,
    VFS_ERR_NOTFOUND  = -2,
    VFS_ERR_EXISTS    = -3,
    VFS_ERR_NOTDIR    = -4,
    VFS_ERR_NOTEMPTY  = -5,
    VFS_ERR_CONFLICT  = -6,
    VFS_ERR_FULL      = -7,
    VFS_ERR_NOMEM     = -8,
    VFS_ERR_EPOCH     = -9,
} vfs_error_t;

const char* vfs_error_string(vfs_error_t err);

uint32_t vfs_crc32c(const uint8_t* data, size_t len);

typedef struct vfs_t vfs_t;

vfs_t*  vfs_open(const char* path, int64_t page_size);
void    vfs_close(vfs_t* vfs);

/* Include the full tree API */
#include "tree_api.h"

#ifdef __cplusplus
}
#endif
#endif
