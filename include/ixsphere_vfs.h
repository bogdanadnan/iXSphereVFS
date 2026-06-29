/*
 * include/ixsphere_vfs.h — iXSphereVFS Public API
 *
 * This header declares the public interface. It grows with each phase.
 */
#ifndef IXSPHERE_VFS_H
#define IXSPHERE_VFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Error codes ───────────────────────────────────────── */

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
} vfs_error_t;

const char* vfs_error_string(vfs_error_t err);

/* ── Opaque handle ─────────────────────────────────────── */

typedef struct vfs_t vfs_t;

/* ── Instance management ───────────────────────────────── */

vfs_t*  vfs_open(const char* path);
void    vfs_close(vfs_t* vfs);

#ifdef __cplusplus
}
#endif
#endif /* IXSPHERE_VFS_H */
