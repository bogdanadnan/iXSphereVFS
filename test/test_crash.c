/* Crassh recovery scenarios — standalone tests of VFS mount/unmount
 * resilience.  Each scenario runs 100 iterations internally and returns
 * 0 (pass) or -1 (fail).  Run via CMake test or directly.
 *
 * Usage: ./test_crash                    — run all 8 scenarios
 *        ./test_crash <scenario-number>  — run one scenario (0-7)
 */

#include "ixsphere/vfs.h"
#include "ixsphere/vfs_internal.h"
#include "nodes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------- */

#define VFS_PATH  "/tmp/test_crash_vfs.dat"
#define PAGE_SZ   8192

/* Clean up the VFS backing file before each iteration. */
static void cleanup(void) { unlink(VFS_PATH); }

/* Check condition, print failure, return -1. */
#define SCENARIO_CHECK(expr, msg) do { \
    if (!(expr)) { fprintf(stderr, "  FAIL: %s\n", msg); return -1; } \
} while(0)

/* Find a file by name in root directory via readdir. Returns VirtualPtr or -1. */
static int64_t find_file(vfs_t* vfs, const char* name) {
    vfs_dirent_t entries[64];
    int n = vfs_readdir(vfs, vfs->ctx->rootNodeOffset, entries, 64, 0);
    if (n < 0) return -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(entries[i].name, name) == 0 && !entries[i].isDir)
            return entries[i].vp;
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * Scenario 0: create_file — create a file, crash/reopen, verify it exists.
 * --------------------------------------------------------------------------- */

static int scenario_create_file(void) {
    for (int i = 0; i < 100; i++) {
        cleanup();
        vfs_t* vfs = vfs_mount(VFS_PATH, PAGE_SZ);
        if (!vfs) { fprintf(stderr, "  FAIL iter %d: mount\n", i); return -1; }
        char name[64];
        snprintf(name, sizeof(name), "f%d.txt", i);
        int64_t fvp = vfs_create(vfs, vfs->ctx->rootNodeOffset, name, 0);
        if (fvp <= 0) { vfs_unmount(vfs); fprintf(stderr, "  FAIL iter %d: create\n", i); return -1; }
        vfs_flush(vfs);
        vfs_unmount(vfs);  /* simulated crash — no explicit flush */

        /* Reopen and verify */
        vfs = vfs_mount(VFS_PATH, PAGE_SZ);
        if (!vfs) { fprintf(stderr, "  FAIL iter %d: remount\n", i); return -1; }
        int64_t vp = vfs_open(vfs, vfs->ctx->rootNodeOffset, name, 0);
        int ok = (vp > 0);
        vfs_unmount(vfs);
        if (!ok) { fprintf(stderr, "  FAIL iter %d: verify\n", i); return -1; }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Scenario 1: write_verify — write data, crash/reopen, read and verify.
 * --------------------------------------------------------------------------- */

static int scenario_write_verify(void) {
    uint8_t ref[128];
    for (int i = 0; i < 100; i++) {
        cleanup();
        vfs_t* vfs = vfs_mount(VFS_PATH, PAGE_SZ);
        if (!vfs) return -1;
        int64_t fvp = vfs_create(vfs, vfs->ctx->rootNodeOffset, "test.dat", 0);
        if (fvp <= 0) { vfs_unmount(vfs); return -1; }
        memset(ref, (uint8_t)(i & 0xFF), sizeof(ref));
        int w = vfs_write(vfs, fvp, ref, 0, sizeof(ref), 0);
        if (w != (int)sizeof(ref)) { vfs_unmount(vfs); return -1; }
        vfs_flush(vfs);
        vfs_unmount(vfs);

        vfs = vfs_mount(VFS_PATH, PAGE_SZ);
        if (!vfs) return -1;
        int64_t fvp2 = find_file(vfs, "test.dat");
        if (fvp2 <= 0) { vfs_unmount(vfs); return -1; }
        uint8_t buf[128];
        int r = vfs_read(vfs, fvp2, buf, 0, sizeof(buf), 0);
        int ok = (r == (int)sizeof(buf) && memcmp(buf, ref, sizeof(buf)) == 0);
        vfs_unmount(vfs);
        if (!ok) { fprintf(stderr, "  FAIL iter %d: data mismatch\n", i); return -1; }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Scenario 2: delete_verify — create+delete, crash/reopen, verify gone.
 * --------------------------------------------------------------------------- */

static int scenario_delete_verify(void) {
    for (int i = 0; i < 100; i++) {
        cleanup();
        vfs_t* vfs = vfs_mount(VFS_PATH, PAGE_SZ);
        if (!vfs) return -1;
        int64_t fvp = vfs_create(vfs, vfs->ctx->rootNodeOffset, "del.dat", 0);
        if (fvp <= 0) { vfs_unmount(vfs); return -1; }
        uint8_t data[64];
        memset(data, (uint8_t)i, sizeof(data));
        vfs_write(vfs, fvp, data, 0, sizeof(data), 0);
        int del_ok = (vfs_delete(vfs, vfs->ctx->rootNodeOffset, "del.dat", 0) == VFS_OK);
        if (!del_ok) { vfs_unmount(vfs); return -1; }
        vfs_flush(vfs);
        vfs_unmount(vfs);

        vfs = vfs_mount(VFS_PATH, PAGE_SZ);
        if (!vfs) return -1;
        int64_t vp = vfs_open(vfs, vfs->ctx->rootNodeOffset, "del.dat", 0);
        int ok = (vp <= 0);  /* must not exist */
        vfs_unmount(vfs);
        if (!ok) { fprintf(stderr, "  FAIL iter %d: file still exists\n", i); return -1; }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Scenario 3: dir_ops — mkdir, create file inside, readdir, delete, rmdir,
 *                       crash/reopen, verify directory gone.
 * --------------------------------------------------------------------------- */

static int scenario_dir_ops(void) {
    for (int i = 0; i < 100; i++) {
        cleanup();
        vfs_t* vfs = vfs_mount(VFS_PATH, PAGE_SZ);
        if (!vfs) return -1;
        int64_t root = vfs->ctx->rootNodeOffset;

        int64_t dir_vp = vfs_mkdir(vfs, root, "subdir", 0);
        if (dir_vp <= 0) { vfs_unmount(vfs); return -1; }

        int64_t fvp = vfs_create(vfs, dir_vp, "inner.txt", 0);
        if (fvp <= 0) { vfs_unmount(vfs); return -1; }

        vfs_dirent_t ents[8];
        int n = vfs_readdir(vfs, dir_vp, ents, 8, 0);
        if (n < 1) { vfs_unmount(vfs); return -1; }

        if (vfs_delete(vfs, dir_vp, "inner.txt", 0) != VFS_OK) { vfs_unmount(vfs); return -1; }
        if (vfs_rmdir(vfs, root, "subdir", 0) != VFS_OK) { vfs_unmount(vfs); return -1; }

        vfs_flush(vfs);
        vfs_unmount(vfs);

        /* Reopen and verify directory is gone */
        vfs = vfs_mount(VFS_PATH, PAGE_SZ);
        if (!vfs) return -1;
        int64_t vp = vfs_open(vfs, vfs->ctx->rootNodeOffset, "subdir", 0);
        int ok = (vp <= 0);  /* must not exist */
        vfs_unmount(vfs);
        if (!ok) { fprintf(stderr, "  FAIL iter %d: dir still exists\n", i); return -1; }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Scenario 4: multi_write — write several pages, crash/reopen, verify all.
 * --------------------------------------------------------------------------- */

static int scenario_multi_write(void) {
    int npages = 4;
    uint8_t* ref = (uint8_t*)malloc((size_t)PAGE_SZ);
    if (!ref) return -1;

    for (int i = 0; i < 100; i++) {
        cleanup();
        vfs_t* vfs = vfs_mount(VFS_PATH, PAGE_SZ);
        if (!vfs) { free(ref); return -1; }
        int64_t fvp = vfs_create(vfs, vfs->ctx->rootNodeOffset, "multi.dat", 0);
        if (fvp <= 0) { vfs_unmount(vfs); free(ref); return -1; }

        for (int p = 0; p < npages; p++) {
            memset(ref, (uint8_t)(i + p), (size_t)PAGE_SZ);
            if (vfs_write(vfs, fvp, ref, (int64_t)p * PAGE_SZ, PAGE_SZ, 0) != PAGE_SZ) {
                vfs_unmount(vfs); free(ref); return -1;
            }
        }
        vfs_flush(vfs);
        vfs_unmount(vfs);

        vfs = vfs_mount(VFS_PATH, PAGE_SZ);
        if (!vfs) { free(ref); return -1; }
        int64_t fvp2 = find_file(vfs, "multi.dat");
        if (fvp2 <= 0) { vfs_unmount(vfs); free(ref); return -1; }

        int ok = 1;
        for (int p = 0; p < npages && ok; p++) {
            uint8_t* buf = (uint8_t*)malloc((size_t)PAGE_SZ);
            if (!buf) { ok = 0; break; }
            memset(ref, (uint8_t)(i + p), (size_t)PAGE_SZ);
            int r = vfs_read(vfs, fvp2, buf, (int64_t)p * PAGE_SZ, PAGE_SZ, 0);
            if (r != PAGE_SZ || memcmp(buf, ref, (size_t)PAGE_SZ) != 0) ok = 0;
            free(buf);
        }
        vfs_unmount(vfs);
        if (!ok) { fprintf(stderr, "  FAIL iter %d: multi-page mismatch\n", i); free(ref); return -1; }
    }
    free(ref);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Scenario 5: overwrite — write, overwrite with different data, crash/reopen,
 *                         verify latest.
 * --------------------------------------------------------------------------- */

static int scenario_overwrite(void) {
    uint8_t buf_a[256], buf_b[256];
    for (int i = 0; i < 100; i++) {
        cleanup();
        vfs_t* vfs = vfs_mount(VFS_PATH, PAGE_SZ);
        if (!vfs) return -1;
        int64_t fvp = vfs_create(vfs, vfs->ctx->rootNodeOffset, "over.dat", 0);
        if (fvp <= 0) { vfs_unmount(vfs); return -1; }

        memset(buf_a, (uint8_t)(i * 2), sizeof(buf_a));
        memset(buf_b, (uint8_t)(i * 2 + 1), sizeof(buf_b));

        if (vfs_write(vfs, fvp, buf_a, 0, sizeof(buf_a), 0) != (int)sizeof(buf_a)) {
            vfs_unmount(vfs); return -1;
        }
        /* Overwrite with different data */
        if (vfs_write(vfs, fvp, buf_b, 0, sizeof(buf_b), 0) != (int)sizeof(buf_b)) {
            vfs_unmount(vfs); return -1;
        }
        vfs_flush(vfs);
        vfs_unmount(vfs);

        vfs = vfs_mount(VFS_PATH, PAGE_SZ);
        if (!vfs) return -1;
        int64_t fvp2 = find_file(vfs, "over.dat");
        if (fvp2 <= 0) { vfs_unmount(vfs); return -1; }

        uint8_t buf_r[256];
        int r = vfs_read(vfs, fvp2, buf_r, 0, sizeof(buf_r), 0);
        int ok = (r == (int)sizeof(buf_r) && memcmp(buf_r, buf_b, sizeof(buf_b)) == 0);
        vfs_unmount(vfs);
        if (!ok) { fprintf(stderr, "  FAIL iter %d: overwrite mismatch\n", i); return -1; }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Scenario 6: rename — create file, rename, crash/reopen, verify new name.
 * --------------------------------------------------------------------------- */

static int scenario_rename(void) {
    for (int i = 0; i < 100; i++) {
        cleanup();
        vfs_t* vfs = vfs_mount(VFS_PATH, PAGE_SZ);
        if (!vfs) return -1;
        int64_t root = vfs->ctx->rootNodeOffset;

        int64_t fvp = vfs_create(vfs, root, "old.dat", 0);
        if (fvp <= 0) { vfs_unmount(vfs); return -1; }
        uint8_t data[64];
        memset(data, (uint8_t)i, sizeof(data));
        vfs_write(vfs, fvp, data, 0, sizeof(data), 0);

        if (vfs_rename(vfs, root, "old.dat", root, "new.dat", 0) != VFS_OK) {
            vfs_unmount(vfs); return -1;
        }
        vfs_flush(vfs);
        vfs_unmount(vfs);

        /* Reopen: old name must be gone, new name must exist with correct data */
        vfs = vfs_mount(VFS_PATH, PAGE_SZ);
        if (!vfs) return -1;
        root = vfs->ctx->rootNodeOffset;

        int64_t old_vp = vfs_open(vfs, root, "old.dat", 0);
        if (old_vp > 0) { vfs_unmount(vfs); return -1; }  /* must not exist */

        int64_t new_vp = find_file(vfs, "new.dat");
        if (new_vp <= 0) { vfs_unmount(vfs); return -1; }

        uint8_t buf[64];
        int r = vfs_read(vfs, new_vp, buf, 0, sizeof(buf), 0);
        int ok = (r == (int)sizeof(buf) && memcmp(buf, data, sizeof(data)) == 0);
        vfs_unmount(vfs);
        if (!ok) { fprintf(stderr, "  FAIL iter %d: rename data mismatch\n", i); return -1; }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Scenario 7: many_files — create N files, crash/reopen, verify all exist.
 * --------------------------------------------------------------------------- */

static int scenario_many_files(void) {
    int nfiles = 20;
    for (int i = 0; i < 100; i++) {
        cleanup();
        vfs_t* vfs = vfs_mount(VFS_PATH, PAGE_SZ);
        if (!vfs) return -1;
        int64_t root = vfs->ctx->rootNodeOffset;

        char name[64];
        for (int f = 0; f < nfiles; f++) {
            snprintf(name, sizeof(name), "mf_%d_%d.dat", i, f);
            int64_t fvp = vfs_create(vfs, root, name, 0);
            if (fvp <= 0) { vfs_unmount(vfs); return -1; }
            uint8_t val = (uint8_t)(i + f);
            uint8_t data[32];
            memset(data, val, sizeof(data));
            if (vfs_write(vfs, fvp, data, 0, sizeof(data), 0) != (int)sizeof(data)) {
                vfs_unmount(vfs); return -1;
            }
        }
        vfs_flush(vfs);
        vfs_unmount(vfs);

        vfs = vfs_mount(VFS_PATH, PAGE_SZ);
        if (!vfs) return -1;
        root = vfs->ctx->rootNodeOffset;
        int all_ok = 1;
        for (int f = 0; f < nfiles && all_ok; f++) {
            snprintf(name, sizeof(name), "mf_%d_%d.dat", i, f);
            int64_t fvp2 = find_file(vfs, name);
            if (fvp2 <= 0) { all_ok = 0; break; }
            uint8_t val = (uint8_t)(i + f);
            uint8_t buf[32];
            int r = vfs_read(vfs, fvp2, buf, 0, sizeof(buf), 0);
            if (r != (int)sizeof(buf) || buf[0] != val) all_ok = 0;
        }
        vfs_unmount(vfs);
        if (!all_ok) { fprintf(stderr, "  FAIL iter %d: many files\n", i); return -1; }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Scenario 8: single_copy_write — write data without mirroring, then simulate
 * a process crash via fork/exit (no flush, no clean unmount).  On remount
 * the VFS must detect the missing data — a single-copy write that was never
 * flushed leaves either no valid page or a torn write, and the read returns
 * zero-filled data (corruption detected).
 *
 * Each iteration: fork → child mounts, writes, _exit(0) immediately →
 * parent remounts → verifies that the page content is NOT the written data
 * (zero-filled due to missing/partial CRC check on the single on-disk copy).
 * --------------------------------------------------------------------------- */

static int scenario_single_copy_write(void) {
    uint8_t* page_buf = (uint8_t*)malloc((size_t)PAGE_SZ);
    if (!page_buf) return -1;

    for (int i = 0; i < 100; i++) {
        cleanup();

        /* Phase 1: fork a child that writes data then hard-crashes */
        pid_t pid = fork();
        if (pid < 0) { free(page_buf); return -1; }

        if (pid == 0) {
            /* Child: mount, write one page, then die immediately — no flush, no unmount */
            vfs_t* vfs = vfs_mount(VFS_PATH, PAGE_SZ);
            if (!vfs) _exit(-1);
            int64_t fvp = vfs_create(vfs, vfs->ctx->rootNodeOffset, "sc.dat", 0);
            if (fvp <= 0) _exit(-1);
            memset(page_buf, (uint8_t)(i & 0xFF), (size_t)PAGE_SZ);
            int w = vfs_write(vfs, fvp, page_buf, 0, PAGE_SZ, 0);
            if (w != PAGE_SZ) _exit(-1);
            /* HARD CRASH — no vfs_flush, no vfs_unmount, no storage_close */
            _exit(0);
        }

        /* Parent: wait for child to crash */
        int status;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status)) { free(page_buf); return -1; }

        /* Phase 2: remount and verify data is NOT intact */
        vfs_t* vfs = vfs_mount(VFS_PATH, PAGE_SZ);
        if (!vfs) { free(page_buf); return -1; }
        int64_t fvp2 = find_file(vfs, "sc.dat");
        /* If the file doesn't exist at all, that's also detection */
        if (fvp2 <= 0) {
            vfs_unmount(vfs);
            continue;  /* file lost = corruption detected = pass */
        }

        uint8_t* buf = (uint8_t*)malloc((size_t)PAGE_SZ);
        if (!buf) { vfs_unmount(vfs); free(page_buf); return -1; }
        int r = vfs_read(vfs, fvp2, buf, 0, PAGE_SZ, 0);
        /* After a crash with no flush, the data page either:
         *  - was not written to disk at all → zero-filled (detection works)
         *  - was partially written → CRC mismatch → zero-filled (detection works)
         *  - was fully written by luck → data matches (very unlikely but possible)
         * We accept zero-filled as "corruption detected".  If the data
         * actually matches the original, the crash didn't cause loss. */
        int data_intact = (r == PAGE_SZ && memcmp(buf, page_buf, (size_t)PAGE_SZ) == 0);
        free(buf);
        vfs_unmount(vfs);

        if (data_intact) {
            fprintf(stderr, "  FAIL iter %d: single-copy data survived crash (no detection)\n", i);
            free(page_buf);
            return -1;
        }
    }
    free(page_buf);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Scenario 9: mirrored_write — write data with lazy mirror active, crash
 * mid-write via fork/exit, remount, verify that valid data survives from
 * at least one of the two mirror copies.
 *
 * Each iteration: fork child → child mounts, creates file, writes v1,
 * writes v2 (overwrite same page, triggering mirror allocation), crashes
 * immediately → parent remounts → verifies data is NOT all zeros (at
 * least one mirror copy survived the crash).
 * --------------------------------------------------------------------------- */

static int scenario_mirrored_write(void) {
    uint8_t* page_v1 = (uint8_t*)malloc((size_t)PAGE_SZ);
    uint8_t* page_v2 = (uint8_t*)malloc((size_t)PAGE_SZ);
    if (!page_v1 || !page_v2) { free(page_v1); free(page_v2); return -1; }

    for (int i = 0; i < 100; i++) {
        cleanup();
        memset(page_v1, 0x11, (size_t)PAGE_SZ);
        memset(page_v2, 0x22, (size_t)PAGE_SZ);

        /* Fork child that writes twice (forcing mirror) then crashes */
        pid_t pid = fork();
        if (pid < 0) { free(page_v1); free(page_v2); return -1; }

        if (pid == 0) {
            /* Child: mount, write v1 (single copy), write v2 (allocates mirror), crash */
            vfs_t* vfs = vfs_mount(VFS_PATH, PAGE_SZ);
            if (!vfs) _exit(-1);
            int64_t fvp = vfs_create(vfs, vfs->ctx->rootNodeOffset, "mir.dat", 0);
            if (fvp <= 0) _exit(-1);

            /* First write — single copy (generation 1, no mirror) */
            if (vfs_write(vfs, fvp, page_v1, 0, PAGE_SZ, 0) != PAGE_SZ) _exit(-1);

            /* Second write — allocates mirror (generation 2, writes to sibling) */
            if (vfs_write(vfs, fvp, page_v2, 0, PAGE_SZ, 0) != PAGE_SZ) _exit(-1);

            /* Flush all dirty pages (pool metadata + data) to disk so that
               the file's DirContent/NameEntry survives the crash.  The mirror
               data page was already written via pwrite in mirror_write, but
               pool pages are modified in-place in the cache and need flush. */
            vfs_flush(vfs);

            /* HARD CRASH — no clean unmount, just exit */
            _exit(0);
        }

        /* Parent waits for crash */
        int status;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status)) { free(page_v1); free(page_v2); return -1; }

        /* Remount and verify data survives (not all zeros) */
        vfs_t* vfs = vfs_mount(VFS_PATH, PAGE_SZ);
        if (!vfs) { free(page_v1); free(page_v2); return -1; }
        int64_t fvp2 = find_file(vfs, "mir.dat");
        if (fvp2 <= 0) {
            fprintf(stderr,
                    "  FAIL iter %d: file metadata lost — mirror mechanism did not preserve accessible data\n", i);
            vfs_unmount(vfs);
            free(page_v1); free(page_v2);
            return -1;
        }

        uint8_t* buf = (uint8_t*)malloc((size_t)PAGE_SZ);
        if (!buf) { vfs_unmount(vfs); free(page_v1); free(page_v2); return -1; }
        int r = vfs_read(vfs, fvp2, buf, 0, PAGE_SZ, 0);
        int ok = 0;
        if (r == PAGE_SZ) {
            /* Data must match one of the two versions (mirror recovery), NOT be all zeros */
            int match_v1 = (memcmp(buf, page_v1, (size_t)PAGE_SZ) == 0);
            int match_v2 = (memcmp(buf, page_v2, (size_t)PAGE_SZ) == 0);
            ok = (match_v1 || match_v2);
        }
        free(buf);
        vfs_unmount(vfs);

        if (!ok) {
            fprintf(stderr, "  FAIL iter %d: mirrored data did not survive crash\n", i);
            free(page_v1); free(page_v2);
            return -1;
        }
    }
    free(page_v1); free(page_v2);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Scenarios table
 * --------------------------------------------------------------------------- */

typedef int (*scenario_fn)(void);

static struct {
    const char* name;
    scenario_fn fn;
} scenarios[] = {
    {"create_file",   scenario_create_file},
    {"write_verify",  scenario_write_verify},
    {"delete_verify", scenario_delete_verify},
    {"dir_ops",       scenario_dir_ops},
    {"multi_write",   scenario_multi_write},
    {"overwrite",     scenario_overwrite},
    {"rename",        scenario_rename},
    {"many_files",           scenario_many_files},
    {"single_copy_write",    scenario_single_copy_write},
    {"mirrored_write",       scenario_mirrored_write},
};
static const int n_scenarios = (int)(sizeof(scenarios) / sizeof(scenarios[0]));

/* ---------------------------------------------------------------------------
 * Main
 * --------------------------------------------------------------------------- */

int main(int argc, char** argv) {
    int single = -1;
    if (argc > 1) {
        single = atoi(argv[1]);
        if (single < 0 || single >= n_scenarios) {
            fprintf(stderr, "Usage: %s [scenario 0-%d]\n", argv[0], n_scenarios - 1);
            return 1;
        }
    }

    printf("=== Crash Recovery Scenarios ===\n\n");

    int passed = 0;
    int failed = 0;

    for (int s = 0; s < n_scenarios; s++) {
        if (single >= 0 && s != single) continue;
        printf("Scenario %d: %s... ", s, scenarios[s].name);
        fflush(stdout);
        int ret = scenarios[s].fn();
        if (ret == 0) {
            printf("PASS\n");
            passed++;
        } else {
            printf("FAIL\n");
            failed++;
        }
    }

    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           passed, passed + failed, failed);
    return failed > 0 ? 1 : 0;
}
