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

    /* nextNodeId should be 0 (node 0 used for root, next available is 0) */
    CHECK_EQ((int)ctx->nextNodeId, 0);

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

    /* nextNodeId should still be 0 (no new nodes created) */
    CHECK_EQ((int)ctx->nextNodeId, 0);

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

/* ---------------------------------------------------------------------------
 * vfs_create test
 * --------------------------------------------------------------------------- */

static void test_create_file(void) {
    vfs_t* vfs = vfs_open(test_path);
    CHECK(vfs != NULL);
    CHECK(vfs->ctx != NULL);
    TreeContext* ctx = vfs->ctx;

    int64_t root_vp = ctx->rootNodeOffset;
    CHECK(root_vp != 0);

    /* Create a file under root */
    int result = vfs_create(vfs, root_vp, "test.txt", 0);
    CHECK(result > 0);  /* should return a positive nodeId */

    /* Verify the file exists in root's DirContent chain */
    uint8_t* root_slot = pool_resolve(&ctx->pool, root_vp);
    CHECK(root_slot != NULL);
    int64_t headPtr = vfs_rd8(root_slot, DIRNODE_OFF_HEADPTR);
    CHECK(headPtr != 0);  /* should have 1 entry now */

    /* Walk the chain to find our file */
    int64_t walk_vp = headPtr;
    int found = 0;
    while (walk_vp != 0 && !found) {
        uint8_t* dc_slot = pool_resolve(&ctx->pool, walk_vp);
        CHECK(dc_slot != NULL);

        uint32_t ce_child, ce_epoch;
        int64_t ce_childPtr, ce_namePtr, ce_next;
        nodes_read_dircontent(dc_slot, &ce_child, &ce_epoch, &ce_childPtr,
                              &ce_namePtr, &ce_next);
        (void)ce_childPtr;
        if (ce_epoch == 0 && ce_namePtr != 0) {
            char entry_name[256];
            int name_len = nodes_read_name(&ctx->pool, ce_namePtr,
                                            entry_name, (int)sizeof(entry_name));
            if (name_len > 0 && strcmp(entry_name, "test.txt") == 0) {
                found = 1;
                CHECK_EQ((int)ce_child, 1);  /* first created file gets nodeId=1 */
            }
        }
        walk_vp = ce_next;
    }
    CHECK(found);

    /* nextNodeId should now be 1 */
    CHECK_EQ((int)ctx->nextNodeId, 1);

    vfs_close(vfs);
}

/* ---------------------------------------------------------------------------
 * vfs_delete test
 * --------------------------------------------------------------------------- */

static void test_delete_file(void) {
    vfs_t* vfs = vfs_open(test_path);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    /* Create a file first */
    int nodeId = vfs_create(vfs, root_vp, "delete_me.txt", 0);
    CHECK(nodeId > 0);

    /* Verify it exists in root's DirContent chain */
    uint8_t* root_slot = pool_resolve(&ctx->pool, root_vp);
    CHECK(root_slot != NULL);
    int64_t headPtr = vfs_rd8(root_slot, DIRNODE_OFF_HEADPTR);
    CHECK(headPtr != 0);

    int64_t walk_vp = headPtr;
    int found = 0;
    while (walk_vp != 0 && !found) {
        uint8_t* dc_slot = pool_resolve(&ctx->pool, walk_vp);
        CHECK(dc_slot != NULL);
        uint32_t ce_child, ce_epoch;
        int64_t ce_childPtr, ce_namePtr, ce_next;
        nodes_read_dircontent(dc_slot, &ce_child, &ce_epoch, &ce_childPtr,
                              &ce_namePtr, &ce_next);
        (void)ce_child; (void)ce_childPtr;
        if (ce_epoch == 0 && ce_namePtr != 0) {
            char entry_name[256];
            int nl = nodes_read_name(&ctx->pool, ce_namePtr,
                                      entry_name, (int)sizeof(entry_name));
            if (nl > 0 && strcmp(entry_name, "delete_me.txt") == 0)
                found = 1;
        }
        walk_vp = ce_next;
    }
    CHECK(found);

    /* Delete the file at epoch 2 */
    int ret = vfs_delete(vfs, root_vp, "delete_me.txt", 2);
    CHECK_EQ(ret, VFS_OK);

    /* Verify the tombstone exists */
    root_slot = pool_resolve(&ctx->pool, root_vp);
    CHECK(root_slot != NULL);
    headPtr = vfs_rd8(root_slot, DIRNODE_OFF_HEADPTR);
    CHECK(headPtr != 0);

    walk_vp = headPtr;
    int found_tombstone = 0;
    while (walk_vp != 0) {
        uint8_t* dc_slot = pool_resolve(&ctx->pool, walk_vp);
        CHECK(dc_slot != NULL);
        uint32_t ce_child, ce_epoch;
        int64_t ce_childPtr, ce_namePtr, ce_next;
        nodes_read_dircontent(dc_slot, &ce_child, &ce_epoch, &ce_childPtr,
                              &ce_namePtr, &ce_next);
        (void)ce_child; (void)ce_childPtr;
        if (ce_epoch == 2 && ce_namePtr == 0)
            found_tombstone = 1;
        walk_vp = ce_next;
    }
    CHECK(found_tombstone);

    /* Delete non-existent file → VFS_ERR_NOTFOUND */
    ret = vfs_delete(vfs, root_vp, "nonexistent.txt", 2);
    CHECK_EQ(ret, VFS_ERR_NOTFOUND);

    vfs_close(vfs);
}

/* ---------------------------------------------------------------------------
 * vfs_open_file test
 * --------------------------------------------------------------------------- */

static void test_open_file(void) {
    vfs_t* vfs = vfs_open(test_path);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    /* Create a file */
    int nodeId = vfs_create(vfs, root_vp, "found.txt", 0);
    CHECK(nodeId > 0);

    /* Open it by name */
    int64_t opened = vfs_open_file(vfs, root_vp, "found.txt", 0);
    CHECK_EQ(opened, (int64_t)nodeId);

    /* Open non-existent → VFS_ERR_NOTFOUND */
    opened = vfs_open_file(vfs, root_vp, "missing.txt", 0);
    CHECK_EQ(opened, VFS_ERR_NOTFOUND);

    /* Open from a file VirtualPtr → VFS_ERR_NOTDIR */
    {
        uint8_t* root_slot2 = pool_resolve(&ctx->pool, root_vp);
        CHECK(root_slot2 != NULL);
        int64_t head2 = vfs_rd8(root_slot2, DIRNODE_OFF_HEADPTR);
        CHECK(head2 != 0);
        uint32_t ce_c, ce_e;
        int64_t ce_cp, ce_np, ce_nx;
        nodes_read_dircontent(pool_resolve(&ctx->pool, head2),
                              &ce_c, &ce_e, &ce_cp, &ce_np, &ce_nx);
        (void)ce_c; (void)ce_e; (void)ce_np; (void)ce_nx;
        opened = vfs_open_file(vfs, ce_cp, "anything.txt", 0);
        CHECK_EQ(opened, VFS_ERR_NOTDIR);
    }

    vfs_close(vfs);
}

/* ---------------------------------------------------------------------------
 * Duplicate name test
 * --------------------------------------------------------------------------- */

static void test_create_duplicate(void) {
    vfs_t* vfs = vfs_open(test_path);
    CHECK(vfs != NULL);
    int64_t root_vp = vfs->ctx->rootNodeOffset;

    /* First create succeeds */
    int r1 = vfs_create(vfs, root_vp, "dup.txt", 0);
    CHECK(r1 > 0);

    /* Second create with same name at same epoch → VFS_ERR_EXISTS */
    int r2 = vfs_create(vfs, root_vp, "dup.txt", 0);
    CHECK_EQ(r2, VFS_ERR_EXISTS);

    vfs_close(vfs);
}

/* ---------------------------------------------------------------------------
 * Epoch isolation test: delete at epoch 2, verify epoch 0 still sees file
 * --------------------------------------------------------------------------- */

static void test_delete_epoch_isolation(void) {
    vfs_t* vfs = vfs_open(test_path);
    CHECK(vfs != NULL);
    int64_t root_vp = vfs->ctx->rootNodeOffset;

    /* Create file at epoch 0 */
    int nodeId = vfs_create(vfs, root_vp, "epoch_test.txt", 0);
    CHECK(nodeId > 0);

    /* Verify it's visible at epoch 0 */
    int64_t found = vfs_open_file(vfs, root_vp, "epoch_test.txt", 0);
    CHECK_EQ(found, (int64_t)nodeId);

    /* Delete at epoch 2 */
    int ret = vfs_delete(vfs, root_vp, "epoch_test.txt", 2);
    CHECK_EQ(ret, VFS_OK);

    /* Verify it's NOT visible at epoch 2 */
    found = vfs_open_file(vfs, root_vp, "epoch_test.txt", 2);
    CHECK_EQ(found, VFS_ERR_NOTFOUND);

    /* Verify it IS still visible at epoch 0 (older epoch unaffected) */
    found = vfs_open_file(vfs, root_vp, "epoch_test.txt", 0);
    CHECK_EQ(found, (int64_t)nodeId);

    vfs_close(vfs);
}

/* ---------------------------------------------------------------------------
 * File stat tests
 * --------------------------------------------------------------------------- */

static void test_file_stat(void) {
    vfs_t* vfs = vfs_open(test_path);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    /* Create a file and get its VirtualPtr from the DirContent chain */
    int nodeId = vfs_create(vfs, root_vp, "stat.txt", 0);
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
        file_vp = cp;  /* VirtualPtr to the FileNode */
    }
    CHECK(file_vp != 0);

    /* New file: size=0, ctime set, mtime=0 (no FileSize chain yet) */
    int64_t size = vfs_file_size(vfs, file_vp, 0);
    CHECK_EQ(size, 0);

    int64_t ctime = vfs_file_ctime(vfs, file_vp);
    CHECK(ctime > 0);  /* should be a valid timestamp */

    int64_t mtime = vfs_file_mtime(vfs, file_vp, 0);
    CHECK_EQ(mtime, 0);  /* empty chain */

    /* Open file on non-file VirtualPtr (root is DirNode) → -1 */
    size = vfs_file_size(vfs, root_vp, 0);
    CHECK_EQ(size, -1);

    vfs_close(vfs);
}

/* ---------------------------------------------------------------------------
 * File stat on non-directory test
 * --------------------------------------------------------------------------- */

static void test_stat_not_file(void) {
    vfs_t* vfs = vfs_open(test_path);
    CHECK(vfs != NULL);
    int64_t root_vp = vfs->ctx->rootNodeOffset;

    /* Root is a DirNode, not a FileNode → all stat functions return -1 */
    int64_t size = vfs_file_size(vfs, root_vp, 0);
    CHECK_EQ(size, -1);

    int64_t mtime = vfs_file_mtime(vfs, root_vp, 0);
    CHECK_EQ(mtime, -1);

    int64_t ctime = vfs_file_ctime(vfs, root_vp);
    CHECK_EQ(ctime, -1);

    vfs_close(vfs);
}

/* ---------------------------------------------------------------------------
 * Helper: get file VirtualPtr from parent's first DirContent entry
 * --------------------------------------------------------------------------- */

static int64_t get_file_vp(Pool* pool, int64_t root_vp) {
    uint8_t* rs = pool_resolve(pool, root_vp);
    if (!rs) return 0;
    int64_t head = vfs_rd8(rs, DIRNODE_OFF_HEADPTR);
    if (head == 0) return 0;
    uint32_t cc, ce;
    int64_t cp, np, nx;
    nodes_read_dircontent(pool_resolve(pool, head),
                          &cc, &ce, &cp, &np, &nx);
    (void)cc; (void)ce; (void)np; (void)nx;
    return cp;
}

/* ---------------------------------------------------------------------------
 * File size with epoch isolation test
 *
 * Directly writes a FileSize entry to simulate a file write.
 * Verifies: new file size=0, after write size matches,
 * old epoch returns old size, ctime unchanged.
 * --------------------------------------------------------------------------- */

static void test_file_size_epoch(void) {
    vfs_t* vfs = vfs_open(test_path);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int nodeId = vfs_create(vfs, root_vp, "sizetest.txt", 0);
    CHECK(nodeId > 0);
    int64_t file_vp = get_file_vp(&ctx->pool, root_vp);
    CHECK(file_vp != 0);

    int64_t ctime_before = vfs_file_ctime(vfs, file_vp);
    CHECK(ctime_before > 0);

    /* New file: size=0 */
    CHECK_EQ(vfs_file_size(vfs, file_vp, 0), 0);

    /* Directly write a FileSize entry at epoch 2 (simulating a write) */
    int64_t fs_vp = pool_alloc(&ctx->pool);
    CHECK(fs_vp != VFS_VPTR_NULL);
    uint8_t* fs_slot = pool_resolve(&ctx->pool, fs_vp);
    CHECK(fs_slot != NULL);

    uint8_t* file_slot = pool_resolve(&ctx->pool, file_vp);
    CHECK(file_slot != NULL);
    int64_t old_sizePtr = vfs_rd8(file_slot, FILENODE_OFF_SIZEPTR);

    nodes_write_filesize(fs_slot, 2, 2000, 500, old_sizePtr);
    vfs_mb_release();
    int64_t cas_result = vfs_cas_i64(
        (int64_t*)(file_slot + FILENODE_OFF_SIZEPTR),
        old_sizePtr, fs_vp);
    CHECK_EQ(cas_result, old_sizePtr);  /* CAS succeeded */

    /* At epoch 2: size=500, mtime=2000 */
    CHECK_EQ(vfs_file_size(vfs, file_vp, 2), 500);
    CHECK_EQ(vfs_file_mtime(vfs, file_vp, 2), 2000);

    /* At epoch 0: still size=0 (old epoch unaffected) */
    CHECK_EQ(vfs_file_size(vfs, file_vp, 0), 0);
    CHECK_EQ(vfs_file_mtime(vfs, file_vp, 0), 0);

    /* ctime unchanged across epochs */
    int64_t ctime_after = vfs_file_ctime(vfs, file_vp);
    CHECK_EQ(ctime_after, ctime_before);

    vfs_close(vfs);
}

int main(void) {
    /* Clean up any leftover file from a previous run */
    unlink(test_path);

    test_bootstrap_root();
    test_bootstrap_reopen();
    test_pool_list_head();
    test_create_file();
    test_delete_file();
    test_open_file();
    test_create_duplicate();
    test_delete_epoch_isolation();
    test_file_stat();
    test_stat_not_file();
    test_file_size_epoch();

    /* Clean up */
    unlink(test_path);

    printf("test_tree: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
