/* fuse_vfs.h — shared state and declarations for the FUSE filesystem layer.
 *
 * Includes ONLY the public ixsphere/vfs.h header — never vfs_internal.h.
 * This ensures the FUSE layer can be built against only the public API
 * surface, keeping internal struct details encapsulated.
 */

#ifndef VFS_FUSE_VFS_H
#define VFS_FUSE_VFS_H

#include "ixsphere/vfs.h"
#include <stdbool.h>

/* Per-mount state passed to FUSE callbacks via private_data.
   All fields are owned by the caller; the FUSE layer does not
   allocate or free them. */
typedef struct {
    vfs_t*  vfs;        /* mounted VFS handle */
    char*   vfs_path;   /* backing file path (caller-owned) */
    int64_t epoch;      /* current working epoch (0 = base, odd = snapshot) */
    int64_t page_size;  /* VFS page size in bytes */
    bool    readonly;   /* mount is read-only */
} fuse_vfs_state_t;

#endif /* VFS_FUSE_VFS_H */
