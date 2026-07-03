/* Phase 7: Tree lock unit tests */
#include "vfs_internal.h"
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

    /* Enqueue a page — mirror sibling check uses sb->mirror_pages.
       For a fresh SB, mirror_pages is uninitialized (NULL/calloc'd to 0).
       Enqueuing a page with no mirror should not add extra entries. */
    deferred_free_enqueue(&q, 5, sb);
    CHECK_EQ(q.count, 1);  /* just page 5, no mirror */

    /* Enqueue another page */
    deferred_free_enqueue(&q, 6, sb);
    CHECK_EQ(q.count, 2);

    /* Verify both are in the queue */
    CHECK(deferred_free_is_queued(&q, 5));
    CHECK(deferred_free_is_queued(&q, 6));

    deferred_free_destroy(&q);
    df_teardown(sb);
}

/* ---------------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------------- */

int main(void) {
    test_shared_lock();
    test_exclusive_lock();
    test_exclusive_blocks_shared();
    test_exclusive_drains_readers();
    test_crash_recovery();

    test_df_enqueue_blocks_alloc();
    test_df_confirm_releases();
    test_df_mirror_sibling();

    printf("test_gc: %d/%d passed\n", tests_passed, tests_run);
    unlink(test_path);
    return (tests_passed == tests_run) ? 0 : 1;
}
