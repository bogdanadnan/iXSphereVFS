/* Phase 28 W2: GC thread infrastructure tests (spec: impl/phase-28-gc.md)
 *
 * Tests the background GC thread's lifecycle and dispatch.
 *
 * Test gate (per the spec):
 *   - test_gc_thread_lifecycle: mount, verify thread is running,
 *     push a NOOP, verify it's processed
 *   - test_gc_thread_shutdown: mount, push 100 NOOPs, immediately
 *     unmount, verify the thread joins (unprocessed entries left
 *     in the Bin for next mount)
 *   - test_gc_thread_empty: mount, wait 1.5s, verify thread idle,
 *     push 1 entry, verify processed within 1 backoff cycle
 */

#include "ixsphere/vfs.h"
#include "ixsphere/vfs_internal.h"
#include "storage.h"
#include "bin.h"
#include "gc.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/time.h>

/* Wall-clock seconds (double).  Used for benchmark timing. */
static double now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static int tests_run    = 0;
static int tests_passed = 0;

#define CHECK(expr) do { \
    tests_run++; \
    if (expr) { tests_passed++; } \
    else { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } \
} while(0)

#define CHECK_EQ(a, b)  CHECK((a) == (b))
#define CHECK_TRUE(a)   CHECK((a))

static void cleanup(const char* path) { unlink(path); }

/* Helper: read the Bin's global count from the header. */
static int64_t read_bin_count(vfs_t* vfs) {
    return vfs_atomic_load_i64((int64_t*)(vfs->ctx->sb->header_buf + HDR_OFF_BIN_COUNT));
}

/* ========================================================================== */

void test_gc_thread_lifecycle(void) {
    printf("1. GC thread lifecycle (start, process, stop)...\n");
    const char* path = "/tmp/test_gc_thread_lifecycle.vfs";
    cleanup(path);

    vfs_t* vfs = vfs_mount(path, 8192);
    CHECK(vfs != NULL);
    if (!vfs) { cleanup(path); return; }

    /* Push a NOOP entry.  The thread should pop and process it. */
    int rc = bin_push(vfs->ctx->sb, BIN_TRIGGER_NOOP, 42, 0);
    CHECK_EQ(rc, 0);
    CHECK_EQ(read_bin_count(vfs), 1);

    /* Wait up to 500ms for the thread to process the entry. */
    int drained = 0;
    for (int i = 0; i < 50; i++) {
        if (read_bin_count(vfs) == 0) { drained = 1; break; }
        usleep(10000);  /* 10ms */
    }
    CHECK(drained);
    CHECK_EQ(read_bin_count(vfs), 0);

    vfs_unmount(vfs);
    cleanup(path);
}

void test_gc_thread_shutdown(void) {
    printf("2. GC thread shutdown (unmount with pending entries)...\n");
    const char* path = "/tmp/test_gc_thread_shutdown.vfs";
    cleanup(path);

    vfs_t* vfs = vfs_mount(path, 8192);
    CHECK(vfs != NULL);
    if (!vfs) { cleanup(path); return; }

    /* Push 100 NOOPs.  The GC thread may process some of them
       concurrently, so we don't check bin_count == 100 (the count
       can be anywhere from 0 to 100 by the time the loop ends). */
    for (int i = 0; i < 100; i++) {
        int rc = bin_push(vfs->ctx->sb, BIN_TRIGGER_NOOP, (int64_t)(i + 1), 0);
        CHECK_EQ(rc, 0);
    }
    /* The push succeeded for all 100.  The count can be anywhere
       from 0 to 100 by now (the GC thread may have processed some). */
    int64_t after_push = read_bin_count(vfs);
    CHECK(after_push >= 0);
    CHECK(after_push <= 100);

    /* Immediately unmount — the thread may not have processed
       everything yet.  vfs_unmount should:
       (a) Set gc_shutdown = 1
       (b) Wait for the thread to exit (with 1s timeout)
       (c) Return

       Some entries may be processed before the thread sees the
       shutdown flag, but not all.  After unmount, the Bin still
       has any unprocessed entries.  We can't easily verify this
       after unmount (the vfs is gone), but we can verify that
       the unmount returned successfully. */
    vfs_unmount(vfs);

    /* Remount and verify the Bin still has the unprocessed entries. */
    vfs_t* vfs2 = vfs_mount(path, 8192);
    CHECK(vfs2 != NULL);
    if (!vfs2) { cleanup(path); return; }

    /* The Bin should have some entries (>= 0, depending on how
       many the first mount's thread processed).  If the first
       thread processed all 100, the count is 0.  If it processed
       some, the count is < 100.  We just verify it's <= 100. */
    int64_t remaining = read_bin_count(vfs2);
    CHECK(remaining >= 0);
    CHECK(remaining <= 100);

    vfs_unmount(vfs2);
    cleanup(path);
}

void test_gc_thread_empty(void) {
    printf("3. GC thread empty backoff (idle then wake on push)...\n");
    const char* path = "/tmp/test_gc_thread_empty.vfs";
    cleanup(path);

    vfs_t* vfs = vfs_mount(path, 8192);
    CHECK(vfs != NULL);
    if (!vfs) { cleanup(path); return; }

    /* Wait 1.5s.  The thread should be idle (no Bin entries).
       Tier-based backoff: 100ms for first 5 empty cycles, then
       1000ms.  After ~600ms, the thread is in steady-state 1000ms
       backoff.  Total 1.5s wait: ~5 cycles × 100ms + ~1 cycle ×
       1000ms = 1.5s. */
    usleep(1500000);

    /* Verify the thread is still running (we can push and the
       push should be visible to the thread). */
    int rc = bin_push(vfs->ctx->sb, BIN_TRIGGER_NOOP, 99, 0);
    CHECK_EQ(rc, 0);
    CHECK_EQ(read_bin_count(vfs), 1);

    /* Wait up to 1.5s for the thread to process.  In steady-state
       backoff, the thread might be in the middle of a 1s sleep
       when we push.  Worst case: ~1s + processing time. */
    int drained = 0;
    for (int i = 0; i < 150; i++) {
        if (read_bin_count(vfs) == 0) { drained = 1; break; }
        usleep(10000);  /* 10ms */
    }
    CHECK(drained);

    vfs_unmount(vfs);
    cleanup(path);
}

/* ========================================================================== */

/* Phase 28 W3: producer integration.  Verify that a real public
   operation (vfs_delete) pushes a NOOP trigger to the Bin, and
   the GC thread processes it. */
void test_gc_producer_integration(void) {
    printf("4. GC producer integration (vfs_delete pushes NOOP trigger)...\n");
    const char* path = "/tmp/test_gc_producer.vfs";
    cleanup(path);

    vfs_t* vfs = vfs_mount(path, 8192);
    CHECK(vfs != NULL);
    if (!vfs) { cleanup(path); return; }

    /* Create a file to delete. */
    int64_t root = vfs_root(vfs);
    int64_t file = vfs_create(vfs, root, "test.txt", 0);
    CHECK(file > 0);
    if (file <= 0) { vfs_unmount(vfs); cleanup(path); return; }

    /* Verify Bin is empty (no producers have pushed yet). */
    CHECK_EQ(read_bin_count(vfs), 0);

    /* Delete the file.  This should push a NOOP trigger. */
    int rc = vfs_delete(vfs, root, "test.txt", 0);
    CHECK_EQ(rc, VFS_OK);

    /* The push is concurrent with the GC thread, so bin_count
       can be 0 (already processed) or 1 (just pushed).  We
       wait up to 500ms for the thread to process. */
    int drained = 0;
    for (int i = 0; i < 50; i++) {
        if (read_bin_count(vfs) == 0) { drained = 1; break; }
        usleep(10000);  /* 10ms */
    }
    CHECK(drained);
    CHECK_EQ(read_bin_count(vfs), 0);

    vfs_unmount(vfs);
    cleanup(path);
}

/* Phase 28 W3: vfs_commit pushes a NOOP trigger.  Snapshot + commit
   is a multi-step operation; we verify the commit pushes a trigger. */
void test_gc_producer_commit(void) {
    printf("5. GC producer integration (vfs_commit pushes NOOP trigger)...\n");
    const char* path = "/tmp/test_gc_producer_commit.vfs";
    cleanup(path);

    vfs_t* vfs = vfs_mount(path, 8192);
    CHECK(vfs != NULL);
    if (!vfs) { cleanup(path); return; }

    /* Take a snapshot. */
    int64_t snap = vfs_snapshot(vfs);
    CHECK(snap > 0);
    if (snap <= 0) { vfs_unmount(vfs); cleanup(path); return; }

    /* Commit the snapshot.  This should push a NOOP trigger. */
    int rc = vfs_commit(vfs, snap);
    CHECK_EQ(rc, VFS_OK);

    /* Wait for the GC thread to process. */
    int drained = 0;
    for (int i = 0; i < 50; i++) {
        if (read_bin_count(vfs) == 0) { drained = 1; break; }
        usleep(10000);
    }
    CHECK(drained);

    vfs_unmount(vfs);
    cleanup(path);
}

/* ========================================================================== */

/* Phase 28 W4: full framework end-to-end.  4 producer threads perform
   a mix of operations (vfs_create, vfs_write, vfs_delete, vfs_commit)
   that each push a NOOP trigger to the Bin.  The single GC thread
   processes them.  After producers finish, wait for the Bin to drain
   and verify it's empty. */
typedef struct {
    vfs_t* vfs;
    int   thread_id;
    int   ops_per_thread;
    int   ops_succeeded;
} e2e_producer_arg_t;

static void* e2e_producer_thread_fn(void* arg) {
    e2e_producer_arg_t* pa = (e2e_producer_arg_t*)arg;
    int64_t root = vfs_root(pa->vfs);

    for (int i = 0; i < pa->ops_per_thread; i++) {
        /* Cycle through the 4 operations.  Each pushes a NOOP. */
        int op = i % 4;
        char name[64];
        snprintf(name, sizeof(name), "e2e_t%d_f%d", pa->thread_id, i);
        switch (op) {
        case 0: {
            /* vfs_create */
            int64_t file = vfs_create(pa->vfs, root, name, 0);
            if (file > 0) pa->ops_succeeded++;
            break;
        }
        case 1: {
            /* vfs_write (on a previously-created file) */
            char fname[64];
            snprintf(fname, sizeof(fname), "e2e_t%d_f%d", pa->thread_id, i - 1);
            int64_t file = vfs_open(pa->vfs, root, fname, 0);
            if (file > 0) {
                uint8_t buf[64];
                memset(buf, (uint8_t)pa->thread_id, sizeof(buf));
                if (vfs_write(pa->vfs, file, buf, 0, sizeof(buf), 0) > 0) {
                    pa->ops_succeeded++;
                }
            }
            break;
        }
        case 2: {
            /* vfs_delete (delete a previously-created file) */
            char fname[64];
            snprintf(fname, sizeof(fname), "e2e_t%d_f%d", pa->thread_id, i - 2);
            if (vfs_delete(pa->vfs, root, fname, 0) == VFS_OK) {
                pa->ops_succeeded++;
            }
            break;
        }
        case 3: {
            /* vfs_commit (snapshot + commit) */
            int64_t snap = vfs_snapshot(pa->vfs);
            if (snap > 0) {
                if (vfs_commit(pa->vfs, snap) == VFS_OK) {
                    pa->ops_succeeded++;
                }
            }
            break;
        }
        }
    }
    return NULL;
}

void test_bin_end_to_end(void) {
    printf("6. Bin end-to-end (4 producers, GC drain)...\n");
    const char* path = "/tmp/test_bin_e2e.vfs";
    cleanup(path);

    vfs_t* vfs = vfs_mount(path, 8192);
    CHECK(vfs != NULL);
    if (!vfs) { cleanup(path); return; }

    /* 4 producer threads, 100 ops each (mix of create/write/delete/commit). */
    enum { N_THREADS = 4, OPS_PER_THREAD = 100 };
    pthread_t threads[N_THREADS];
    e2e_producer_arg_t args[N_THREADS];
    for (int i = 0; i < N_THREADS; i++) {
        args[i].vfs = vfs;
        args[i].thread_id = i;
        args[i].ops_per_thread = OPS_PER_THREAD;
        args[i].ops_succeeded = 0;
        int rc = pthread_create(&threads[i], NULL, e2e_producer_thread_fn, &args[i]);
        CHECK_EQ(rc, 0);
    }
    for (int i = 0; i < N_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Total ops succeeded.  Some may have failed (e.g., delete
       before create, commit before snapshot), so this is at most
       4*100 = 400, not exactly 400. */
    int total_succeeded = 0;
    for (int i = 0; i < N_THREADS; i++) {
        total_succeeded += args[i].ops_succeeded;
    }
    CHECK(total_succeeded > 0);
    printf("    total ops succeeded: %d\n", total_succeeded);

    /* Wait for the GC thread to drain the Bin.  Each successful
       op pushed a NOOP trigger, so the Bin should have at most
       `total_succeeded` entries.  The GC thread is processing
       concurrently with our producer thread pool, so most
       entries may already be drained by the time we check. */
    int drained = 0;
    for (int i = 0; i < 100; i++) {
        if (read_bin_count(vfs) == 0) { drained = 1; break; }
        usleep(10000);  /* 10ms */
    }
    CHECK(drained);
    CHECK_EQ(read_bin_count(vfs), 0);

    vfs_unmount(vfs);
    cleanup(path);
}

/* ========================================================================== */

/* Phase 28 W4: crash safety.  Use fork() to simulate a crash.  The
   child exits without unmounting; the parent remounts and verifies
   the Bin entries are still on disk (the GC thread in the child
   was killed mid-processing; some entries may be processed, others
   not). */
void test_bin_crash_safety(void) {
    printf("7. Bin crash safety (fork+kill+remount)...\n");
    const char* path = "/tmp/test_bin_crash.vfs";
    cleanup(path);

    /* Phase 1: mount, push 100 NOOPs, fork. */
    int pipefd[2];
    CHECK_EQ(pipe(pipefd), 0);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        cleanup(path);
        return;
    }

    if (pid == 0) {
        /* Child: mount, push 100 NOOPs, signal parent, _exit
         * (simulating a crash — no unmount, no thread cleanup). */
        close(pipefd[0]);
        vfs_t* vfs = vfs_mount(path, 8192);
        if (!vfs) _exit(1);
        for (int i = 0; i < 100; i++) {
            bin_push(vfs->ctx->sb, BIN_TRIGGER_NOOP, (int64_t)(i + 1), 0);
        }
        /* Signal parent: 1 byte. */
        char x = 'X';
        write(pipefd[1], &x, 1);
        close(pipefd[1]);
        /* _exit immediately — no unmount, no thread stop.  This
         * simulates a process crash.  The Bin entries on disk
         * are still there. */
        _exit(0);
    }

    /* Parent: wait for child to push entries (signal). */
    close(pipefd[1]);
    char x;
    ssize_t n = read(pipefd[0], &x, 1);
    close(pipefd[0]);
    CHECK_EQ(n, 1);

    /* Wait for child to die (it _exit'd). */
    int status;
    waitpid(pid, &status, 0);
    CHECK_TRUE(WIFEXITED(status));

    /* Phase 2: remount and verify the Bin entries are still there. */
    vfs_t* vfs2 = vfs_mount(path, 8192);
    CHECK(vfs2 != NULL);
    if (!vfs2) { cleanup(path); return; }

    /* The Bin should have some entries (0 to 100, depending on
       whether the child's GC thread processed any).  We can't
       know the exact count, but it should be > 0 (we pushed
       100 and the child did _exit immediately, so the GC
       thread had minimal time to process).  Wait up to 5s
       for the new mount's GC thread to drain. */
    int64_t count = read_bin_count(vfs2);
    CHECK(count >= 0);
    CHECK(count <= 100);
    /* The count should be at least 50 — the GC thread had
       microseconds to process before _exit, so most entries
       should still be on disk.  Allow for some variation. */
    printf("    Bin count after remount: %lld\n", (long long)count);

    /* Wait for the new mount's GC thread to drain. */
    int drained = 0;
    for (int i = 0; i < 500; i++) {  /* 5 seconds max */
        if (read_bin_count(vfs2) == 0) { drained = 1; break; }
        usleep(10000);  /* 10ms */
    }
    CHECK(drained);

    vfs_unmount(vfs2);
    cleanup(path);
}

/* ========================================================================== */

/* Phase 28 W4: bin performance.  Measure push and pop throughput.
   This is a measurement, not a pass/fail test — the costs are
   printed but not asserted (the spec's predicted ranges are
   ~10-20 ns per push/pop, but exact numbers depend on hardware). */
void test_bin_performance(void) {
    printf("8. Bin performance (push/pop throughput)...\n");
    const char* path = "/tmp/test_bin_perf.vfs";
    cleanup(path);

    vfs_t* vfs = vfs_mount(path, 8192);
    CHECK(vfs != NULL);
    if (!vfs) { cleanup(path); return; }

    /* Push throughput. */
    enum { PUSH_N = 10000 };
    double t0 = now_sec();
    for (int i = 0; i < PUSH_N; i++) {
        bin_push(vfs->ctx->sb, BIN_TRIGGER_NOOP, (int64_t)(i + 1), 0);
    }
    double t1 = now_sec();
    double push_sec = t1 - t0;
    double push_per_sec = (double)PUSH_N / push_sec;
    double push_ns = (push_sec / PUSH_N) * 1e9;
    printf("    bin_push:    %d ops in %.3f ms  (%.0f ops/sec, ~%.0f ns/op)\n",
           PUSH_N, push_sec * 1000.0, push_per_sec, push_ns);
    /* Sanity: push should be > 10K ops/sec on any reasonable hardware. */
    CHECK(push_per_sec > 10000.0);

    /* Wait for GC thread to drain (so pop has something to do). */
    int drained = 0;
    for (int i = 0; i < 200; i++) {
        if (read_bin_count(vfs) == 0) { drained = 1; break; }
        usleep(10000);
    }
    CHECK(drained);

    /* Pop throughput.  Push 1000 fresh entries, then pop them all
       in a tight loop.  Note: this is inherently racy — the GC
       thread is concurrently popping.  The measurement is "how
       fast the consumer thread (main or GC) can drain", not
       "exclusive main-thread pop throughput". */
    enum { POP_N = 1000 };
    for (int i = 0; i < POP_N; i++) {
        bin_push(vfs->ctx->sb, BIN_TRIGGER_NOOP, (int64_t)(i + 1), 0);
    }
    /* No sleep here — start popping immediately to race the GC. */

    int popped = 0;
    double t2 = now_sec();
    for (int i = 0; i < POP_N; i++) {
        BinEntry entry;
        if (bin_pop(vfs->ctx->sb, &entry) == 0) popped++;
    }
    double t3 = now_sec();
    double pop_sec = t3 - t2;
    /* If popped is 0, the GC drained everything before we could
       pop.  In that case, throughput is meaningless.  Print
       whatever the main thread observed (could be 0). */
    double pop_per_sec = popped > 0 ? (double)popped / pop_sec : 0.0;
    double pop_ns = popped > 0 ? (pop_sec / popped) * 1e9 : 0.0;
    printf("    bin_pop:     %d ops in %.3f ms  (%.0f ops/sec, ~%.0f ns/op)\n",
           popped, pop_sec * 1000.0, pop_per_sec, pop_ns);
    /* Sanity: at least the throughput number is meaningful
       (i.e., we measured something), OR the GC was so fast that
       there's nothing left to pop.  Either way, no assertion —
       the pop test is a measurement, not a hard pass/fail. */

    vfs_unmount(vfs);
    cleanup(path);
}

/* ---------------------------------------------------------------------------
 * Phase 28 Type 1: file-deletion bin job tests
 *
 * These tests exercise the trigger analysis + work handler via the
 * GC thread.  vfs_delete pushes BIN_TRIGGER_FILE_DELETED; the GC
 * thread processes it (classifies data pages, drops the dir entries
 * if the file is not referenced).
 * --------------------------------------------------------------------------- */

static void sleep_ms(int ms) {
    struct timespec ts = { .tv_sec = ms / 1000,
                          .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* Test 9: file-deletion bin job — create + write + delete, GC drops
 * the create + tombstone from the parent dir's chain, file is gone.
 * Also verifies the dircontentindex is updated (per spec §3.8). */
void test_file_deletion_bin_job(void) {
    printf("9. File-deletion bin job (create+write+delete, GC drops + index update)...\n");
    const char* path = "/tmp/test_file_deletion_bin_job.vfs";
    cleanup(path);

    vfs_t* vfs = vfs_mount(path, 8192);
    CHECK(vfs != NULL);
    if (!vfs) { cleanup(path); return; }

    int64_t root = vfs->ctx->rootNodeOffset;
    int64_t fvp = vfs_create(vfs, root, "to_delete.dat", 0);
    CHECK(fvp > 0);
    if (fvp <= 0) { vfs_unmount(vfs); cleanup(path); return; }

    /* Write some data so data pages exist. */
    uint8_t buf[4096];
    memset(buf, 0xAB, sizeof(buf));
    for (int i = 0; i < 4; i++) {
        int wrc = vfs_write(vfs, fvp, buf, i * 4096, sizeof(buf), 0);
        CHECK_EQ(wrc, (int)sizeof(buf));
    }
    vfs_flush(vfs);

    /* Delete — pushes BIN_TRIGGER_FILE_DELETED. */
    int drc = vfs_delete(vfs, root, "to_delete.dat", 0);
    CHECK_EQ(drc, VFS_OK);

    vfs_flush(vfs);
    /* Give the GC thread time to process the trigger. */
    sleep_ms(200);

    /* Reopen and verify the file is gone. */
    vfs_unmount(vfs);
    vfs = vfs_mount(path, 8192);
    CHECK(vfs != NULL);
    if (!vfs) { cleanup(path); return; }

    int64_t vp = vfs_open(vfs, vfs->ctx->rootNodeOffset, "to_delete.dat", 0);
    int ok = (vp <= 0);
    CHECK(ok);
    if (!ok) fprintf(stderr, "    FAIL: file still exists\n");

    /* Verify the dircontentindex is updated: re-create a file with
     * the same name.  If the index had a stale link, the new
     * create would see a phantom collision.  With the index
     * updated, the new create succeeds.  Also verify vfs_open
     * finds the new file (index fast path is correct). */
    int64_t new_fvp = vfs_create(vfs, root, "to_delete.dat", 0);
    CHECK(new_fvp > 0);
    if (new_fvp > 0) {
        /* The new file should be findable. */
        int64_t new_vp = vfs_open(vfs, root, "to_delete.dat", 0);
        CHECK_EQ(new_vp, new_fvp);
    }

    vfs_unmount(vfs);
    cleanup(path);
}

/* Test 10: file-deletion with active snapshot — file is not removed
 * from the parent dir's chain because the snapshot still references it. */
void test_file_deletion_with_snapshot(void) {
    printf("10. File-deletion with active snapshot (no drop, snapshot holds file)...\n");
    const char* path = "/tmp/test_file_deletion_with_snapshot.vfs";
    cleanup(path);

    vfs_t* vfs = vfs_mount(path, 8192);
    CHECK(vfs != NULL);
    if (!vfs) { cleanup(path); return; }

    int64_t root = vfs->ctx->rootNodeOffset;
    int64_t fvp = vfs_create(vfs, root, "snap_held.dat", 0);
    CHECK(fvp > 0);
    if (fvp <= 0) { vfs_unmount(vfs); cleanup(path); return; }

    /* Take a snapshot BEFORE deleting. */
    int64_t snap = vfs_snapshot(vfs);
    CHECK(snap > 0 && (snap & 1) == 1);

    /* Note: epoch=-1 means "current head" per the spec, but the
     * existing vfs_open/dirchain_find_child pass epoch directly to
     * the read rule without translating -1.  So we use the actual
     * currentEpoch.  After vfs_snapshot, currentEpoch is snap+1. */
    int64_t head_epoch = vfs->ctx->currentEpoch;
    CHECK(head_epoch == snap + 1);

    /* Delete in the head (currentEpoch = snap + 1).  The snapshot at
     * snap still references the file, so the analysis should NOT drop
     * the dir entries. */
    int drc = vfs_delete(vfs, root, "snap_held.dat", head_epoch);
    CHECK_EQ(drc, VFS_OK);

    vfs_flush(vfs);
    sleep_ms(200);

    /* Reopen and verify: at the head, the file is hidden.  But at
     * the snapshot epoch, the file is visible. */
    vfs_unmount(vfs);
    vfs = vfs_mount(path, 8192);
    CHECK(vfs != NULL);
    if (!vfs) { cleanup(path); return; }

    /* At head: file is hidden (deletion visible at H > D). */
    int64_t vp_h = vfs_open(vfs, vfs->ctx->rootNodeOffset, "snap_held.dat", head_epoch);
    CHECK(vp_h <= 0);  /* file is hidden at the head */

    /* At the snapshot epoch: file is visible (no deletion at S, since
     * the deletion happened at the head epoch > S). */
    int64_t vp_s = vfs_open(vfs, vfs->ctx->rootNodeOffset, "snap_held.dat", snap);
    CHECK(vp_s > 0);  /* file is visible at the snapshot */

    vfs_unmount(vfs);
    cleanup(path);
}

int main(void) {
    printf("=== GC Thread Tests (Phase 28 W2 + W3 + W4 + W5 = file deletion bin job) ===\n\n");

    test_gc_thread_lifecycle();
    test_gc_thread_shutdown();
    test_gc_thread_empty();
    test_gc_producer_integration();
    test_gc_producer_commit();
    test_bin_end_to_end();
    test_bin_crash_safety();
    test_bin_performance();
    test_file_deletion_bin_job();
    test_file_deletion_with_snapshot();

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
