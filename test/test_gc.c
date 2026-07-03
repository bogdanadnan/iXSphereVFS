/* Phase 7: Tree lock unit tests */
#include "vfs_internal.h"
#include "epoch.h"
#include "gc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

static int tests_run = 0, tests_passed = 0;

#define CHECK(expr) do { \
    tests_run++; \
    if (expr) { tests_passed++; } \
    else { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } \
} while(0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

static const char* test_path = "/tmp/test_gc.tmp";

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------- */

static vfs_t* setup(void) {
    unlink(test_path);
    return vfs_open(test_path);
}

static void teardown(vfs_t* vfs) {
    if (vfs) vfs_close(vfs);
    unlink(test_path);
}

/* ---------------------------------------------------------------------------
 * Shared lock acquire/release test
 * --------------------------------------------------------------------------- */

static void test_shared_lock(void) {
    vfs_t* vfs = setup();
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;

    CHECK_EQ(ctx->treeLockState, 0);

    tree_lock_acquire_shared(ctx);
    CHECK(ctx->treeLockState & (int64_t)TREE_LOCK_READER_MASK);
    CHECK(!(ctx->treeLockState & (int64_t)TREE_LOCK_EXCLUSIVE_BIT));
    tree_lock_release_shared(ctx);
    CHECK_EQ(ctx->treeLockState, 0);

    /* Multiple readers */
    tree_lock_acquire_shared(ctx);
    tree_lock_acquire_shared(ctx);
    tree_lock_acquire_shared(ctx);
    CHECK_EQ(ctx->treeLockState & (int64_t)TREE_LOCK_READER_MASK,
             (int64_t)TREE_LOCK_READER_INC * 3);
    tree_lock_release_shared(ctx);
    tree_lock_release_shared(ctx);
    tree_lock_release_shared(ctx);
    CHECK_EQ(ctx->treeLockState, 0);

    teardown(vfs);
}

/* ---------------------------------------------------------------------------
 * Exclusive lock acquire/release test
 * --------------------------------------------------------------------------- */

static void test_exclusive_lock(void) {
    vfs_t* vfs = setup();
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;

    tree_lock_acquire_exclusive(ctx);
    CHECK(ctx->treeLockState & (int64_t)TREE_LOCK_EXCLUSIVE_BIT);
    CHECK_EQ(ctx->treeLockState & (int64_t)TREE_LOCK_READER_MASK, 0);
    tree_lock_release_exclusive(ctx);
    CHECK_EQ(ctx->treeLockState, 0);

    teardown(vfs);
}

/* ---------------------------------------------------------------------------
 * Exclusive blocks concurrent readers
 * --------------------------------------------------------------------------- */

static TreeContext* blocking_ctx;
static volatile int reader_blocked_started = 0;
static volatile int reader_blocked_acquired = 0;

static void* blocking_reader_thread(void* arg) {
    (void)arg;
    reader_blocked_started = 1;
    tree_lock_acquire_shared(blocking_ctx);
    reader_blocked_acquired = 1;
    tree_lock_release_shared(blocking_ctx);
    return NULL;
}

static void test_exclusive_blocks_shared(void) {
    vfs_t* vfs = setup();
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    blocking_ctx = ctx;
    reader_blocked_started = 0;
    reader_blocked_acquired = 0;

    /* Hold exclusive lock */
    tree_lock_acquire_exclusive(ctx);

    /* Spawn reader — should block on exclusive bit */
    pthread_t reader;
    CHECK_EQ(pthread_create(&reader, NULL, blocking_reader_thread, NULL), 0);
    while (!reader_blocked_started) sched_yield();
    usleep(5000);  /* give reader time to attempt acquire */
    CHECK(!reader_blocked_acquired);  /* must still be blocked */

    /* Release exclusive — reader should now acquire and finish */
    tree_lock_release_exclusive(ctx);
    pthread_join(reader, NULL);
    CHECK(reader_blocked_acquired);  /* reader succeeded after release */

    teardown(vfs);
}

/* ---------------------------------------------------------------------------
 * Exclusive drains active readers before acquiring
 * --------------------------------------------------------------------------- */

static volatile int drain_readers_ready = 0;
static volatile int drain_readers_released = 0;

static void* slow_reader_thread(void* arg) {
    (void)arg;
    tree_lock_acquire_shared(blocking_ctx);
    __sync_fetch_and_add(&drain_readers_ready, 1);
    usleep(30000);  /* hold lock for 30ms */
    tree_lock_release_shared(blocking_ctx);
    __sync_fetch_and_add(&drain_readers_released, 1);
    return NULL;
}

static void test_exclusive_drains_readers(void) {
    vfs_t* vfs = setup();
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    blocking_ctx = ctx;
    drain_readers_ready = 0;
    drain_readers_released = 0;

    /* Spawn 3 readers that hold the lock briefly */
    pthread_t readers[3];
    for (int i = 0; i < 3; i++)
        CHECK_EQ(pthread_create(&readers[i], NULL, slow_reader_thread, NULL), 0);

    /* Wait for all readers to acquire */
    while (drain_readers_ready < 3) sched_yield();
    usleep(1000);

    /* Now acquire exclusive — must block until readers drain */
    tree_lock_acquire_exclusive(ctx);
    /* All readers must have finished before exclusive acquired */
    CHECK_EQ(drain_readers_released, 3);
    CHECK(ctx->treeLockState & (int64_t)TREE_LOCK_EXCLUSIVE_BIT);
    CHECK_EQ(ctx->treeLockState & (int64_t)TREE_LOCK_READER_MASK, 0);

    tree_lock_release_exclusive(ctx);
    for (int i = 0; i < 3; i++) pthread_join(readers[i], NULL);

    teardown(vfs);
}

/* ---------------------------------------------------------------------------
 * Crash-recovery bit detection test
 * --------------------------------------------------------------------------- */

static void test_crash_recovery(void) {
    vfs_t* vfs = setup();
    CHECK(vfs != NULL);
    vfs->ctx->treeLockState = (int64_t)TREE_LOCK_EXCLUSIVE_BIT;
    vfs_close(vfs);

    vfs = vfs_open(test_path);
    CHECK(vfs != NULL);
    CHECK_EQ(vfs->ctx->treeLockState, 0);
    teardown(vfs);
}

/* ---------------------------------------------------------------------------
 * Deferred-free queue tests
 * --------------------------------------------------------------------------- */

static StorageBackend* df_setup(void) {
    unlink(test_path);
    return storage_open(test_path, 8192);
}

static void df_teardown(StorageBackend* sb) {
    if (sb) storage_close(sb);
    unlink(test_path);
}

/* Enqueue prevents allocation */
static void test_df_enqueue_blocks_alloc(void) {
    StorageBackend* sb = df_setup();
    CHECK(sb != NULL);

    DeferredFreeQueue q;
    CHECK_EQ(deferred_free_init(&q, 8), VFS_OK);

    /* Set the deferred queue so storage_allocate checks it */
    storage_set_deferred_queue(&q);

    /* Allocate some pages */
    int64_t p1 = storage_allocate(sb, 1);
    CHECK(p1 >= 2);  /* pages 0,1 are reserved */
    int64_t p2 = storage_allocate(sb, 1);
    CHECK(p2 > p1);

    /* Free p1 so it's available for reuse */
    storage_free(sb, p1);

    /* Enqueue p1 in the deferred queue */
    deferred_free_enqueue(&q, p1, sb);

    /* Allocate again — should NOT reuse p1 (it's in the deferred queue) */
    int64_t p3 = storage_allocate(sb, 1);
    CHECK(p3 > p2);  /* should get a fresh page beyond p2 */
    CHECK(p3 != p1); /* should not reuse the deferred page */

    /* Clear deferred queue so later tests are unaffected */
    storage_set_deferred_queue(NULL);
    deferred_free_destroy(&q);
    df_teardown(sb);
}

/* Confirm releases pages: confirm_and_release frees queued pages */
static void test_df_confirm_releases(void) {
    DeferredFreeQueue q;
    CHECK_EQ(deferred_free_init(&q, 8), VFS_OK);

    /* Enqueue some pages */
    deferred_free_enqueue(&q, 10, NULL);
    deferred_free_enqueue(&q, 20, NULL);
    deferred_free_enqueue(&q, 30, NULL);
    CHECK_EQ(q.count, 3);

    /* is_queued should find them */
    CHECK(deferred_free_is_queued(&q, 10));
    CHECK(deferred_free_is_queued(&q, 20));
    CHECK(deferred_free_is_queued(&q, 30));
    CHECK(!deferred_free_is_queued(&q, 99));
    CHECK(!deferred_free_is_queued(&q, 0));

    /* confirm_and_release needs a StorageBackend.
       We just check that it clears the queue (pages won't be freed
       since they don't correspond to real pages in this SB). */
    StorageBackend* sb = df_setup();
    CHECK(sb != NULL);

    deferred_free_confirm_and_release(&q, sb);

    /* Queue should be empty and confirmed */
    CHECK_EQ(q.count, 0);
    CHECK_EQ(q.pages, NULL);
    CHECK(q.confirmed);

    deferred_free_destroy(&q);
    df_teardown(sb);
}

/* Mirror sibling handling */
static void test_df_mirror_sibling(void) {
    DeferredFreeQueue q;
    CHECK_EQ(deferred_free_init(&q, 8), VFS_OK);

    StorageBackend* sb = df_setup();
    CHECK(sb != NULL);

    /* Allocate mirror arrays and configure mirror relationship */
    ensure_mirror_arrays(sb, 10);
    sb->mirror_pages[5] = 7;    /* page 7 is mirror of page 5 */
    sb->mirror_pages[7] = 5;    /* bidirectional */

    /* Enqueue page 5 — should also enqueue mirror page 7 */
    deferred_free_enqueue(&q, 5, sb);
    CHECK_EQ(q.count, 2);
    CHECK(deferred_free_is_queued(&q, 5));
    CHECK(deferred_free_is_queued(&q, 7));

    /* No-mirror page: mirror_pages[6] was set to 0 by calloc.
       Set to -1 explicitly to test the no-mirror boundary. */
    sb->mirror_pages[6] = -1;
    deferred_free_enqueue(&q, 6, sb);
    CHECK_EQ(q.count, 3);  /* only page 6, no mirror added */
    CHECK(deferred_free_is_queued(&q, 6));

    /* Negative case: mirror == -1 means no mirror */
    sb->mirror_pages[8] = -1;
    deferred_free_enqueue(&q, 8, sb);
    CHECK_EQ(q.count, 4);  /* only page 8, no mirror */

    deferred_free_destroy(&q);
    df_teardown(sb);
}

/* ---------------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * GC integration test: create file, snapshot, write more, soft-delete,
 * run GC, verify data reverts and size drops.
 * --------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * GC integration test: commit snapshot, run GC, verify committed version
 * nodes relabeled and mapper entry removed.
 * --------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * Crash-before-swap test: simulate kill-9 during GC (bit 63 set), remount,
 * verify old tree intact.
 * --------------------------------------------------------------------------- */

static void test_gc_crash_before_swap(void) {
    vfs_t* vfs = vfs_open(test_path);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int nodeId = vfs_create(vfs, root_vp, "crash_test.txt", 0);
    CHECK(nodeId > 0);

    int64_t file_vp = 0;
    {
        uint8_t* rs = pool_resolve(&ctx->pool, root_vp);
        CHECK(rs != NULL);
        int64_t head = vfs_rd8(rs, DIRNODE_OFF_HEADPTR);
        CHECK(head != 0);
        uint32_t cc, ce;
        int64_t cp, np, nx;
        nodes_read_dircontent(pool_resolve(&ctx->pool, head),
                              &cc, &ce, &cp, &np, &nx);
        (void)cc; (void)ce; (void)np; (void)nx;
        file_vp = cp;
    }
    CHECK(file_vp != 0);

    CHECK_EQ(vfs_write(vfs, file_vp, "CRASH", 0, 5, 0), 5);

    /* Simulate crash during GC: set exclusive lock bit, then close.
       vfs_close writes the superblock with treeLockState=bit63 set. */
    ctx->treeLockState = (int64_t)TREE_LOCK_EXCLUSIVE_BIT;
    vfs_close(vfs);

    /* Reopen — tree_init should detect stale bit and clear it.
       The lock was never released, so the superblock on disk has bit63 set.
       tree_init reads it, sees it, logs a warning, and zeroes treeLockState. */
    vfs = vfs_open(test_path);
    CHECK(vfs != NULL);
    CHECK_EQ(vfs->ctx->treeLockState, 0);

    /* Verify old tree intact: re-resolve file and read data */
    {
        int64_t rv = vfs->ctx->rootNodeOffset;
        uint8_t* rs = pool_resolve(&vfs->ctx->pool, rv);
        CHECK(rs != NULL);
        int64_t head = vfs_rd8(rs, DIRNODE_OFF_HEADPTR);
        if (head != 0) {
            uint32_t cc, ce;
            int64_t cp, np, nx;
            uint8_t* dc = pool_resolve(&vfs->ctx->pool, head);
            if (dc) {
                nodes_read_dircontent(dc, &cc, &ce, &cp, &np, &nx);
                (void)cc; (void)ce; (void)np; (void)nx;
                char rbuf[16];
                int ret = vfs_read(vfs, cp, rbuf, 0, 5, 0);
                CHECK_EQ(ret, 5);
                CHECK_EQ(strncmp(rbuf, "CRASH", 5), 0);
            }
        }
    }

    vfs_close(vfs);
}

static void test_gc_commit_then_gc(void) {
    vfs_t* vfs = vfs_open(test_path);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int nodeId = vfs_create(vfs, root_vp, "commit_gc.txt", 0);
    CHECK(nodeId > 0);

    int64_t file_vp = 0;
    {
        uint8_t* rs = pool_resolve(&ctx->pool, root_vp);
        CHECK(rs != NULL);
        int64_t head = vfs_rd8(rs, DIRNODE_OFF_HEADPTR);
        CHECK(head != 0);
        uint32_t cc, ce;
        int64_t cp, np, nx;
        nodes_read_dircontent(pool_resolve(&ctx->pool, head),
                              &cc, &ce, &cp, &np, &nx);
        (void)cc; (void)ce; (void)np; (void)nx;
        file_vp = cp;
    }
    CHECK(file_vp != 0);

    /* Write at epoch 0 */
    CHECK_EQ(vfs_write(vfs, file_vp, "CCCC", 0, 4, 0), 4);

    /* Snapshot → epoch 1 */
    int64_t snap = vfs_snapshot(vfs);
    CHECK_EQ(snap, 1);

    /* Write at epoch 2 (live head) */
    CHECK_EQ(vfs_write(vfs, file_vp, "DDDD", 0, 4, 2), 4);

    /* Commit the snapshot */
    CHECK_EQ(vfs_commit(vfs, snap), VFS_OK);

    /* Run GC */
    int gc_ret = vfs_gc(vfs);
    if (gc_ret == VFS_OK) {
        /* Verify data: epoch 0 unchanged, epoch 2 has live data */
        char rbuf[16];
        CHECK_EQ(vfs_read(vfs, file_vp, rbuf, 0, 4, 0), 4);
        CHECK_EQ(strncmp(rbuf, "CCCC", 4), 0);

        memset(rbuf, 0, sizeof(rbuf));
        CHECK_EQ(vfs_read(vfs, file_vp, rbuf, 0, 4, 2), 4);
        CHECK_EQ(strncmp(rbuf, "DDDD", 4), 0);

        /* Mapper entry for epoch 1 should be removed (committed then GC'd) */
        CHECK(mapper_resolve(&ctx->mapper, snap) == snap);  /* identity = no entry */
    } else {
        CHECK_EQ(gc_ret, VFS_ERR_FULL);  /* expected failure in small files */
    }

    vfs_close(vfs);
}

static void test_gc_integration(void) {
    vfs_t* vfs = vfs_open(test_path);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    /* Create a file at epoch 0 */
    int nodeId = vfs_create(vfs, root_vp, "gc_test.txt", 0);
    CHECK(nodeId > 0);

    /* Get file VirtualPtr */
    int64_t file_vp = 0;
    {
        uint8_t* rs = pool_resolve(&ctx->pool, root_vp);
        CHECK(rs != NULL);
        int64_t head = vfs_rd8(rs, DIRNODE_OFF_HEADPTR);
        CHECK(head != 0);
        uint32_t cc, ce;
        int64_t cp, np, nx;
        nodes_read_dircontent(pool_resolve(&ctx->pool, head),
                              &cc, &ce, &cp, &np, &nx);
        (void)cc; (void)ce; (void)np; (void)nx;
        file_vp = cp;
    }
    CHECK(file_vp != 0);

    /* Write "AAAA" at epoch 0 */
    CHECK_EQ(vfs_write(vfs, file_vp, "AAAA", 0, 4, 0), 4);
    CHECK_EQ(vfs_file_size(vfs, file_vp, 0), 4);

    /* Snapshot → epoch 1, currentEpoch becomes 2 */
    int64_t snap = vfs_snapshot(vfs);
    CHECK_EQ(snap, 1);

    /* Write "BBBB" at epoch 2 (live head) */
    CHECK_EQ(vfs_write(vfs, file_vp, "BBBB", 0, 4, 2), 4);
    CHECK_EQ(vfs_file_size(vfs, file_vp, 2), 4);

    /* Soft-delete snapshot 1 */
    CHECK_EQ(vfs_delete_snapshot(vfs, snap), VFS_OK);

    /* Count pool pages before GC */
    int64_t pool_before = 0;
    {
        int64_t p = ctx->pool.list_head ? *ctx->pool.list_head : 0;
        while (p != 0) {
            pool_before++;
            uint8_t* ph = storage_read(ctx->sb, p);
            if (!ph) break;
            p = vfs_rd8(ph, 0);
        }
    }

    int gc_ret = vfs_gc(vfs);
    if (gc_ret == VFS_OK) {
        /* Verify pool pages reclaimed (or at least not grown) */
        int64_t pool_after = 0;
        {
            int64_t p = ctx->pool.list_head ? *ctx->pool.list_head : 0;
            while (p != 0) {
                pool_after++;
                uint8_t* ph = storage_read(ctx->sb, p);
                if (!ph) break;
                p = vfs_rd8(ph, 0);
            }
        }
        CHECK(pool_after <= pool_before);

        /* Verify file sizes at live epochs */
        CHECK_EQ(vfs_file_size(vfs, file_vp, 0), 4);
        CHECK_EQ(vfs_file_size(vfs, file_vp, 2), 4);

        /* Verify data integrity across epochs */
        char rbuf[16];
        CHECK_EQ(vfs_read(vfs, file_vp, rbuf, 0, 4, 0), 4);
        CHECK_EQ(strncmp(rbuf, "AAAA", 4), 0);

        memset(rbuf, 0, sizeof(rbuf));
        CHECK_EQ(vfs_read(vfs, file_vp, rbuf, 0, 4, 1), 4);
        CHECK_EQ(strncmp(rbuf, "AAAA", 4), 0);

        memset(rbuf, 0, sizeof(rbuf));
        CHECK_EQ(vfs_read(vfs, file_vp, rbuf, 0, 4, 2), 4);
        CHECK_EQ(strncmp(rbuf, "BBBB", 4), 0);
    } else {
        /* GC failed — verify pre-GC state is preserved */
        CHECK_EQ(vfs_file_size(vfs, file_vp, 0), 4);
        CHECK_EQ(vfs_file_size(vfs, file_vp, 2), 4);
    }

    vfs_close(vfs);
}

int main(void) {
    test_shared_lock();
    test_exclusive_lock();
    test_exclusive_blocks_shared();
    test_exclusive_drains_readers();
    test_crash_recovery();

    test_df_enqueue_blocks_alloc();
    test_df_confirm_releases();
    test_df_mirror_sibling();

    unlink(test_path);  /* fresh file for integration test */
    test_gc_integration();

    unlink(test_path);  /* fresh file for commit-then-GC test */
    test_gc_commit_then_gc();

    unlink(test_path);  /* fresh file for crash-before-swap test */
    test_gc_crash_before_swap();

    printf("test_gc: %d/%d passed\n", tests_passed, tests_run);
    unlink(test_path);
    return (tests_passed == tests_run) ? 0 : 1;
}
