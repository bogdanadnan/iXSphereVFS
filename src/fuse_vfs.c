/* FUSE-based filesystem interface for iXSphereVFS.
* Conditionally built when FUSE3 is available.
 * Exposes a VFS mount as a FUSE filesystem.
 */

#define FUSE_USE_VERSION 317
#define FUSE_DARWIN_ENABLE_EXTENSIONS 1

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef FUSE3_FOUND
#include <fuse.h>
#include <fuse_lowlevel.h>
#include <stdio.h>
#endif

#include "ixsphere/vfs.h"
#include "ixsphere/vfs_internal.h"
#include "fuse_vfs.h"

/* ---------------------------------------------------------------------------
 * Per-mount debug logging — toggled via env var FUSE_VFS_LOG (set to a
 * substring of the callback name to enable; "*" for everything).  Used to
 * diagnose the cp -R persistence bug.  Off in production (no env var).
 * --------------------------------------------------------------------------- */
static int fv_log_enabled(const char* what) {
    const char* env = getenv("FUSE_VFS_LOG");
    if (!env || !*env) return 0;
    if (env[0] == '*' && env[1] == 0) return 1;
    return strstr(env, what) != NULL;
}
#define FV_LOG(what, fmt, ...) do { \
    if (fv_log_enabled(what)) fprintf(stderr, "[fvfs] %s " fmt "\n", what, ##__VA_ARGS__); \
} while (0)

/* ---------------------------------------------------------------------------
 * FUSE option parsing keys — custom keys for -o epoch=, -o page_size=,
 * -o readonly.  allow_other is handled by libfuse's built-in parser.
 * --------------------------------------------------------------------------- */

#ifdef FUSE3_FOUND

#include <inttypes.h>

/* Custom option keys (negative to avoid collision with FUSE_OPT_KEY_*). */
enum {
    KEY_VFS_PATH   = -10,
    KEY_EPOCH      = -20,
    KEY_PAGE_SIZE  = -30,
    KEY_READONLY   = -40,
    KEY_HELP       = -50,
};

const struct fuse_opt fuse_vfs_opts_spec[] = {
    { "--vfs-file=%s", 0, KEY_VFS_PATH },
    FUSE_OPT_KEY("epoch=",              KEY_EPOCH),
    FUSE_OPT_KEY("page_size=",          KEY_PAGE_SIZE),
    FUSE_OPT_KEY("readonly",            KEY_READONLY),
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

    case KEY_EPOCH: {
        /* arg may be either the full form (e.g. "epoch=1") when
           matched via FUSE_OPT_KEY, or just the value when libfuse
           already stripped the prefix.  Strip the prefix if present. */
        const char* p = strchr(arg, '=');
        opts->epoch = (int64_t)strtoll(p ? p + 1 : arg, NULL, 10);
        return 0;
    }

    case KEY_PAGE_SIZE: {
        const char* p = strchr(arg, '=');
        opts->page_size = (int64_t)strtoll(p ? p + 1 : arg, NULL, 10);
        return 0;
    }

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
    /* Disable libfuse's auto_cache: macFUSE 3.18's update_stat path calls
       get_node(f, ino) which aborts if the inode isn't in libfuse's
       internal node tree.  Truncate/setattr replies crash the daemon
       in that case.  Disabling auto_cache routes attribute updates
       through the regular getattr reply path which doesn't abort. */
    if (cfg) cfg->auto_cache = 0;
    cfg->nullpath_ok = 1;
    /* Allow ioctl on directories (snapshot/commit/gc commands) */
    conn->want |= FUSE_CAP_IOCTL_DIR;

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

    /* Initialize control-file buffer + lock */
    fuse_vfs_ctl_init(state);

    /* Mount the VFS */
    state->vfs = vfs_mount(state->vfs_path, state->page_size);
    if (!state->vfs) {
        fuse_vfs_ctl_destroy(state);
        free(state->vfs_path);
        free(state);
        return NULL;
    }

    return state;
}

/* ---------------------------------------------------------------------------
 * FUSE destroy callback — called by libfuse at unmount time.
 * Unmounts the VFS and frees all resources.
 * --------------------------------------------------------------------------- */

void fuse_vfs_destroy(void* private_data) {
    fuse_vfs_state_t* state = (fuse_vfs_state_t*)private_data;
    if (!state) return;

    /* FUSE does not call .flush on umount.  Flush all dirty pool
       metadata pages + storage cache before unmount so writes survive
       a SIGKILL or unexpected termination. */
    if (state->vfs) {
        vfs_flush(state->vfs);
        vfs_unmount(state->vfs);
    }
    fuse_vfs_ctl_destroy(state);
    free(state->vfs_path);
    free(state);
}

/* ---------------------------------------------------------------------------
 * FUSE operations — each callback retrieves the per-mount state via
 * fuse_get_context()->private_data and delegates to the VFS API.
 * --------------------------------------------------------------------------- */

int fuse_vfs_getattr(const char* path, struct fuse_darwin_attr* stbuf,
                     struct fuse_file_info* fi) {
    fuse_vfs_state_t* state = (fuse_vfs_state_t*)fuse_get_context()->private_data;
    if (!state) return -EIO;
    FV_LOG("getattr", "path=%s%s%s", path ? path : "(null)",
          fi ? "" : " no-fi", (path && fi) ? " w-fi" : "");
    memset(stbuf, 0, sizeof(struct fuse_darwin_attr));
    time_t now = time(NULL);
    stbuf->uid = getuid();
    stbuf->gid = getgid();
    stbuf->atimespec.tv_sec = now;
    stbuf->atimespec.tv_nsec = 0;
    stbuf->btimespec = stbuf->atimespec;

    /* When macFUSE calls getattr after setattr (ftruncate path), path
       may be NULL and the VP is conveyed via fi->fh. */
    int64_t vp = 0;
    if (!path || path[0] == '\0') {
        if (fi) {
            vp = (int64_t)fi->fh;
        } else {
            /* No path and no fi — return root attribtes as sentinel. */
            stbuf->mode = S_IFDIR | 0755;
            stbuf->nlink = 2;
            stbuf->mtimespec = stbuf->atimespec;
            stbuf->ctimespec = stbuf->atimespec;
            return 0;
        }
    } else if (strcmp(path, "/") == 0) {
        stbuf->mode = S_IFDIR | 0755;
        stbuf->nlink = 2;
        stbuf->mtimespec = stbuf->atimespec;
        stbuf->ctimespec = stbuf->atimespec;
        return 0;
    } else if (fuse_vfs_is_ctl_path(path)) {
        /* Synthetic file — used by vfsctl to send control commands
           (substitute for ioctl).  Size tracks the latest response. */
        fuse_vfs_ctl_getattr(stbuf, state);
        return 0;
    } else {
        vp = resolve_full_path(state->vfs, state->epoch, path);
    }
    if (vp <= 0) return vfs_error_to_errno(vfs_last_error(state->vfs));

    if (fuse_is_dir(state->vfs, vp)) {
        stbuf->mode = S_IFDIR | 0755;
        stbuf->nlink = 2;
        stbuf->mtimespec = stbuf->atimespec;
        stbuf->ctimespec = stbuf->atimespec;
    } else {
        stbuf->mode = S_IFREG | 0644;
        stbuf->nlink = 1;
        stbuf->size  = vfs_file_size(state->vfs, vp, state->epoch);
        stbuf->mtimespec.tv_sec = (long)vfs_file_mtime(state->vfs, vp, state->epoch);
        stbuf->mtimespec.tv_nsec = 0;
        stbuf->ctimespec.tv_sec = (long)vfs_file_ctime(state->vfs, vp);
        stbuf->ctimespec.tv_nsec = 0;
    }
    stbuf->blksize = 0;
    return 0;
}

int fuse_vfs_readdir(const char* path, void* buf, fuse_darwin_fill_dir_t filler,
                     off_t offset, struct fuse_file_info* fi,
                     enum fuse_readdir_flags flags) {
    (void)offset; (void)fi; (void)flags;
    fuse_vfs_state_t* state = (fuse_vfs_state_t*)fuse_get_context()->private_data;
    if (!state || !state->vfs) return -EIO;
    FV_LOG("readdir", "path=%s", path ? path : "(null)");

    /* Path may be NULL (highlevel libfuse passes NULL for root when
       cfg->nullpath_ok is set).  Treat NULL or "/" as the root. */
    int64_t dir_vp = 0;
    if (path && path[0] != '\0' && strcmp(path, "/") != 0) {
        /* Non-root directory: walk tree at state->epoch (snapshot-aware).
           Falls back to root if path resolution fails. */
        dir_vp = resolve_full_path(state->vfs, state->epoch, path);
        if (dir_vp <= 0) {
            dir_vp = vfs_root(state->vfs);
        }
    } else if (fi && fi->fh != 0) {
        /* Path is NULL or "/" but fi->fh has the VP from opendir. */
        dir_vp = (int64_t)fi->fh;
    } else {
        dir_vp = vfs_root(state->vfs);
    }
    if (dir_vp <= 0) return vfs_error_to_errno(vfs_last_error(state->vfs));

    struct fuse_darwin_attr dummy_attr;
    memset(&dummy_attr, 0, sizeof(dummy_attr));
    filler(buf, ".", &dummy_attr, 0, 0);
    filler(buf, "..", &dummy_attr, 0, 0);

    vfs_dirent_t ents[64];
    int n = vfs_readdir(state->vfs, dir_vp, ents, 64, state->epoch);
    if (n < 0) {
        return vfs_error_to_errno(vfs_last_error(state->vfs));
    }
    for (int i = 0; i < n; i++) {
        if (filler(buf, ents[i].name, &dummy_attr, 0, 0)) break;
    }

    return 0;
}

/* Magic sentinel for the synthetic control-file fd.  Any real VFS
   VirtualPtr is non-negative, so UINT64_MAX is safe and cannot collide. */
#define FUSE_VFS_CTL_FH_SENTINEL  ((uint64_t)-1)

int fuse_vfs_open(const char* path, struct fuse_file_info* fi) {
    fuse_vfs_state_t* state = (fuse_vfs_state_t*)fuse_get_context()->private_data;

    /* Control file: synthetic, no VFS state required.  open always
       succeeds with no per-fd allocation. */
    if (fuse_vfs_is_ctl_path(path)) {
        if (state->readonly && (fi->flags & (O_WRONLY | O_RDWR)))
            return -EROFS;
        fi->fh = FUSE_VFS_CTL_FH_SENTINEL;
        return 0;
    }

    int64_t vp = 0;
    /* With cfg->nullpath_ok=1, macFUSE may call open with NULL path when
       the kernel already has an inode handle.  Prefer fi->fh if set. */
    if (!path || path[0] == '\0') {
        if (fi && fi->fh) {
            vp = (int64_t)fi->fh;
        } else {
            vp = vfs_root(state->vfs);
        }
    } else {
        vp = resolve_full_path(state->vfs, state->epoch, path);
    }
    if (vp <= 0) return vfs_error_to_errno(vfs_last_error(state->vfs));
    if (fuse_is_dir(state->vfs, vp)) return -EISDIR;
    if (state->readonly && (fi->flags & (O_WRONLY | O_RDWR)))
        return -EROFS;

    /* Acquire per-file lock for write opens */
    if (fi->flags & (O_WRONLY | O_RDWR)) {
        int lr = vfs_lock(state->vfs, vp, vfs_current_epoch(state->vfs));
        if (lr != VFS_OK) return -EACCES;
    }

    /* O_TRUNC handling: macFUSE will call our setattr after open.
       Don't truncate inline here — setattr is the canonical path
       now that auto_cache is disabled (no abort). */
    fi->fh = (uint64_t)vp;
    return 0;
}

int fuse_vfs_read(const char* path, char* buf, size_t size,
                  off_t offset, struct fuse_file_info* fi) {
    fuse_vfs_state_t* state = (fuse_vfs_state_t*)fuse_get_context()->private_data;
    if (fuse_vfs_is_ctl_path(path) ||
        (fi && fi->fh == FUSE_VFS_CTL_FH_SENTINEL)) {
        int r = fuse_vfs_ctl_read(state, buf, size, offset);
        return (r >= 0) ? r : vfs_error_to_errno(-r);
    }
    int64_t vp = (int64_t)fi->fh;
    int r = vfs_read(state->vfs, vp, buf, (int64_t)offset,
                     (int64_t)size, state->epoch);
    return (r >= 0) ? r : vfs_error_to_errno(vfs_last_error(state->vfs));
}

int fuse_vfs_write(const char* path, const char* buf, size_t size,
                   off_t offset, struct fuse_file_info* fi) {
    fuse_vfs_state_t* state = (fuse_vfs_state_t*)fuse_get_context()->private_data;
    if (state->readonly) return -EROFS;
    if (fuse_vfs_is_ctl_path(path) ||
        (fi && fi->fh == FUSE_VFS_CTL_FH_SENTINEL)) {
        int r = fuse_vfs_ctl_write(state, buf, size, offset);
        return (r >= 0) ? r : vfs_error_to_errno(-r);
    }
    FV_LOG("write", "vp=%ld off=%lld sz=%zu%s%s", (long)fi->fh,
          (long long)offset, size,
          fi ? "" : " no-fi",
          (size > 10000) ? " (large)" : "");
    int64_t vp = (int64_t)fi->fh;
    /* Writes always target the writable base epoch (0), even if the
       mount was opened at a snapshot epoch (odd).  Snapshot data is
       read-only; all mutations go to the current base. */
    int r = vfs_write(state->vfs, vp, buf, (int64_t)offset,
                      (int64_t)size, vfs_current_epoch(state->vfs));
    return (r >= 0) ? r : vfs_error_to_errno(vfs_last_error(state->vfs));
}

#ifdef __APPLE__
int fuse_vfs_create(const char* path, mode_t mode,
                    struct fuse_file_info* fi
#ifdef __APPLE__
                    , uint32_t flags
#endif
                    ) {
    (void)mode;
#ifdef __APPLE__
    (void)flags;
#endif
    (void)mode; (void)flags;
#else
int fuse_vfs_create(const char* path, mode_t mode,
                    struct fuse_file_info* fi) {
    (void)mode;
#endif
    FV_LOG("create", "%s", path);
    fuse_vfs_state_t* state = (fuse_vfs_state_t*)fuse_get_context()->private_data;
    if (state->readonly) return -EROFS;
    /* Resolve parent directory */
    char* path_copy = strdup(path);
    if (!path_copy) return -ENOMEM;
    char* last_slash = strrchr(path_copy, '/');
    const char* name;
    int64_t parent_vp;
    if (last_slash && last_slash != path_copy) {
        *last_slash = '\0';
        name = last_slash + 1;
        parent_vp = resolve_full_path(state->vfs, state->epoch, path_copy);
    } else {
        name = path + 1;  /* "/file" → skip leading slash */
        parent_vp = vfs_root(state->vfs);
    }
    if (parent_vp <= 0) { free(path_copy); return -ENOENT; }

    int64_t vp = vfs_create(state->vfs, parent_vp, name,
                              vfs_current_epoch(state->vfs));
    free(path_copy);
    if (vp <= 0) return vfs_error_to_errno(vfs_last_error(state->vfs));

    /* Acquire lock on the newly created file */
    if (vfs_lock(state->vfs, vp, state->epoch) != VFS_OK)
        return -EACCES;

    fi->fh = (uint64_t)vp;
    return 0;
}

int fuse_vfs_unlink(const char* path) {
    fuse_vfs_state_t* state = (fuse_vfs_state_t*)fuse_get_context()->private_data;
    if (state->readonly) return -EROFS;
    FV_LOG("unlink", "%s", path);
    /* Find parent */
    char* path_copy = strdup(path);
    if (!path_copy) return -ENOMEM;
    char* last_slash = strrchr(path_copy, '/');
    const char* name;
    int64_t parent_vp;
    if (last_slash && last_slash != path_copy) {
        *last_slash = '\0';
        name = last_slash + 1;
        parent_vp = resolve_full_path(state->vfs, state->epoch, path_copy);
    } else {
        name = path + 1;
        parent_vp = vfs_root(state->vfs);
    }
    if (parent_vp <= 0) { free(path_copy); return -ENOENT; }

    int ret = vfs_delete(state->vfs, parent_vp, name,
                        vfs_current_epoch(state->vfs));
    free(path_copy);
    return (ret == VFS_OK) ? 0 : vfs_error_to_errno(vfs_last_error(state->vfs));
}

int fuse_vfs_mkdir(const char* path, mode_t mode) {
    (void)mode;
    fuse_vfs_state_t* state = (fuse_vfs_state_t*)fuse_get_context()->private_data;
    if (state->readonly) return -EROFS;
    FV_LOG("mkdir", "%s", path);
    char* path_copy = strdup(path);
    if (!path_copy) return -ENOMEM;
    char* last_slash = strrchr(path_copy, '/');
    const char* name;
    int64_t parent_vp;
    if (last_slash && last_slash != path_copy) {
        *last_slash = '\0';
        name = last_slash + 1;
        parent_vp = resolve_full_path(state->vfs, state->epoch, path_copy);
    } else {
        name = path + 1;
        parent_vp = vfs_root(state->vfs);
    }
    if (parent_vp <= 0) { free(path_copy); return -ENOENT; }

    int64_t vp = vfs_mkdir(state->vfs, parent_vp, name,
                             vfs_current_epoch(state->vfs));
    free(path_copy);
    return (vp > 0) ? 0 : vfs_error_to_errno(vfs_last_error(state->vfs));
}

int fuse_vfs_rmdir(const char* path) {
    fuse_vfs_state_t* state = (fuse_vfs_state_t*)fuse_get_context()->private_data;
    if (state->readonly) return -EROFS;
    FV_LOG("rmdir", "%s", path);
    char* path_copy = strdup(path);
    if (!path_copy) return -ENOMEM;
    char* last_slash = strrchr(path_copy, '/');
    const char* name;
    int64_t parent_vp;
    if (last_slash && last_slash != path_copy) {
        *last_slash = '\0';
        name = last_slash + 1;
        parent_vp = resolve_full_path(state->vfs, state->epoch, path_copy);
    } else {
        name = path + 1;
        parent_vp = vfs_root(state->vfs);
    }
    if (parent_vp <= 0) { free(path_copy); return -ENOENT; }

    int ret = vfs_rmdir(state->vfs, parent_vp, name,
                        vfs_current_epoch(state->vfs));
    free(path_copy);
    return (ret == VFS_OK) ? 0 : vfs_error_to_errno(vfs_last_error(state->vfs));
}

int fuse_vfs_rename(const char* from, const char* to, unsigned int flags) {
    (void)flags;
    fuse_vfs_state_t* state = (fuse_vfs_state_t*)fuse_get_context()->private_data;
    if (state->readonly) return -EROFS;
    FV_LOG("rename", "%s -> %s", from, to);

    char* from_copy = strdup(from);
    char* to_copy   = strdup(to);
    if (!from_copy || !to_copy) { free(from_copy); free(to_copy); return -ENOMEM; }

    /* Split "from" into parent_path + src_name */
    char* from_slash = strrchr(from_copy, '/');
    if (!from_slash) { free(from_copy); free(to_copy); return -ENOENT; }
    *from_slash = '\0';
    const char* src_name = from_slash + 1;
    int64_t src_parent = (from_slash == from_copy)  /* "/name" → root */
                         ? vfs_root(state->vfs)
                         : resolve_full_path(state->vfs, state->epoch, from_copy);
    if (src_parent <= 0) { free(from_copy); free(to_copy); return -ENOENT; }

    /* Split "to" into parent_path + dst_name */
    char* to_slash = strrchr(to_copy, '/');
    if (!to_slash) { free(from_copy); free(to_copy); return -ENOENT; }
    *to_slash = '\0';
    const char* dst_name = to_slash + 1;
    int64_t dst_parent = (to_slash == to_copy)  /* "/name" → root */
                         ? vfs_root(state->vfs)
                         : resolve_full_path(state->vfs, state->epoch, to_copy);
    if (dst_parent <= 0) { free(from_copy); free(to_copy); return -ENOENT; }

    int ret = vfs_rename(state->vfs, src_parent, src_name, dst_parent, dst_name,
                         vfs_current_epoch(state->vfs));
    free(from_copy);
    free(to_copy);
    return (ret == VFS_OK) ? 0 : vfs_error_to_errno(vfs_last_error(state->vfs));
}

int fuse_vfs_truncate(const char* path, off_t size,
                      struct fuse_file_info* fi) {
    fuse_vfs_state_t* state = (fuse_vfs_state_t*)fuse_get_context()->private_data;
    if (state->readonly) return -EROFS;

    int64_t vp;
    if (path && path[0] != '\0') {
        vp = resolve_full_path(state->vfs, state->epoch, path);
    } else if (fi) {
        /* macFUSE ftruncate path: no path string, VP carried via fi->fh */
        vp = (int64_t)fi->fh;
    } else {
        return -ENOENT;
    }
    if (vp <= 0) return vfs_error_to_errno(vfs_last_error(state->vfs));

    int r = vfs_truncate(state->vfs, vp, (int64_t)size,
                         vfs_current_epoch(state->vfs));
    if (r != VFS_OK) return vfs_error_to_errno(vfs_last_error(state->vfs));
    return 0;
}

int fuse_vfs_utimens(const char* path, const struct timespec tv[2],
                     struct fuse_file_info* fi) {
    (void)path; (void)tv; (void)fi;
    /* VFS has no standalone timestamp-setting API — mtime is write-driven */
    return -ENOSYS;
}

/* ---------------------------------------------------------------------------
 * Placeholder callbacks — return -ENOSYS until Phases 6-10 implement them.
 * --------------------------------------------------------------------------- */

int fuse_vfs_opendir(const char* path, struct fuse_file_info* fi) {
    fuse_vfs_state_t* state = (fuse_vfs_state_t*)fuse_get_context()->private_data;
    int64_t vp = resolve_full_path(state->vfs, state->epoch, path);
    if (vp <= 0) return vfs_error_to_errno(vfs_last_error(state->vfs));
    if (!fuse_is_dir(state->vfs, vp)) return -ENOTDIR;
    fi->fh = (uint64_t)vp;
    return 0;
}
int fuse_vfs_releasedir(const char* path, struct fuse_file_info* fi)
    { (void)path; (void)fi; return 0; }
int fuse_vfs_release(const char* path, struct fuse_file_info* fi) {
    fuse_vfs_state_t* state = (fuse_vfs_state_t*)fuse_get_context()->private_data;
    if (fuse_vfs_is_ctl_path(path) ||
        (fi && fi->fh == FUSE_VFS_CTL_FH_SENTINEL)) {
        /* No per-fd state for the control file. */
        return 0;
    }
    FV_LOG("release", "vp=%ld flags=0x%x", (long)fi->fh, fi ? fi->flags : 0);
    /* Release the per-file lock.  vfs_unlock is not idempotent — it
       returns VFS_ERR_IO if no lock is held (e.g., read-only opens that
       never called vfs_lock).  We silently discard the return value:
       FUSE release must always succeed, and a failed unlock does not
       affect filesystem consistency. */
    int64_t vp = (int64_t)fi->fh;
    (void)vfs_unlock(state->vfs, vp, vfs_current_epoch(state->vfs));
    return 0;
}
int fuse_vfs_flush(const char* path, struct fuse_file_info* fi) {
    /* Per-file flush is a no-op.  FUSE calls .flush once per close, and
       each close was triggering a full cache_flush_all (every dirty page
       walked + mirror_write).  For workloads with many small files (e.g.
       zip extraction with 1000+ files), this caused thousands of full
       scans + lazy-mirror dance.

       Dirty pages now flow through cache_evict_batch (flushed + evicted
       at 100% cache usage, in batches of 25%).  Final durability is
       handled by fuse_vfs_destroy calling vfs_flush at unmount. */
    (void)path; (void)fi;
    return 0;
}
#ifdef __APPLE__
int fuse_vfs_statfs(const char* path, struct statfs* stbuf) {
#else
int fuse_vfs_statfs(const char* path, struct statvfs* stbuf) {
#endif
    (void)path;
    fuse_vfs_state_t* state = (fuse_vfs_state_t*)fuse_get_context()->private_data;
    memset(stbuf, 0, sizeof(*stbuf));
    stbuf->f_bsize   = (unsigned long)state->page_size;
#ifdef __APPLE__
    /* macOS struct statfs doesn't have f_frsize */
#else
    stbuf->f_frsize  = (unsigned long)state->page_size;
#endif

    /* The VFS is self-growing — the backing file expands as writes happen.
       Report a generous capacity (16 TiB) and subtract current usage
       from the free/avail figures so Finder doesn't treat us as full.
       Without this, f_blocks=0 makes Finder display "0 bytes available
       out of 0 bytes" and refuse all writes. */
    const uint64_t kTotalBytes = (uint64_t)16 * 1024 * 1024 * 1024 * 1024ULL;  /* 16 TiB */
    int64_t used_bytes = state->vfs ? state->vfs->ctx->sb->physical_tail : 0;
    uint64_t used_blks = (uint64_t)used_bytes / (uint64_t)state->page_size;
    stbuf->f_blocks  = kTotalBytes / (uint64_t)state->page_size;
    stbuf->f_bfree   = stbuf->f_blocks - used_blks;
    stbuf->f_bavail  = stbuf->f_bfree;
    stbuf->f_files   = UINT64_MAX;
    stbuf->f_ffree   = UINT64_MAX;
#ifdef __APPLE__
    /* macOS struct statfs doesn't have f_namemax */
#else
    stbuf->f_namemax = 255;
#endif
    return 0;
}
int fuse_vfs_access(const char* path, int mask) {
    (void)path;
    fuse_vfs_state_t* state = (fuse_vfs_state_t*)fuse_get_context()->private_data;
    /* VFS has no permission model — allow all checks by default.
       Only reject write access (W_OK) when mounted read-only. */
    if (state->readonly && (mask & W_OK)) return -EROFS;
    return 0;
}
int fuse_vfs_chmod(const char* path, mode_t m, struct fuse_file_info* fi)
    { (void)path; (void)m; (void)fi; return -ENOSYS; }
int fuse_vfs_chown(const char* path, uid_t u, gid_t g, struct fuse_file_info* fi)
    { (void)path; (void)u; (void)g; (void)fi; return -ENOSYS; }
int fuse_vfs_readlink(const char* path, char* buf, size_t size)
    { (void)path; (void)buf; (void)size; return -EINVAL; }
int fuse_vfs_symlink(const char* from, const char* to)
    { (void)from; (void)to; return -EPERM; }
int fuse_vfs_link(const char* from, const char* to)
    { (void)from; (void)to; return -EPERM; }
int fuse_vfs_setxattr(const char* path, const char* name,
                              const char* value, size_t size, int flags)
    { (void)path; (void)name; (void)value; (void)size; (void)flags; return -ENOSYS; }
int fuse_vfs_getxattr(const char* path, const char* name,
                              char* value, size_t size)
    { (void)path; (void)name; (void)value; (void)size; return -ENOSYS; }
int fuse_vfs_listxattr(const char* path, char* list, size_t size)
    { (void)path; (void)list; (void)size; return -ENOSYS; }
int fuse_vfs_removexattr(const char* path, const char* name)
    { (void)path; (void)name; return -ENOSYS; }

/* ---------------------------------------------------------------------------
 * Note on ioctl:
 *
 * The original FUSE interface registered an .ioctl callback so that
 * vfsctl could send VFS_IOC_* commands via ioctl() on a directory fd.
 * However, the macFUSE 3.18 kernel does not advertise FUSE_CAP_IOCTL_DIR
 * (it never sets FUSE_INIT_EXT in INIT flags), so even setting
 * conn->want |= FUSE_CAP_IOCTL_DIR has no effect on macOS.  ioctl() on
 * a FUSE fd returns ENOTTY without dispatching to userspace.
 *
 * The control-file protocol (src/fuse_vfs_ctl.c) replaces ioctl: vfsctl
 * opens "<mountpoint>/.vfsctl", writes a text command, and reads the
 * textual response.  This works on every FUSE version because it
 * uses regular read/write, not ioctl.
 *
 * If a future macFUSE release adds ioctl support, the dispatcher can
 * be restored from the vfsctl snapshot in commit history.
 * --------------------------------------------------------------------------- */

/* macFUSE 3.18 setattr handler.  Called for explicit truncate (ftruncate
   syscall) and implicit truncate (open with O_TRUNC).  We perform the
   truncation via vfs_truncate and rely on the regular getattr reply
   path (auto_cache is disabled in init) so no internal abort() is hit. */
int fuse_vfs_setattr(const char* path, struct fuse_darwin_attr* attr,
                     int to_set, struct fuse_file_info* fi) {
    fuse_vfs_state_t* state = (fuse_vfs_state_t*)fuse_get_context()->private_data;
    if (!state) return -EIO;
    if (state->readonly && (to_set & FUSE_SET_ATTR_SIZE)) return -EROFS;

    /* Return -ENOSYS for SIZE: libfuse falls through to its per-attribute
       handler (fuse_fs_truncate) which calls fuse_vfs_truncate.  With
       cfg->auto_cache=0 (set in init), the implicit getattr after
       our return uses the normal reply path without abort() on a missing
       node.  Returning 0 here causes macFUSE 3.18 to crash because
       the implicit getattr reply packs inconsistent attr fields. */
    if (to_set & FUSE_SET_ATTR_SIZE) return -ENOSYS;
    if (!attr) return -EIO;
    /* mode/uid/gid/atime/mtime/etc — accept silently */
    return 0;
}

/* ---------------------------------------------------------------------------
 * FUSE lookup — required by libfuse to resolve paths.
 * --------------------------------------------------------------------------- */

int fuse_vfs_lookup(fuse_ino_t parent, const char* name) {
    fuse_vfs_state_t* state = (fuse_vfs_state_t*)fuse_get_context()->private_data;
    int64_t vp = vfs_open(state->vfs, (int64_t)parent, name, state->epoch);
    if (vp <= 0) return -ENOENT;
    return 0;
}

/* ---------------------------------------------------------------------------
 * FUSE operations table — registered via fuse_main_real.
 * --------------------------------------------------------------------------- */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-function-pointer-types"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-function-pointer-types"
const struct fuse_operations fuse_vfs_ops = {
    .init        = fuse_vfs_init,
    .destroy     = fuse_vfs_destroy,
    .getattr     = fuse_vfs_getattr,
    .setattr     = fuse_vfs_setattr,
    .readdir     = fuse_vfs_readdir,
    .opendir     = fuse_vfs_opendir,
    .releasedir  = fuse_vfs_releasedir,
    .open        = fuse_vfs_open,
    .create      = fuse_vfs_create,
    .read        = fuse_vfs_read,
    .write       = fuse_vfs_write,
    .release     = fuse_vfs_release,
    .unlink      = fuse_vfs_unlink,
    .mkdir       = fuse_vfs_mkdir,
    .rmdir       = fuse_vfs_rmdir,
    .rename      = fuse_vfs_rename,
    .truncate    = fuse_vfs_truncate,
    .flush       = fuse_vfs_flush,
    .statfs      = fuse_vfs_statfs,
    .access      = fuse_vfs_access,
    .utimens     = fuse_vfs_utimens,
    .chmod       = fuse_vfs_chmod,
    .chown       = fuse_vfs_chown,
    /* .ioctl not registered — macFUSE 3.18 doesn't deliver ioctl to
       userspace daemons.  Use the control-file protocol via
       "<mountpoint>/.vfsctl" instead. */
    .readlink    = fuse_vfs_readlink,
    .symlink     = fuse_vfs_symlink,
    .link        = fuse_vfs_link,
    .setxattr    = fuse_vfs_setxattr,
    .getxattr    = fuse_vfs_getxattr,
    .listxattr   = fuse_vfs_listxattr,
    .removexattr = fuse_vfs_removexattr,
};
#pragma GCC diagnostic pop
#pragma GCC diagnostic pop

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

const char* vfs_error_to_str(int vfs_err) {
    switch (vfs_err) {
    case VFS_OK:            return "OK";
    case VFS_ERR_IO:        return "IO";
    case VFS_ERR_NOTFOUND:  return "not_found";
    case VFS_ERR_EXISTS:    return "exists";
    case VFS_ERR_NOTDIR:    return "not_a_directory";
    case VFS_ERR_NOTEMPTY:  return "not_empty";
    case VFS_ERR_CONFLICT:  return "conflict";
    case VFS_ERR_FULL:      return "full";
    case VFS_ERR_NOMEM:     return "out_of_memory";
    case VFS_ERR_EPOCH:     return "invalid_epoch";
    default:                return "unknown";
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

int64_t resolve_full_path(vfs_t* vfs, int64_t epoch, const char* path) {
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
        int64_t child = vfs_open(vfs, parent, token, epoch);
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
