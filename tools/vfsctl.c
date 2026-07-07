/* vfsctl — iXSphereVFS command-line control tool.
 * Inspect, modify, and manage VFS backing files.
 *
 * Usage: ./vfsctl <vfs-file> <command> [args...]
 * Commands:
 *   info         — print superblock information
 *   list         — list root directory contents
 *   create NAME  — create a file in root
 *   read  NAME   — read file contents (up to 4KB)
 */

#include "ixsphere/vfs.h"
#include "ixsphere/vfs_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s <vfs-file> <command> [args...]\n"
        "Commands:\n"
        "  info                show superblock info\n"
        "  list                list root directory\n"
        "  create <name>       create a file\n"
        "  read <name>         read file (up to 4096 bytes)\n",
        prog);
}

static int cmd_info(vfs_t* vfs) {
    printf("page_size:      %lld\n", (long long)vfs->ctx->page_size);
    printf("rootNodeOffset: %lld\n", (long long)vfs->ctx->rootNodeOffset);
    printf("currentEpoch:   %lld\n", (long long)vfs->ctx->currentEpoch);
    printf("nextNodeId:     %u\n", (unsigned)vfs->ctx->nextNodeId);
    printf("segment_size:   %u\n", (unsigned)vfs->ctx->segment_size);
    return 0;
}

static int cmd_list(vfs_t* vfs) {
    vfs_dirent_t ents[64];
    int n = vfs_readdir(vfs, vfs->ctx->rootNodeOffset, ents, 64, 0);
    if (n < 0) { fprintf(stderr, "readdir failed\n"); return 1; }
    printf("%d entries:\n", n);
    for (int i = 0; i < n; i++) {
        printf("  %s %s (vp=%lld, nodeId=%lld)\n",
               ents[i].isDir ? "[DIR]" : "[FILE]",
               ents[i].name,
               (long long)ents[i].vp,
               (long long)ents[i].nodeId);
    }
    return 0;
}

static int cmd_create(vfs_t* vfs, const char* name) {
    int64_t fvp = vfs_create(vfs, vfs->ctx->rootNodeOffset, name, 0);
    if (fvp <= 0) { fprintf(stderr, "create '%s' failed: %d\n", name, (int)fvp); return 1; }
    printf("created '%s' (vp=%lld)\n", name, (long long)fvp);
    return 0;
}

static int cmd_read(vfs_t* vfs, const char* name) {
    int64_t fvp = vfs_open(vfs, vfs->ctx->rootNodeOffset, name, 0);
    if (fvp <= 0) { fprintf(stderr, "file '%s' not found\n", name); return 1; }
    char buf[4096];
    int r = vfs_read(vfs, fvp, buf, 0, sizeof(buf), 0);
    if (r < 0) { fprintf(stderr, "read failed\n"); return 1; }
    if (r == 0) { printf("(empty)\n"); return 0; }
    printf("%.*s", r, buf);
    if (r > 0 && buf[r-1] != '\n') printf("\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) { usage(argv[0]); return 1; }
    const char* path = argv[1];
    const char* cmd  = argv[2];

    vfs_t* vfs = vfs_mount(path, 8192);
    if (!vfs) { fprintf(stderr, "failed to mount %s\n", path); return 1; }

    int ret = 1;
    if (strcmp(cmd, "info") == 0) {
        ret = cmd_info(vfs);
    } else if (strcmp(cmd, "list") == 0) {
        ret = cmd_list(vfs);
    } else if (strcmp(cmd, "create") == 0) {
        if (argc < 4) { fprintf(stderr, "missing name\n"); ret = 1; }
        else ret = cmd_create(vfs, argv[3]);
    } else if (strcmp(cmd, "read") == 0) {
        if (argc < 4) { fprintf(stderr, "missing name\n"); ret = 1; }
        else ret = cmd_read(vfs, argv[3]);
    } else {
        fprintf(stderr, "unknown command: %s\n", cmd);
        usage(argv[0]);
        ret = 1;
    }

    vfs_unmount(vfs);
    return ret;
}
