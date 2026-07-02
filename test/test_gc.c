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
 * main
 * --------------------------------------------------------------------------- */

int main(void) {
    test_shared_lock();
    test_exclusive_lock();
    test_exclusive_blocks_shared();
    test_exclusive_drains_readers();
    test_crash_recovery();

    printf("test_gc: %d/%d passed\n", tests_passed, tests_run);
    unlink(test_path);
    return (tests_passed == tests_run) ? 0 : 1;
}
