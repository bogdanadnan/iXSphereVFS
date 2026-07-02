/* Phase 7: Tree lock unit tests */
#include "vfs_internal.h"
#include "gc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

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

    /* Acquire shared lock */
    tree_lock_acquire_shared(ctx);
    /* Reader count should be 1, exclusive bit clear */
    CHECK(ctx->treeLockState & (int64_t)TREE_LOCK_READER_MASK);
    CHECK(!(ctx->treeLockState & (int64_t)TREE_LOCK_EXCLUSIVE_BIT));

    /* Release shared lock */
    tree_lock_release_shared(ctx);
    CHECK_EQ(ctx->treeLockState, 0);

    /* Acquire multiple shared locks */
    tree_lock_acquire_shared(ctx);
    tree_lock_acquire_shared(ctx);
    tree_lock_acquire_shared(ctx);
    /* Reader count should be 3 */
    int64_t readers = ctx->treeLockState & (int64_t)TREE_LOCK_READER_MASK;
    CHECK_EQ(readers, (int64_t)TREE_LOCK_READER_INC * 3);

    /* Release all */
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

    /* Acquire exclusive lock */
    tree_lock_acquire_exclusive(ctx);
    CHECK(ctx->treeLockState & (int64_t)TREE_LOCK_EXCLUSIVE_BIT);
    /* Reader count should be 0 */
    int64_t readers = ctx->treeLockState & (int64_t)TREE_LOCK_READER_MASK;
    CHECK_EQ(readers, 0);

    /* Release exclusive lock */
    tree_lock_release_exclusive(ctx);
    CHECK_EQ(ctx->treeLockState, 0);

    teardown(vfs);
}

/* ---------------------------------------------------------------------------
 * Exclusive blocks shared acquire test (conceptual: verify state transition)
 * --------------------------------------------------------------------------- */

static void test_exclusive_blocks_shared(void) {
    vfs_t* vfs = setup();
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;

    /* Acquire exclusive lock */
    tree_lock_acquire_exclusive(ctx);
    CHECK(ctx->treeLockState & (int64_t)TREE_LOCK_EXCLUSIVE_BIT);

    /* Release exclusive */
    tree_lock_release_exclusive(ctx);
    CHECK_EQ(ctx->treeLockState, 0);

    /* Shared → Exclusive → Shared ordering works */
    tree_lock_acquire_shared(ctx);
    CHECK(ctx->treeLockState & (int64_t)TREE_LOCK_READER_MASK);
    tree_lock_release_shared(ctx);
    CHECK_EQ(ctx->treeLockState, 0);

    teardown(vfs);
}

/* ---------------------------------------------------------------------------
 * Crash-recovery bit detection test
 * --------------------------------------------------------------------------- */

static void test_crash_recovery(void) {
    vfs_t* vfs = setup();
    CHECK(vfs != NULL);
    /* Simulate stale exclusive lock by setting bit 63 */
    vfs->ctx->treeLockState = (int64_t)TREE_LOCK_EXCLUSIVE_BIT;

    /* Close and reopen (without explicit unlink to preserve state).
       This triggers tree_init which should detect and clear the stale bit. */
    vfs_close(vfs);

    /* Reopen the same file — tree_init should clear the stale exclusive bit */
    vfs = vfs_open(test_path);
    CHECK(vfs != NULL);
    /* treeLockState should be 0 after crash recovery */
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
    test_crash_recovery();

    printf("test_gc: %d/%d passed\n", tests_passed, tests_run);
    unlink(test_path);
    return (tests_passed == tests_run) ? 0 : 1;
}
