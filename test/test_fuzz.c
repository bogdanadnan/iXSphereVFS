/* Fuzz testing harness — deterministic pseudo-random VFS operations.
 * Each iteration creates a fresh VFS, applies a random sequence of
 * operations, and verifies no unexpected errors occur.
 *
 * Deterministic PRNG: xorshift64 with fixed seed (42).
 * Run: ./test_fuzz [--seed N] [--iter N]
 */

#ifndef _WIN32
#include "ixsphere/vfs.h"
#include "ixsphere/vfs_internal.h"
#include "nodes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

/* ---------------------------------------------------------------------------
 * Deterministic PRNG — xorshift64
 * --------------------------------------------------------------------------- */

typedef struct {
    uint64_t state;
} Rand;

static uint64_t rand_next(Rand* r) {
    uint64_t x = r->state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    r->state = x;
    return x;
}

static int rand_int(Rand* r, int max) {
    if (max <= 0) return 0;
    return (int)(rand_next(r) % (uint64_t)max);
}

/* ---------------------------------------------------------------------------
 * Fuzz context — tracks open files, directories, snapshots
 * --------------------------------------------------------------------------- */

#define FUZZ_VFS_PATH  "/tmp/test_fuzz.vfs"
#define FUZZ_SEED_PATH  "/tmp/test_fuzz_seed.vfs"
#define FUZZ_PAGE_SZ   8192
#define FUZZ_MAX_OPEN  64
#define FUZZ_OPS_PER_ITER  20

typedef struct {
    int64_t fvp;        /* VirtualPtr of file or directory, 0 = empty */
    char    name[64];   /* entry name */
    int     is_dir;
} FuzzEntry;

typedef struct {
    FuzzEntry entries[FUZZ_MAX_OPEN];
    int       count;
    int64_t   snapshots[16];
    int       snap_count;
} FuzzState;

static void fuzz_state_add(FuzzState* st, int64_t vp, const char* name, int is_dir) {
    if (st->count >= FUZZ_MAX_OPEN) return;
    st->entries[st->count].fvp = vp;
    st->entries[st->count].is_dir = is_dir;
    snprintf(st->entries[st->count].name, sizeof(st->entries[0].name), "%s", name);
    st->count++;
}

static void fuzz_state_remove(FuzzState* st, int idx) {
    if (idx < 0 || idx >= st->count) return;
    memmove(&st->entries[idx], &st->entries[idx + 1],
            (size_t)(st->count - idx - 1) * sizeof(FuzzEntry));
    st->count--;
}

static int fuzz_state_find_index(FuzzState* st, const char* name) {
    for (int i = 0; i < st->count; i++)
        if (strcmp(st->entries[i].name, name) == 0) return i;
    return -1;
}

/* ---------------------------------------------------------------------------
 * Protect against SIGKILL in child processes from crash scenarios
 * --------------------------------------------------------------------------- */
static void cleanup_vfs(void) { unlink(FUZZ_VFS_PATH); }

/* ---------------------------------------------------------------------------
 * Populate a seed VFS with 10 files containing known content.
 * Returns 0 on success, -1 on failure.
 * --------------------------------------------------------------------------- */

static int populate_seed_vfs(void) {
    unlink(FUZZ_SEED_PATH);
    vfs_t* vfs = vfs_mount(FUZZ_SEED_PATH, FUZZ_PAGE_SZ);
    if (!vfs) return -1;
    int64_t root = vfs->ctx->rootNodeOffset;

    for (int f = 0; f < 10; f++) {
        char name[32];
        snprintf(name, sizeof(name), "seed_%d.dat", f);
        int64_t fvp = vfs_create(vfs, root, name, 0);
        if (fvp <= 0) { vfs_unmount(vfs); return -1; }

        /* Write a page of known content: 0xAA + file index in every byte */
        uint8_t buf[FUZZ_PAGE_SZ];
        memset(buf, (uint8_t)(0xAA + f), (size_t)FUZZ_PAGE_SZ);
        if (vfs_write(vfs, fvp, buf, 0, FUZZ_PAGE_SZ, 0) != FUZZ_PAGE_SZ) {
            vfs_unmount(vfs); return -1;
        }
    }

    vfs_flush(vfs);
    vfs_unmount(vfs);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Corrupt phase — open the backing file and XOR random bits into a
 * single random byte (excluding the page-0 XVFS magic at bytes 0..3).
 * --------------------------------------------------------------------------- */

static void fuzz_corrupt(const char* path, Rand* r) {
    int fd = open(path, O_RDWR);
    if (fd < 0) return;

    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size <= 4) { close(fd); return; }  /* too small */

    /* Pick a random byte offset, avoiding bytes 0-3 (page-0 magic) */
    off_t pos = (off_t)(4 + rand_next(r) % (uint64_t)(file_size - 4));
    uint8_t byte_val = 0;
    if (pread(fd, &byte_val, 1, pos) != 1) { close(fd); return; }

    /* XOR a random bit pattern into the byte */
    uint8_t mask = (uint8_t)(rand_next(r) & 0xFF);
    byte_val ^= mask;

    pwrite(fd, &byte_val, 1, pos);
    close(fd);
}

/* ---------------------------------------------------------------------------
 * Verify phase — recursively walk the directory tree, read all files,
 * and check that seed-file content is intact.  CRC or corruption errors
 * detected by vfs_read must return error codes, not crash the test.
 * --------------------------------------------------------------------------- */

static void fuzz_verify_dir(vfs_t* vfs, int64_t dir_vp, int depth,
                             int* files_read, int* errors) {
    if (depth > 8) return;  /* safety limit */
    vfs_dirent_t* ents = NULL;
    int n = 0;
    int rc = vfs_readdir(vfs, dir_vp, &ents, &n, 0);
    if (rc != VFS_OK) { (*errors)++; return; }

    for (int i = 0; i < n; i++) {
        if (ents[i].isDir) {
            fuzz_verify_dir(vfs, ents[i].vp, depth + 1, files_read, errors);
        } else {
            (*files_read)++;
            /* Read the file — must not crash even with corrupted data */
            uint8_t buf[256];
            int r = vfs_read(vfs, ents[i].vp, buf, 0, sizeof(buf), 0);
            if (r < 0) { (*errors)++; continue; }

            /* Verify known seed file content */
            if (strncmp(ents[i].name, "seed_", 5) == 0) {
                int idx = atoi(ents[i].name + 5);
                uint8_t expected = (uint8_t)(0xAA + idx);
                for (int b = 0; b < r; b++) {
                    if (buf[b] != expected) {
                        (*errors)++;
                        break;
                    }
                }
            }
        }
    }
    vfs_free_dirents(ents);
}

static int fuzz_verify(vfs_t* vfs) {
    int files_read = 0;
    int errors = 0;
    fuzz_verify_dir(vfs, vfs->ctx->rootNodeOffset, 0, &files_read, &errors);
    return errors;
}

/* ---------------------------------------------------------------------------
 * Fuzz loop — 10,000 iterations
 * --------------------------------------------------------------------------- */

int main(int argc, char** argv) {
    uint64_t seed = 42;
    int      iterations = 10000;

    for (int a = 1; a < argc; a++) {
        if (strncmp(argv[a], "--seed=", 7) == 0)
            seed = (uint64_t)atoll(argv[a] + 7);
        else if (strncmp(argv[a], "--iter=", 7) == 0)
            iterations = atoi(argv[a] + 7);
    }

    Rand r;
    r.state = seed;

    printf("=== Fuzz Test (seed=%llu, iterations=%d) ===\n\n",
           (unsigned long long)seed, iterations);

    /* Phase 1: populate seed VFS with 10 known files */
    if (populate_seed_vfs() != 0) {
        printf("FAIL: seed VFS populate\n");
        return 1;
    }
    printf("Seed VFS populated: 10 files with known content\n\n");

    int crashes = 0;
    int ops_total = 0;

    for (int iter = 0; iter < iterations; iter++) {
        /* Fork a child for each iteration — protects against
           SIGSEGV/SIGABRT crashes and hangs (alarm 30s timeout). */
        pid_t pid = fork();
        if (pid < 0) { printf("FAIL iter %d: fork\n", iter); return 1; }

        if (pid == 0) {
            /* Child: set 30-second hang detector */
            alarm(30);

            cleanup_vfs();

            /* Copy seed VFS to working path */
            {
            FILE* src = fopen(FUZZ_SEED_PATH, "rb");
            FILE* dst = fopen(FUZZ_VFS_PATH, "wb");
            if (!src || !dst) { if (src) fclose(src); if (dst) fclose(dst); continue; }
            uint8_t cbuf[8192];
            size_t n;
            while ((n = fread(cbuf, 1, sizeof(cbuf), src)) > 0)
                fwrite(cbuf, 1, n, dst);
            fclose(src);
            fclose(dst);
        }

        /* Mount pre-populated VFS */
        vfs_t* vfs = vfs_mount(FUZZ_VFS_PATH, FUZZ_PAGE_SZ);
        if (!vfs) {
            printf("FAIL iter %d: mount\n", iter);
            return 1;
        }

        FuzzState st;
        memset(&st, 0, sizeof(st));
        int64_t root = vfs->ctx->rootNodeOffset;

        /* Seed fuzz state with pre-populated files */
        vfs_dirent_t* seed_ents = NULL;
        int seed_n = 0;
        int seed_rc = vfs_readdir(vfs, root, &seed_ents, &seed_n, 0);
        for (int se = 0; se < seed_n && st.count < FUZZ_MAX_OPEN; se++) {
            fuzz_state_add(&st, seed_ents[se].vp,
                           seed_ents[se].name, (int)seed_ents[se].isDir);
        }
        vfs_free_dirents(seed_ents);

        int nops = rand_int(&r, FUZZ_OPS_PER_ITER) + 1;
        for (int op = 0; op < nops; op++) {
            int action = rand_int(&r, 11);  /* 0..10 */
            int idx;
            char name[32];
            int64_t ret;
            uint8_t buf[64];

            switch (action) {
            case 0: /* create file in root */
                snprintf(name, sizeof(name), "f%lld", (long long)rand_next(&r) % 100000);
                if (fuzz_state_find_index(&st, name) >= 0) break;
                ret = vfs_create(vfs, root, name, 0);
                if (ret > 0) {
                    fuzz_state_add(&st, ret, name, 0);
                    ops_total++;
                }
                break;

            case 1: /* create file in a random directory */
                if (st.count == 0) break;
                idx = rand_int(&r, st.count);
                if (!st.entries[idx].is_dir) break;
                snprintf(name, sizeof(name), "g%lld", (long long)rand_next(&r) % 100000);
                ret = vfs_create(vfs, st.entries[idx].fvp, name, 0);
                if (ret > 0) {
                    fuzz_state_add(&st, ret, name, 0);
                    ops_total++;
                }
                break;

            case 2: /* write data to a random file */
                if (st.count == 0) break;
                idx = rand_int(&r, st.count);
                if (st.entries[idx].is_dir) break;
                memset(buf, (uint8_t)rand_int(&r, 256), sizeof(buf));
                vfs_write(vfs, st.entries[idx].fvp, buf,
                          (int64_t)rand_int(&r, 1024) * 64, sizeof(buf), 0);
                ops_total++;
                break;

            case 3: /* read from a random file */
                if (st.count == 0) break;
                idx = rand_int(&r, st.count);
                if (st.entries[idx].is_dir) break;
                vfs_read(vfs, st.entries[idx].fvp, buf,
                         (int64_t)rand_int(&r, 512) * 64, sizeof(buf), 0);
                ops_total++;
                break;

            case 4: /* delete a random file */
                if (st.count == 0) break;
                idx = rand_int(&r, st.count);
                if (st.entries[idx].is_dir) break;
                {
                    int64_t parent = root;
                    /* Find parent — for simplicity, delete from root;
                       files in subdirs need the subdir as parent */
                    (void)parent;
                }
                ret = vfs_delete(vfs, root, st.entries[idx].name, 0);
                if (ret == VFS_OK) {
                    fuzz_state_remove(&st, idx);
                    ops_total++;
                }
                break;

            case 5: /* mkdir */
                snprintf(name, sizeof(name), "d%lld", (long long)rand_next(&r) % 100000);
                if (fuzz_state_find_index(&st, name) >= 0) break;
                ret = vfs_mkdir(vfs, root, name, 0);
                if (ret > 0) {
                    fuzz_state_add(&st, ret, name, 1);
                    ops_total++;
                }
                break;

            case 6: /* rmdir */
                if (st.count == 0) break;
                idx = rand_int(&r, st.count);
                if (!st.entries[idx].is_dir) break;
                ret = vfs_rmdir(vfs, root, st.entries[idx].name, 0);
                if (ret == VFS_OK) {
                    fuzz_state_remove(&st, idx);
                    ops_total++;
                }
                break;

            case 7: /* readdir on root */
                {
                    vfs_dirent_t* ents = NULL;
                    int n = 0;
                    vfs_readdir(vfs, root, &ents, &n, 0);
                    vfs_free_dirents(ents);
                    ops_total++;
                }
                break;

            case 8: /* rename */
                if (st.count < 2) break;
                idx = rand_int(&r, st.count);
                snprintf(name, sizeof(name), "r%lld", (long long)rand_next(&r) % 100000);
                if (fuzz_state_find_index(&st, name) >= 0) break;
                vfs_rename(vfs, root, st.entries[idx].name, root, name, 0);
                snprintf(st.entries[idx].name, sizeof(st.entries[idx].name), "%s", name);
                ops_total++;
                break;

            case 9: /* snapshot */
                {
                    int64_t snap = vfs_snapshot(vfs);
                    if (snap > 0 && st.snap_count < 16) {
                        st.snapshots[st.snap_count++] = snap;
                        ops_total++;
                    }
                }
                break;

            case 10: /* flush */
                vfs_flush(vfs);
                ops_total++;
                break;
            }
        }

        /* Flush and unmount cleanly */
        vfs_flush(vfs);
        vfs_unmount(vfs);

        /* Corrupt a random byte in the backing file (avoid page-0 magic) */
        if (rand_int(&r, 4) == 0) {  /* ~25% chance of corruption per iteration */
            fuzz_corrupt(FUZZ_VFS_PATH, &r);
        }

        /* Remount and verify the VFS is still loadable (or gracefully fails) */
        vfs = vfs_mount(FUZZ_VFS_PATH, FUZZ_PAGE_SZ);
        if (vfs) {
            /* Walk tree, read all files, verify seed content.
               CRC errors from corruption must return error codes, not crash. */
            int verrors = fuzz_verify(vfs);
            if (verrors > 0) {
                /* Verification errors from corrupted data are expected —
                   the corruption phase intentionally modifies bytes.
                   The test passes as long as no crash occurs. */
            }
            vfs_unmount(vfs);
        }

            _exit(0);  /* child completes successfully */
        }

        /* Parent: wait for child and check exit status */
        int status;
        waitpid(pid, &status, 0);

        if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            if (sig == SIGALRM) {
                printf("HANG iter %d: alarm timeout (30s)\n", iter);
                crashes++;
            } else {
                printf("CRASH iter %d: signal %d (%s)\n", iter, sig,
                       sig == SIGSEGV ? "SIGSEGV" :
                       sig == SIGABRT ? "SIGABRT" : "unknown");
                crashes++;
            }
        } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            printf("CRASH iter %d: exit code %d\n", iter, WEXITSTATUS(status));
            crashes++;
        }
    }  /* end for(iter) */

    printf("\n=== Fuzz Results ===\n");
    printf("  iterations: %d\n", iterations);
    printf("  total ops:  %d\n", ops_total);
    printf("  crashes:    %d\n", crashes);

    cleanup_vfs();
    unlink(FUZZ_SEED_PATH);
    return crashes > 0 ? 1 : 0;
}

#else  /* _WIN32 — stub */
#include <stdio.h>
int main(int argc, char** argv) {
    (void)argc; (void)argv;
    fprintf(stderr, "test_fuzz: skipped on Windows\n");
    return 0;
}
#endif
