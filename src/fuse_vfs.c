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

const struct fuse_opt fuse_vfs_opts_spec[] = {
    { "--vfs-file=%s", 0, KEY_VFS_PATH },
    FUSE_OPT_KEY("-o epoch=",           KEY_EPOCH),
    FUSE_OPT_KEY("-o page_size=",       KEY_PAGE_SIZE),
    FUSE_OPT_KEY("-o readonly",         KEY_READONLY),
    FUSE_OPT_KEY("-h",                  KEY_HELP),
    FUSE_OPT_KEY("--help",              KEY_HELP),
    FUSE_OPT_END
};

/* ---------------------------------------------------------------------------
 * Option processing — called by libfuse for every matched option.
 * Populates the fuse_vfs_opts struct.  Non-matched options return 1
 * (pass through to libfuse).  Matched options return 0 (consumed).
 * --------------------------------------------------------------------------- */

int fuse_vfs_opt_proc(void* data, const char* arg, int key,
                             struct fuse_args* outargs) {
    fuse_vfs_opts* opts = (fuse_vfs_opts*)data;
    (void)outargs;

    switch (key) {
    case KEY_VFS_PATH:
        free(opts->vfs_path);
        opts->vfs_path = strdup(arg);
        return 0;

    case KEY_EPOCH:
        opts->epoch = (int64_t)strtoll(arg, NULL, 10);
        return 0;

    case KEY_PAGE_SIZE:
        opts->page_size = (int64_t)strtoll(arg, NULL, 10);
        return 0;

    case KEY_READONLY:
        opts->readonly = 1;
        return 0;

    case KEY_HELP:
        fprintf(stderr,
            "Usage: vfs_fuse <vfs-file> <mountpoint> [options]\n"
            "FUSE options:\n"
            "  -o epoch=N        initial epoch (0 = base, default)\n"
            "  -o page_size=N    VFS page size (default 8192)\n"
            "  -o readonly       mount read-only\n"
            "  -o allow_other    allow other users (libfuse built-in)\n");
        return 0;

    case FUSE_OPT_KEY_NONOPT:
        /* Positional argument — first non-option is the VFS file path */
        if (!opts->vfs_path) {
            opts->vfs_path = strdup(arg);
            return 0;
        }
        return 1;  /* second positional = mountpoint, pass to libfuse */

    default:
        return 1;  /* pass to libfuse */
    }
}

/* ---------------------------------------------------------------------------
 * FUSE init callback — called by libfuse at mount time.
 * Receives fuse_vfs_opts* via fuse_get_context()->private_data,
 * heap-allocates a fuse_vfs_state_t, mounts the VFS, and returns the
 * state as the new private_data for all subsequent FUSE callbacks.
 * --------------------------------------------------------------------------- */

void* fuse_vfs_init(struct fuse_conn_info* conn, struct fuse_config* cfg) {
    (void)conn;
    (void)cfg;

    fuse_vfs_opts* opts = (fuse_vfs_opts*)fuse_get_context()->private_data;
    if (!opts || !opts->vfs_path) return NULL;

    fuse_vfs_state_t* state = (fuse_vfs_state_t*)calloc(1, sizeof(fuse_vfs_state_t));
    if (!state) return NULL;

    /* Take ownership of the VFS path string */
    state->vfs_path  = opts->vfs_path;
    opts->vfs_path   = NULL;  /* opts no longer owns it */

    state->epoch     = opts->epoch;
    state->page_size = (opts->page_size > 0) ? opts->page_size : 8192;
    state->readonly  = (opts->readonly != 0);

    /* Mount the VFS */
    state->vfs = vfs_mount(state->vfs_path, state->page_size);
    if (!state->vfs) {
        free(state->vfs_path);
        free(state);
        return NULL;
    }

    return state;
}

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
