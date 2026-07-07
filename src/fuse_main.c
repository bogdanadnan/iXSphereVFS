/* vfs_fuse — FUSE filesystem interface for iXSphereVFS.
 * Mounts a VFS backing file as a FUSE filesystem.
 *
 * Usage: ./vfs_fuse <vfs-file> <mountpoint> [fuse-options...]
 *
 * Conditionally built when FUSE3 is available (see CMakeLists.txt).
 */

#include "ixsphere/vfs.h"
#include "ixsphere/vfs_internal.h"

#ifdef FUSE3_FOUND
#include <fuse3/fuse.h>
#include <fuse3/fuse_lowlevel.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FUSE3_FOUND

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    fprintf(stderr, "vfs_fuse: FUSE3 not available at build time.\n");
    return 1;
}

#else

/* ---------------------------------------------------------------------------
 * FUSE operations — full implementation planned for Phase 10.
 * For now, provides a minimal mount that surfaces VFS contents read-only.
 * --------------------------------------------------------------------------- */

static vfs_t* g_vfs = NULL;

static int vfs_fuse_getattr(const char* path, struct stat* stbuf,
                            struct fuse_file_info* fi) {
    (void)fi;
    memset(stbuf, 0, sizeof(struct stat));

    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }

    /* Look up file by name */
    int64_t fvp = vfs_open(g_vfs, g_vfs->ctx->rootNodeOffset, path + 1, 0);
    if (fvp <= 0) return -ENOENT;

    stbuf->st_mode = S_IFREG | 0644;
    stbuf->st_nlink = 1;
    stbuf->st_size = vfs_file_size(g_vfs, fvp, 0);
    return 0;
}

static int vfs_fuse_readdir(const char* path, void* buf,
                            fuse_fill_dir_t filler, off_t offset,
                            struct fuse_file_info* fi,
                            enum fuse_readdir_flags flags) {
    (void)offset; (void)fi; (void)flags;

    if (strcmp(path, "/") != 0) return -ENOENT;

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    vfs_dirent_t ents[64];
    int n = vfs_readdir(g_vfs, g_vfs->ctx->rootNodeOffset, ents, 64, 0);
    for (int i = 0; i < n; i++) {
        filler(buf, ents[i].name, NULL, 0, 0);
    }
    return 0;
}

static int vfs_fuse_open(const char* path, struct fuse_file_info* fi) {
    int64_t fvp = vfs_open(g_vfs, g_vfs->ctx->rootNodeOffset, path + 1, 0);
    if (fvp <= 0) return -ENOENT;
    fi->fh = (uint64_t)fvp;
    return 0;
}

static int vfs_fuse_read(const char* path, char* buf, size_t size,
                         off_t offset, struct fuse_file_info* fi) {
    (void)path;
    int64_t fvp = (int64_t)fi->fh;
    int r = vfs_read(g_vfs, fvp, buf, (int64_t)offset, (int64_t)size, 0);
    return r >= 0 ? r : -EIO;
}

static const struct fuse_operations vfs_fuse_ops = {
    .getattr = vfs_fuse_getattr,
    .readdir = vfs_fuse_readdir,
    .open    = vfs_fuse_open,
    .read    = vfs_fuse_read,
};

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <vfs-file> <mountpoint> [fuse-options...]\n",
                argv[0]);
        return 1;
    }

    const char* vfs_path = argv[1];
    const char* mnt_path = argv[2];

    g_vfs = vfs_mount(vfs_path, 8192);
    if (!g_vfs) {
        fprintf(stderr, "vfs_fuse: failed to mount %s\n", vfs_path);
        return 1;
    }

    /* Shift argv to pass remaining args to FUSE */
    argv[1] = argv[0];
    int fuse_ret = fuse_main(argc - 1, argv + 1, &vfs_fuse_ops, NULL);

    vfs_unmount(g_vfs);
    return fuse_ret;
}

#endif /* FUSE3_FOUND */
