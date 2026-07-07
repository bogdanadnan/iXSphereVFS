/* vfs_fuse — FUSE filesystem interface for iXSphereVFS.
 * Conditionally built when FUSE3 is available.
 *
 * Usage: ./vfs_fuse <vfs-file> <mountpoint> [-o options...]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef FUSE3_FOUND
#include <fuse.h>
#endif

#include "ixsphere/vfs.h"
#include "fuse_vfs.h"

#ifndef FUSE3_FOUND

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    fprintf(stderr, "vfs_fuse: FUSE3 not available at build time.\n");
    return 1;
}

#else

/* FUSE operations table — defined in src/fuse_vfs.c */
extern const struct fuse_operations fuse_vfs_ops;

int main(int argc, char** argv) {
    /* Default option state */
    fuse_vfs_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.page_size = 8192;

    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    /* Parse custom FUSE options into opts */
    if (fuse_opt_parse(&args, &opts, fuse_vfs_opts_spec,
                       fuse_vfs_opt_proc) != 0) {
        free(opts.vfs_path);
        fuse_opt_free_args(&args);
        return 1;
    }

    /* Pass opts as private_data to libfuse's fuse_main.
       fuse_main is the high-level API that handles the daemonize (-f)
       and child-mount-fork logic. */
    int ret = fuse_main(args.argc, args.argv, &fuse_vfs_ops, &opts);

    free(opts.vfs_path);
    fuse_opt_free_args(&args);
    return ret;
}

#endif /* FUSE3_FOUND */
