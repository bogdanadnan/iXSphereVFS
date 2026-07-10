/* Standalone CLI tool to walk a .vfs file via the VFS API directly.
 * Use to verify readdir state without going through FUSE. */
#include "ixsphere/vfs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_errors = 0;
static int g_dirs_visited = 0;
static int g_files_seen = 0;

static int do_readdir(vfs_t* vfs, int64_t dir, int depth, const char* path) {
    vfs_dirent_t* ents = NULL;
    int n = 0;
    int rc = vfs_readdir(vfs, dir, &ents, &n, 0);
    if (rc != VFS_OK) {
        fprintf(stderr, "  %*s[ERR] vfs_readdir(%s) = %d, last_error=%d\n",
                depth * 2, "", path, rc, vfs_last_error(vfs));
        g_errors++;
        vfs_free_dirents(ents);
        return rc;
    }
    fprintf(stderr, "  %*s[%s] vfs_readdir returned %d entries\n",
            depth * 2, "", path, n);
    g_dirs_visited++;
    for (int i = 0; i < n; i++) {
        char child[512];
        if (strcmp(path, "/") == 0) {
            snprintf(child, sizeof(child), "/%s", ents[i].name);
        } else {
            snprintf(child, sizeof(child), "%s/%s", path, ents[i].name);
        }
        g_files_seen++;
        if (ents[i].isDir) {
            fprintf(stderr, "  %*s  [D] %s  vp=%lld\n",
                    depth * 2, "", ents[i].name, (long long)ents[i].vp);
            do_readdir(vfs, ents[i].vp, depth + 1, child);
        } else {
            fprintf(stderr, "  %*s  [F] %s  vp=%lld\n",
                    depth * 2, "", ents[i].name, (long long)ents[i].vp);
        }
    }
    vfs_free_dirents(ents);
    return n;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <vfs-file>\n", argv[0]);
        return 1;
    }
    vfs_t* vfs = vfs_mount(argv[1], 8192);
    if (!vfs) {
        fprintf(stderr, "vfs_mount failed for %s\n", argv[1]);
        return 1;
    }
    int64_t root = vfs_root(vfs);
    fprintf(stderr, "vfs_mount OK, root=%lld\n", (long long)root);
    do_readdir(vfs, root, 0, "/");
    fprintf(stderr, "\n=== summary: %d dirs visited, %d entries seen, %d errors ===\n",
            g_dirs_visited, g_files_seen, g_errors);
    vfs_unmount(vfs);
    return g_errors == 0 ? 0 : 2;
}
