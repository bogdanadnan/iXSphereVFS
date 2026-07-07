/* FUSE-based filesystem interface for iXSphereVFS.
 * Conditionally built when FUSE3 is available.
 * Exposes a VFS mount as a FUSE filesystem.
 */

#include "ixsphere/vfs.h"
#include "fuse_vfs.h"

#ifdef FUSE3_FOUND
#include <fuse3/fuse.h>
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Error mapping — VFS error codes to POSIX errno values.
 * Used by FUSE callbacks to translate VFS errors into negative errno
 * returns expected by FUSE.
 * --------------------------------------------------------------------------- */

static int vfs_error_to_errno(int vfs_err) {
    switch (vfs_err) {
    case VFS_OK:            return 0;
    case VFS_ERR_IO:        return -EIO;
    case VFS_ERR_NOTFOUND:  return -ENOENT;
    case VFS_ERR_EXISTS:    return -EEXIST;
    case VFS_ERR_NOTDIR:    return -ENOTDIR;
    case VFS_ERR_NOTEMPTY:  return -ENOTEMPTY;
    case VFS_ERR_CONFLICT:  return -EBUSY;
    case VFS_ERR_FULL:      return -ENOSPC;
    case VFS_ERR_NOMEM:     return -ENOMEM;
    case VFS_ERR_EPOCH:     return -EINVAL;
    default:                return -EIO;
    }
}

/* ---------------------------------------------------------------------------
 * Path resolution — splits a POSIX path ("/a/b/c.txt") into components
 * and walks the VFS tree to resolve the final VirtualPtr.
 *
 * Returns VirtualPtr on success, negative VFS error code on failure.
 * Root "/" returns vfs_root().  Components "." and ".." are handled.
 * Trailing slashes are stripped.
 * --------------------------------------------------------------------------- */

int64_t resolve_full_path(vfs_t* vfs, const char* path) {
    if (!vfs) return VFS_ERR_IO;

    /* Root directory */
    if (!path || path[0] == '\0' || (path[0] == '/' && path[1] == '\0'))
        return vfs_root(vfs);

    /* Walk from root */
    int64_t parent = vfs_root(vfs);

    /* Make a mutable copy for strtok_r */
    size_t path_len = strlen(path);
    char* path_copy = (char*)malloc(path_len + 1);
    if (!path_copy) return VFS_ERR_NOMEM;
    memcpy(path_copy, path, path_len + 1);

    char* saveptr = NULL;
    char* token = strtok_r(path_copy, "/", &saveptr);

    while (token) {
        /* Skip empty tokens (leading or double slashes) */
        if (token[0] == '\0') {
            token = strtok_r(NULL, "/", &saveptr);
            continue;
        }

        /* "." — stay in current directory */
        if (strcmp(token, ".") == 0) {
            token = strtok_r(NULL, "/", &saveptr);
            continue;
        }

        /* ".." — parent directory (not supported: VFS has no parent pointer).
           Return an error for now; Phase 9 adds readlink/symlink support. */
        if (strcmp(token, "..") == 0) {
            free(path_copy);
            return VFS_ERR_NOTDIR;
        }

        /* Resolve the component */
        int64_t child = vfs_open(vfs, parent, token, 0);
        if (child <= 0) {
            free(path_copy);
            /* vfs_open returns VFS_ERR_NOTFOUND or other negative on failure */
            return (int)child;
        }

        parent = child;
        token = strtok_r(NULL, "/", &saveptr);
    }

    free(path_copy);
    return parent;
}
