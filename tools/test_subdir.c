#include "ixsphere/vfs.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    vfs_t* vfs = vfs_mount(argv[1], 8192);
    if (!vfs) { fprintf(stderr, "mount fail\n"); return 1; }

    int64_t root = vfs_root(vfs);

    /* Create dir */
    int64_t dir = vfs_mkdir(vfs, root, "dir1", 0);
    fprintf(stderr, "mkdir dir1 = %lld\n", (long long)dir);

    /* Create file in dir */
    int64_t f = vfs_create(vfs, dir, "file1.txt", 0);
    fprintf(stderr, "create file1.txt = %lld\n", (long long)f);
    if (f > 0) vfs_write(vfs, f, "content1", 0, 8, 0);

    vfs_flush(vfs);

    /* Now readdir root */
    vfs_dirent_t ents[64];
    int n = vfs_readdir(vfs, root, ents, 64, 0);
    fprintf(stderr, "readdir root: %d entries\n", n);
    for (int i = 0; i < n; i++) fprintf(stderr, "  %s (vp=%lld, isDir=%d)\n",
        ents[i].name, (long long)ents[i].vp, ents[i].isDir);

    /* Readdir dir1 */
    if (dir > 0) {
        n = vfs_readdir(vfs, dir, ents, 64, 0);
        fprintf(stderr, "readdir dir1: %d entries\n", n);
        for (int i = 0; i < n; i++) fprintf(stderr, "  %s (vp=%lld)\n",
            ents[i].name, (long long)ents[i].vp);
    }

    /* Read file1.txt */
    char buf[64] = {0};
    if (f > 0) {
        int r = vfs_read(vfs, f, buf, 0, 8, 0);
        fprintf(stderr, "read file1.txt: %d bytes: '%s'\n", r, buf);
    }

    vfs_unmount(vfs);
    return 0;
}