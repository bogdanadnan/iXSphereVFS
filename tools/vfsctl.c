/* vfsctl — iXSphereVFS control tool: ioctl-based snapshot/commit/delete-snapshot/gc.
 *
 * Usage: vfsctl <subcommand> <mountpoint> [epoch]
 *
 * Operates on a FUSE mountpoint directory fd via ioctl.
 * VFS_IOC_* macros are defined in src/fuse_ioctl.h.
 */

#include "ixsphere/vfs.h"
#include "fuse_ioctl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

static void usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s <subcommand> <mountpoint> [epoch]\n"
        "Subcommands:\n"
        "  snapshot                  create a snapshot\n"
        "  commit <epoch>            commit a snapshot\n"
        "  delete-snapshot <epoch>   soft-delete a snapshot\n"
        "  gc                        run garbage collection\n",
        prog);
}

int main(int argc, char** argv) {
    if (argc < 3) { usage(argv[0]); return 1; }

    const char* subcmd     = argv[1];
    const char* mountpoint = argv[2];

    int fd = open(mountpoint, O_RDONLY);
    if (fd < 0) { perror(mountpoint); return 1; }

    if (strcmp(subcmd, "snapshot") == 0) {
        int64_t epoch = 0;
        if (ioctl(fd, VFS_IOC_SNAPSHOT, &epoch) != 0) {
            perror("ioctl(VFS_IOC_SNAPSHOT)");
            close(fd);
            return 1;
        }
        printf("%lld\n", (long long)epoch);
    } else if (strcmp(subcmd, "commit") == 0) {
        if (argc < 4) {
            fprintf(stderr, "commit requires an epoch argument\n");
            close(fd); return 1;
        }
        int64_t epoch = (int64_t)atoll(argv[3]);
        if (ioctl(fd, VFS_IOC_COMMIT, &epoch) != 0) {
            perror("ioctl(VFS_IOC_COMMIT)");
            close(fd);
            return 1;
        }
    } else if (strcmp(subcmd, "delete-snapshot") == 0) {
        if (argc < 4) {
            fprintf(stderr, "delete-snapshot requires an epoch argument\n");
            close(fd); return 1;
        }
        int64_t epoch = (int64_t)atoll(argv[3]);
        if (ioctl(fd, VFS_IOC_DELETE_SNAP, &epoch) != 0) {
            perror("ioctl(VFS_IOC_DELETE_SNAP)");
            close(fd);
            return 1;
        }
    } else if (strcmp(subcmd, "gc") == 0) {
        if (ioctl(fd, VFS_IOC_GC) != 0) {
            perror("ioctl(VFS_IOC_GC)");
            close(fd);
            return 1;
        }
    } else {
        fprintf(stderr, "unknown subcommand: %s\n", subcmd);
        usage(argv[0]);
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}
