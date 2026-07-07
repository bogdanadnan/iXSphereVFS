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
 * Option parsing state — ephemeral, populated by fuse_vfs_opt_proc
 * during argument parsing, consumed by fuse_vfs_init to populate
 * fuse_vfs_state_t, then destroyed.  Not retained after mount.
 * --------------------------------------------------------------------------- */

typedef struct {
    char*   vfs_path;   /* strdup'd path to VFS backing file */
    int64_t epoch;      /* initial working epoch (0 = base) */
    int64_t page_size;  /* VFS page size (default 8192) */
    int     readonly;   /* non-zero for read-only mount */
} fuse_vfs_opts;

/* ---------------------------------------------------------------------------
 * FUSE option parsing keys — custom keys for -o epoch=, -o page_size=,
 * -o readonly.  allow_other is handled by libfuse's built-in parser.
 * --------------------------------------------------------------------------- */

#ifdef FUSE3_FOUND

#include <inttypes.h>

/* Custom option keys (negative to avoid collision with FUSE_OPT_KEY_*). */
enum {
    KEY_VFS_PATH   = -1,
    KEY_EPOCH      = -2,
    KEY_PAGE_SIZE  = -3,
    KEY_READONLY   = -4,
    KEY_HELP       = -5,
};

static const struct fuse_opt fuse_vfs_opts_spec[] = {
    { "--vfs-file=%s", 0, KEY_VFS_PATH },
    FUSE_OPT_KEY("-o epoch=",           KEY_EPOCH),
    FUSE_OPT_KEY("-o page_size=",       KEY_PAGE_SIZE),
    FUSE_OPT_KEY("-o readonly",         KEY_READONLY),
    FUSE_OPT_KEY("-h",                  KEY_HELP),
    FUSE_OPT_KEY("--help",              KEY_HELP),
    FUSE_OPT_END
};

#endif /* FUSE3_FOUND */

/* ---------------------------------------------------------------------------
 * Error mapping — VFS error codes to POSIX errno values.
 * Used by FUSE callbacks to translate VFS errors into negative errno
 * returns expected by FUSE.
 * --------------------------------------------------------------------------- */

int vfs_error_to_errno(int vfs_err) {
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
    /* root and input validation vfs_open will handle ctx-NULL downstream */
    if (!vfs) return 0;

    /* Root directory */
    if (!path || path[0] == '\0' || (path[0] == '/' && path[1] == '\0'))
        return vfs_root(vfs);

    /* Walk from root */
    int64_t parent = vfs_root(vfs);

    /* Make a mutable copy for strtok_r */
    size_t path_len = strlen(path);
    char* path_copy = (char*)malloc(path_len + 1);
    if (!path_copy) return 0;
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

        /* ".." — parent directory (not supported: VFS has no parent pointer) */
        if (strcmp(token, "..") == 0) {
            free(path_copy);
            return 0;
        }

        /* Resolve the component.  vfs_open sets vfs->ctx->last_error
           on failure; callers read it via vfs_last_error(vfs) and
           convert via vfs_error_to_errno to produce FUSE-negative
           return values. */
        int64_t child = vfs_open(vfs, parent, token, 0);
        if (child <= 0) {
            free(path_copy);
            return 0;
        }

        parent = child;
        token = strtok_r(NULL, "/", &saveptr);
    }

    free(path_copy);
    return parent;
}

/* ---------------------------------------------------------------------------
 * Directory check — returns non-zero if the VFS node at vp is a directory.
 * --------------------------------------------------------------------------- */

int fuse_is_dir(vfs_t* vfs, int64_t vp) {
    return vfs_node_type(vfs, vp) == 1;  /* 1 = NODE_TYPE_DIR, 3 = NODE_TYPE_FILE, 0 = error */
}
