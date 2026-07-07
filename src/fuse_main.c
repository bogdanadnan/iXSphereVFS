/* vfs_fuse — FUSE filesystem interface for iXSphereVFS.
 * Conditionally built when FUSE3 is available.
 *
 * Usage: ./vfs_fuse <vfs-file> <mountpoint>
 *
 * Full FUSE callback implementation is deferred to Phase 5
 * (private_data wrapper, fuse_main_real, fuse_opt_parse, vfs_root,
 *  resolve_full_path, ops table in src/fuse_vfs.c).
 */

#include <stdio.h>

#ifdef FUSE3_FOUND
#include <fuse3/fuse.h>
#endif

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    fprintf(stderr, "vfs_fuse: pending Phase 5+ implementation\n");
    return 1;
}
