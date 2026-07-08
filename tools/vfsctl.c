/* vfsctl — iXSphereVFS control tool.
 *
 * Usage: vfsctl <subcommand> <mountpoint> [epoch]
 *
 * Subcommands:
 *   snapshot                  create a snapshot
 *   commit <epoch>            commit a snapshot
 *   delete-snapshot <epoch>   soft-delete a snapshot
 *   gc                        run garbage collection
 *
 * This program talks to the daemon over a **synthetic control file**
 * ("/.vfsctl") inside the FUSE mount, not via ioctl:
 *
 *   $ open("/mountpoint/.vfsctl", O_RDWR)
 *   $ write(fd, "snapshot\n", 10)
 *   $ read(fd, buf, 64)           ; receives the new epoch
 *   $ close(fd)
 *
 * macFUSE 3.18 (and most kernel-level ioctl stories) does NOT deliver
 * ioctl() on a directory fd to the user-space daemon — the kernel does
 * not advertise FUSE_CAP_IOCTL_DIR, so even setting the want on our
 * side has no effect and ioctl() returns ENOTTY.  The control-file
 * protocol uses ordinary FUSE read/write, which works on every FUSE
 * version that supports open + read + write on regular files
 * (i.e., everything macFUSE/Linux libfuse3 can do).
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define VFS_CTL_PATH "/.vfsctl"

/* Issue one command and return the response text in `out` (NUL-terminated
   on success).  Returns 0 on a normal daemon reply, 1 on usage/I-O error.
   exit_status, when not NULL, is set to:
     0 = daemon accepted the command (ok/conflict/etc.)
     2 = a special "conflict" result for commit
     nonzero = a hard error. */
static int ctl_run(const char* mountpoint, const char* cmd, char* out,
                   size_t out_cap, int* exit_status) {
    /* Build the absolute path "<mountpoint>/.vfsctl".  The daemon
       intercepts getattr/read/write on this special path. */
    char path[1024];
    if (snprintf(path, sizeof(path), "%s%s.vfsctl",
                 mountpoint,
                 mountpoint[strlen(mountpoint) - 1] == '/' ? "" : "/") >= (int)sizeof(path)) {
        fprintf(stderr, "mountpoint path too long\n");
        return 1;
    }

    /* Open RDWR (some commands — commit / delete-snapshot / gc — need
       to mutate VFS state).  Daemon rejects writes on read-only mounts. */
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        perror("open .vfsctl");
        return 1;
    }

    if (write(fd, cmd, strlen(cmd)) != (ssize_t)strlen(cmd)) {
        perror("write control command");
        close(fd);
        return 1;
    }

    /* Close the write fd and reopen for read.  macFUSE's highlevel layer
       caches file content per-fd; if we read on the same fd we just
       wrote to, the kernel may return stale cached data (e.g. an empty
       buffer from before our write).  Opening a fresh fd forces a fresh
       read request to the daemon, which returns the current response. */
    close(fd);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("reopen .vfsctl for read");
        return 1;
    }

    ssize_t n = read(fd, out, out_cap - 1);
    if (n < 0) {
        perror("read control response");
        close(fd);
        return 1;
    }
    out[n] = '\0';
    close(fd);

    /* Map recognized daemon responses to exit statuses. */
    int rc = 0;
    if (exit_status) {
        if (strcmp(out, "conflict\n") == 0) {
            *exit_status = 2;
        } else if (strncmp(out, "ok\n", 3) == 0) {
            *exit_status = 0;
        } else if (strncmp(out, "ERR ", 4) == 0) {
            fputs(out, stderr);
            *exit_status = 1;
            rc = 1;
        } else if (out[0] != '\0') {
            /* Snapshot returns just the epoch; treat any non-empty
               non-prefixed-with-ERR/conflict/ok line as success. */
            *exit_status = 0;
        }
    }
    return rc;
}

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
    const char* subcmd = argv[1];
    const char* mountpoint = argv[2];

    char cmd[64];
    char resp[128];

    if (strcmp(subcmd, "snapshot") == 0) {
        snprintf(cmd, sizeof(cmd), "snapshot\n");
    } else if (strcmp(subcmd, "commit") == 0) {
        if (argc < 4) { usage(argv[0]); return 1; }
        long long e = atoll(argv[3]);
        if (e <= 0) {
            fprintf(stderr, "commit: epoch must be positive\n");
            usage(argv[0]);
            return 1;
        }
        snprintf(cmd, sizeof(cmd), "commit %lld\n", e);
    } else if (strcmp(subcmd, "delete-snapshot") == 0) {
        if (argc < 4) { usage(argv[0]); return 1; }
        long long e = atoll(argv[3]);
        if (e <= 0) {
            fprintf(stderr, "delete-snapshot: epoch must be positive\n");
            usage(argv[0]);
            return 1;
        }
        snprintf(cmd, sizeof(cmd), "delete-snapshot %lld\n", e);
    } else if (strcmp(subcmd, "gc") == 0) {
        snprintf(cmd, sizeof(cmd), "gc\n");
    } else {
        usage(argv[0]);
        return 1;
    }

    int exit_status = 0;
    int rc = ctl_run(mountpoint, cmd, resp, sizeof(resp), &exit_status);
    if (rc != 0) return rc;

    /* Trim trailing newline for snapshot output (epoch is one line). */
    size_t len = strlen(resp);
    while (len > 0 && (resp[len-1] == '\n' || resp[len-1] == '\r'))
        resp[--len] = '\0';

    if (strcmp(subcmd, "snapshot") == 0) {
        /* Print just the epoch on stdout. */
        printf("%s\n", resp);
        return 0;
    } else {
        printf("%s\n", resp);
        return exit_status;
    }
}
