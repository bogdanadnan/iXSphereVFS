#include "ixsphere/vfs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    vfs_t* vfs = vfs_mount(argv[1], 8192);
    if (!vfs) { fprintf(stderr, "mount fail\n"); return 1; }
    vfs_flush(vfs); vfs_unmount(vfs);
    vfs = vfs_mount(argv[1], 8192);

    int64_t root = vfs_root(vfs);
    int64_t f = vfs_create(vfs, root, "grow.dat", 0);
    fprintf(stderr, "create = %lld\n", (long long)f);
    if (f <= 0) return 1;

    /* write 1 MB */
    char* buf = malloc(1024*1024); memset(buf, 0xAB, 1024*1024);
    int r = vfs_write(vfs, f, buf, 0, 1024*1024, 0);
    fprintf(stderr, "write 1MB = %d\n", r);
    int64_t sz = vfs_file_size(vfs, f, 0);
    fprintf(stderr, "size after 1MB write = %lld\n", (long long)sz);
    free(buf);

    /* grow to 4MB via vfs_truncate */
    fprintf(stderr, "truncating to 4MB...\n");
    r = vfs_truncate(vfs, f, 4*1024*1024, 0);
    fprintf(stderr, "truncate to 4MB = %d\n", r);
    sz = vfs_file_size(vfs, f, 0);
    fprintf(stderr, "size = %lld\n", (long long)sz);

    /* grow to 8MB */
    fprintf(stderr, "truncating to 8MB...\n");
    r = vfs_truncate(vfs, f, 8*1024*1024, 0);
    fprintf(stderr, "truncate to 8MB = %d\n", r);
    sz = vfs_file_size(vfs, f, 0);
    fprintf(stderr, "size = %lld\n", (long long)sz);

    vfs_unmount(vfs);
    return 0;
}