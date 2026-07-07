#include "ixsphere/vfs.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <file.vfs>\n", argv[0]); return 1; }
    vfs_t* vfs = vfs_mount(argv[1], 8192);
    if (!vfs) { fprintf(stderr, "mount failed\n"); return 1; }

    int64_t root = vfs_root(vfs);
    int64_t f = vfs_create(vfs, root, "hello.txt", 0);
    if (f > 0) {
        const char* data = "Hello, iXSphereVFS!";
        vfs_write(vfs, f, data, 0, strlen(data), 0);
        fprintf(stderr, "Created hello.txt at VP %lld\n", (long long)f);
    }

    vfs_dirent_t ents[64];
    int n = vfs_readdir(vfs, root, ents, 64, 0);
    fprintf(stderr, "Root has %d entries:\n", n);
    for (int i = 0; i < n; i++) {
        fprintf(stderr, "  %s (vp=%lld, dir=%d)\n", ents[i].name,
                (long long)ents[i].vp, ents[i].isDir);
    }
    vfs_flush(vfs);
    vfs_unmount(vfs);
    fprintf(stderr, "OK\n");
    return 0;
}