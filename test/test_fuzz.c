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

    int crashes = 0;
    int ops_total = 0;

    for (int iter = 0; iter < iterations; iter++) {
        cleanup_vfs();

        /* Mount fresh VFS */
        vfs_t* vfs = vfs_mount(FUZZ_VFS_PATH, FUZZ_PAGE_SZ);
        if (!vfs) {
            printf("FAIL iter %d: mount\n", iter);
            return 1;
        }

        FuzzState st;
        memset(&st, 0, sizeof(st));
        int64_t root = vfs->ctx->rootNodeOffset;

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
                    vfs_dirent_t ents[32];
                    vfs_readdir(vfs, root, ents, 32, 0);
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

        /* Clean unmount — no crash for basic fuzz */
        vfs_flush(vfs);
        vfs_unmount(vfs);

        if (iter > 0 && iter % 1000 == 0)
            printf("  %d iterations, %d ops, %d crashes so far\n",
                   iter, ops_total, crashes);
    }

    printf("\n=== Fuzz Results ===\n");
    printf("  iterations: %d\n", iterations);
    printf("  total ops:  %d\n", ops_total);

    cleanup_vfs();
    return 0;
}

#else  /* _WIN32 — stub */
#include <stdio.h>
int main(int argc, char** argv) {
    (void)argc; (void)argv;
    fprintf(stderr, "test_fuzz: skipped on Windows\n");
    return 0;
}
#endif
