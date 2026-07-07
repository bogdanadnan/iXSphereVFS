/* FUSE-based filesystem interface for iXSphereVFS.
* Conditionally built when FUSE3 is available.
 * Exposes a VFS mount as a FUSE filesystem.
 */

#define FUSE_USE_VERSION 317
#define FUSE_DARWIN_ENABLE_EXTENSIONS 1

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef FUSE3_FOUND
#include <fuse.h>
#include <stdio.h>
#endif

#include "ixsphere/vfs.h"
#include "fuse_vfs.h"

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

    cfg->nullpath_ok = 1;
    /* Allow ioctl on directories (snapshot/commit/gc commands) */
    conn->want |= FUSE_CAP_IOCTL_DIR;

    return state;
}

/* ---------------------------------------------------------------------------
 * FUSE destroy callback — called by libfuse at unmount time.
 * Unmounts the VFS and frees all resources.
 * --------------------------------------------------------------------------- */

void fuse_vfs_destroy(void* private_data) {
    fuse_vfs_state_t* state = (fuse_vfs_state_t*)private_data;
    if (!state) return;

    if (state->vfs) vfs_unmount(state->vfs);
    free(state->vfs_path);
    free(state);
}

/* ---------------------------------------------------------------------------
 * FUSE operations — each callback retrieves the per-mount state via
 * fuse_get_context()->private_data and delegates to the VFS API.
 * --------------------------------------------------------------------------- */

int fuse_vfs_getattr(const char* path, struct fuse_darwin_attr* stbuf,
                     struct fuse_file_info* fi) {
    (void)fi;
    fuse_vfs_state_t* state = (fuse_vfs_state_t*)fuse_get_context()->private_data;
    memset(stbuf, 0, sizeof(struct fuse_darwin_attr));
    time_t now = time(NULL);
    stbuf->uid = getuid();
    stbuf->gid = getgid();
    stbuf->atimespec.tv_sec = now;
    stbuf->atimespec.tv_nsec = 0;
    stbuf->btimespec = stbuf->atimespec;

    if (strcmp(path, "/") == 0) {
        stbuf->mode = S_IFDIR | 0755;
        stbuf->nlink = 2;
        stbuf->mtimespec = stbuf->atimespec;
        stbuf->ctimespec = stbuf->atimespec;
        return 0;
    }

    int64_t vp = resolve_full_path(state->vfs, state->epoch, path);
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

    /* Path may be NULL (highlevel libfuse passes NULL for root when
       cfg->nullpath_ok is set).  Treat NULL or "/" as the root. */
    int64_t dir_vp;
    if (!path || strcmp(path, "/") == 0) {
        dir_vp = vfs_root(state->vfs);
    } else {
        /* Non-root directory: walk tree at state->epoch (snapshot-aware).
           Falls back to root if path resolution fails. */
        dir_vp = resolve_full_path(state->vfs, state->epoch, path);
        if (dir_vp <= 0) {
            dir_vp = vfs_root(state->vfs);
        }
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

int fuse_vfs_open(const char* path, struct fuse_file_info* fi) {
    fuse_vfs_state_t* state = (fuse_vfs_state_t*)fuse_get_context()->private_data;
    int64_t vp = resolve_full_path(state->vfs, state->epoch, path);
    if (vp <= 0) return vfs_error_to_errno(vfs_last_error(state->vfs));
    if (fuse_is_dir(state->vfs, vp)) return -EISDIR;
    if (state->readonly && (fi->flags & (O_WRONLY | O_RDWR)))
        return -EROFS;

    /* Acquire per-file lock for write opens */
    if (fi->flags & (O_WRONLY | O_RDWR)) {
        int lr = vfs_lock(state->vfs, vp, vfs_current_epoch(state->vfs));
        if (lr != VFS_OK) return -EACCES;
    }

    fi->fh = (uint64_t)vp;
    return 0;
}

int fuse_vfs_read(const char* path, char* buf, size_t size,
                  off_t offset, struct fuse_file_info* fi) {
    (void)path;
    fuse_vfs_state_t* state = (fuse_vfs_state_t*)fuse_get_context()->private_data;
    int64_t vp = (int64_t)fi->fh;
    int r = vfs_read(state->vfs, vp, buf, (int64_t)offset,
                     (int64_t)size, state->epoch);
    return (r >= 0) ? r : vfs_error_to_errno(vfs_last_error(state->vfs));
}

int fuse_vfs_write(const char* path, const char* buf, size_t size,
                   off_t offset, struct fuse_file_info* fi) {
    (void)path;
    fuse_vfs_state_t* state = (fuse_vfs_state_t*)fuse_get_context()->private_data;
    if (state->readonly) return -EROFS;
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
    (void)fi;
    fuse_vfs_state_t* state = (fuse_vfs_state_t*)fuse_get_context()->private_data;
    if (state->readonly) return -EROFS;

    int64_t vp = resolve_full_path(state->vfs, state->epoch, path);
    if (vp <= 0) return vfs_error_to_errno(vfs_last_error(state->vfs));

    int64_t cur_size = vfs_file_size(state->vfs, vp, state->epoch);
    if ((int64_t)size < cur_size) return -ENOSYS;  /* shrink not supported */

    /* Grow: write zeros in 1 MiB chunks from current size to target */
    uint8_t zbuf[1048576];  /* 1 MiB */
    memset(zbuf, 0, sizeof(zbuf));
    int64_t remaining = (int64_t)size - cur_size;
    int64_t wr_off = cur_size;
    while (remaining > 0) {
        int64_t chunk = (remaining < (int64_t)sizeof(zbuf)) ? remaining
                                                             : (int64_t)sizeof(zbuf);
        int r = vfs_write(state->vfs, vp, zbuf, wr_off, chunk,
                          vfs_current_epoch(state->vfs));
        if (r < 0) return vfs_error_to_errno(vfs_last_error(state->vfs));
        remaining -= chunk;
        wr_off    += chunk;
    }
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
    (void)path;
    fuse_vfs_state_t* state = (fuse_vfs_state_t*)fuse_get_context()->private_data;
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
    (void)fi;
    fuse_vfs_state_t* state = (fuse_vfs_state_t*)fuse_get_context()->private_data;
    int64_t vp = resolve_full_path(state->vfs, state->epoch, path);
    if (vp <= 0) return vfs_error_to_errno(vfs_last_error(state->vfs));
    int r = vfs_flush(state->vfs);
    return (r == VFS_OK) ? 0 : vfs_error_to_errno(vfs_last_error(state->vfs));
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
    stbuf->f_blocks  = 0;
    stbuf->f_bfree   = 0;
    stbuf->f_bavail  = 0;
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
 * ioctl dispatcher — maps VFS_IOC_* commands to VFS API calls.
 * --------------------------------------------------------------------------- */

#include "fuse_ioctl.h"

int fuse_vfs_ioctl(vfs_t* vfs, unsigned long request, void* arg, void* data) {
    if (!vfs) return -EIO;

    switch (request) {
    case VFS_IOC_SNAPSHOT: {
        int64_t epoch = vfs_snapshot(vfs);
        if (epoch < 0) return vfs_error_to_errno(vfs_last_error(vfs));
        /* Write result to data (kernel output buffer for _IOR) */
        if (data) *(int64_t*)data = epoch;
        return 0;
    }
    case VFS_IOC_COMMIT: {
        if (!data) return -EINVAL;
        int64_t snap_epoch = *(int64_t*)data;
        if (snap_epoch <= 0) return -EINVAL;
        int ret = vfs_commit(vfs, snap_epoch);
        if (ret != VFS_OK) return vfs_error_to_errno(vfs_last_error(vfs));
        /* Write back the committed epoch to data (_IOWR) */
        *(int64_t*)data = vfs_current_epoch(vfs);
        return 0;
    }
    case VFS_IOC_DELETE_SNAP: {
        if (!data) return -EINVAL;
        int64_t snap_epoch = *(int64_t*)data;
        if (snap_epoch <= 0) return -EINVAL;
        int ret = vfs_delete_snapshot(vfs, snap_epoch);
        return (ret == VFS_OK) ? 0
               : vfs_error_to_errno(vfs_last_error(vfs));
    }
    case VFS_IOC_GC: {
        int ret = vfs_gc(vfs);
        return (ret == VFS_OK) ? 0
               : vfs_error_to_errno(vfs_last_error(vfs));
    }
    default:
        return -ENOTTY;
    }
}

/* ---------------------------------------------------------------------------
 * FUSE ioctl callback — bridges the FUSE ioctl path to our dispatcher.
 * --------------------------------------------------------------------------- */

int fuse_vfs_ioctl_cb(const char* path, int cmd, void* arg,
                      struct fuse_file_info* fi, unsigned int flags,
                      void* data) {
    (void)path; (void)fi; (void)flags;
    fuse_vfs_state_t* state = (fuse_vfs_state_t*)fuse_get_context()->private_data;
    return fuse_vfs_ioctl(state->vfs, (unsigned long)cmd, arg, data);
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
    .ioctl       = fuse_vfs_ioctl_cb,  /* Phase 10 */
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
