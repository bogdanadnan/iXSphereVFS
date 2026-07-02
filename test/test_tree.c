/* Phase 5a: Tree bootstrap tests */
#include "vfs_internal.h"
#include "tree.h"
#include "nodes.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static int tests_run = 0, tests_passed = 0;

#define CHECK(expr) do { \
    tests_run++; \
    if (expr) { tests_passed++; } \
    else { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } \
} while(0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

/* ---------------------------------------------------------------------------
 * Bootstrap test
 * --------------------------------------------------------------------------- */

static const char* test_path = "/tmp/test_tree_bootstrap.tmp";

static void test_bootstrap_root(void) {
    /* Open fresh file → bootstrap creates root */
    vfs_t* vfs = vfs_open(test_path);
    CHECK(vfs != NULL);
    CHECK(vfs->ctx != NULL);

    TreeContext* ctx = vfs->ctx;

    /* Root DirNode must exist with correct fields */
    CHECK(ctx->rootNodeOffset != 0);

    uint8_t* root_slot = pool_resolve(&ctx->pool, ctx->rootNodeOffset);
    CHECK(root_slot != NULL);

    /* Verify root DirNode: nodeId=0, type=0x01, headPtr=0 */
    int16_t type = vfs_rd2(root_slot, DIRNODE_OFF_TYPE);
    CHECK_EQ((int)type, (int)NODE_TYPE_DIR);

    uint32_t nodeId = (uint32_t)vfs_rd4(root_slot, DIRNODE_OFF_NODEID);
    CHECK_EQ(nodeId, 0u);

    int64_t headPtr = vfs_rd8(root_slot, DIRNODE_OFF_HEADPTR);
    CHECK_EQ(headPtr, 0);

    /* nextNodeId should be 1 (0 used for root) */
    CHECK_EQ((int)ctx->nextNodeId, 1);

    vfs_close(vfs);
}

static void test_bootstrap_reopen(void) {
    /* Open existing file → reopen */
    vfs_t* vfs = vfs_open(test_path);
    CHECK(vfs != NULL);
    CHECK(vfs->ctx != NULL);

    TreeContext* ctx = vfs->ctx;

    /* Root must still exist */
    CHECK(ctx->rootNodeOffset != 0);

    uint8_t* root_slot = pool_resolve(&ctx->pool, ctx->rootNodeOffset);
    CHECK(root_slot != NULL);

    int16_t type = vfs_rd2(root_slot, DIRNODE_OFF_TYPE);
    CHECK_EQ((int)type, (int)NODE_TYPE_DIR);

    uint32_t nodeId = (uint32_t)vfs_rd4(root_slot, DIRNODE_OFF_NODEID);
    CHECK_EQ(nodeId, 0u);

    int64_t headPtr = vfs_rd8(root_slot, DIRNODE_OFF_HEADPTR);
    CHECK_EQ(headPtr, 0);

    /* nextNodeId should still be 1 (no new nodes created) */
    CHECK_EQ((int)ctx->nextNodeId, 1);

    /* currentEpoch should be 0 */
    CHECK_EQ(ctx->currentEpoch, 0);

    /* epochMapperPtr should be 0 (no mappings) */
    CHECK_EQ(ctx->epochMapperPtr, 0);

    /* touchedFilesPtr should be 0 */
    CHECK_EQ(ctx->touchedFilesPtr, 0);

    /* treeLockState should be 0 */
    CHECK_EQ(ctx->treeLockState, 0);

    vfs_close(vfs);
}

static void test_pool_list_head(void) {
    /* poolListHead should be non-zero after bootstrap (pool alloc happened) */
    vfs_t* vfs = vfs_open(test_path);
    CHECK(vfs != NULL);

    CHECK(vfs->ctx->pool_list_head_value != 0);

    vfs_close(vfs);
}

int main(void) {
    /* Clean up any leftover file from a previous run */
    unlink(test_path);

    test_bootstrap_root();
    test_bootstrap_reopen();
    test_pool_list_head();

    /* Clean up */
    unlink(test_path);

    printf("test_tree: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
