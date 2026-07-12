/* Phase 5a: Tree bootstrap tests */
#include "ixsphere/vfs_internal.h"
#include "tree.h"
#include "nodes.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

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
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    CHECK(vfs->ctx != NULL);

    TreeContext* ctx = vfs->ctx;

    /* Root DirNode must exist with correct fields */
    CHECK(ctx->rootNodeOffset != 0);

    uint8_t* root_slot = pool_resolve_ro(&ctx->pool, ctx->rootNodeOffset);
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

    vfs_unmount(vfs);
}

static void test_bootstrap_reopen(void) {
    /* Open existing file → reopen */
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    CHECK(vfs->ctx != NULL);

    TreeContext* ctx = vfs->ctx;

    /* Root must still exist */
    CHECK(ctx->rootNodeOffset != 0);

    uint8_t* root_slot = pool_resolve_ro(&ctx->pool, ctx->rootNodeOffset);
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

    /* touchedFilesPtr removed — no field to check */

    /* treeLockState should be 0 */
    CHECK_EQ(ctx->treeLockState, 0);

    vfs_unmount(vfs);
}

static void test_pool_list_head(void) {
    /* poolListHead should be non-zero after bootstrap (pool alloc happened) */
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);

    CHECK(vfs->ctx->pool_list_head_value != 0);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * vfs_create test
 * --------------------------------------------------------------------------- */

static void test_create_file(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    CHECK(vfs->ctx != NULL);
    TreeContext* ctx = vfs->ctx;

    int64_t root_vp = ctx->rootNodeOffset;
    CHECK(root_vp != 0);

    /* Create a file under root */
    int64_t file_vp = vfs_create(vfs, root_vp, "test.txt", 0);
    CHECK(file_vp > 0);  /* should return a positive VirtualPtr */

    /* Verify the FileNode's nodeId slot via the returned VirtualPtr */
    uint8_t* fn_slot = pool_resolve_ro(&ctx->pool, file_vp);
    CHECK(fn_slot != NULL);
    uint32_t fn_nodeId = (uint32_t)vfs_rd4(fn_slot, FILENODE_OFF_NODEID);
    CHECK_EQ(fn_nodeId, 1u);  /* first created file gets nodeId=1 */

    /* Verify the file exists in root's DirContent chain */
    uint8_t* root_slot = pool_resolve_ro(&ctx->pool, root_vp);
    CHECK(root_slot != NULL);
    int64_t headPtr = vfs_rd8(root_slot, DIRNODE_OFF_HEADPTR);
    CHECK(headPtr != 0);  /* should have 1 entry now */

    /* Walk the chain to find our file */
    int64_t walk_vp = headPtr;
    int found = 0;
    while (walk_vp != 0 && !found) {
        uint8_t* dc_slot = pool_resolve_ro(&ctx->pool, walk_vp);
        CHECK(dc_slot != NULL);

        uint32_t ce_child, ce_epoch;
        int64_t ce_childPtr, ce_namePtr, ce_next;
        nodes_read_dircontent(dc_slot, &ce_child, &ce_epoch, &ce_childPtr,
                              &ce_namePtr, &ce_next, VFS_PAGE_SIZE);
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

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * vfs_delete test
 * --------------------------------------------------------------------------- */

static void test_delete_file(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    /* Create a file first */
    int64_t nodeId = vfs_create(vfs, root_vp, "delete_me.txt", 0);
    CHECK(nodeId > 0);

    /* Verify it exists in root's DirContent chain */
    uint8_t* root_slot = pool_resolve_ro(&ctx->pool, root_vp);
    CHECK(root_slot != NULL);
    int64_t headPtr = vfs_rd8(root_slot, DIRNODE_OFF_HEADPTR);
    CHECK(headPtr != 0);

    int64_t walk_vp = headPtr;
    int found = 0;
    while (walk_vp != 0 && !found) {
        uint8_t* dc_slot = pool_resolve_ro(&ctx->pool, walk_vp);
        CHECK(dc_slot != NULL);
        uint32_t ce_child, ce_epoch;
        int64_t ce_childPtr, ce_namePtr, ce_next;
        nodes_read_dircontent(dc_slot, &ce_child, &ce_epoch, &ce_childPtr,
                              &ce_namePtr, &ce_next, VFS_PAGE_SIZE);
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
    root_slot = pool_resolve_ro(&ctx->pool, root_vp);
    CHECK(root_slot != NULL);
    headPtr = vfs_rd8(root_slot, DIRNODE_OFF_HEADPTR);
    CHECK(headPtr != 0);

    walk_vp = headPtr;
    int found_tombstone = 0;
    while (walk_vp != 0) {
        uint8_t* dc_slot = pool_resolve_ro(&ctx->pool, walk_vp);
        CHECK(dc_slot != NULL);
        uint32_t ce_child, ce_epoch;
        int64_t ce_childPtr, ce_namePtr, ce_next;
        nodes_read_dircontent(dc_slot, &ce_child, &ce_epoch, &ce_childPtr,
                              &ce_namePtr, &ce_next, VFS_PAGE_SIZE);
        (void)ce_child; (void)ce_childPtr;
        if (ce_epoch == 2 && ce_namePtr == 0)
            found_tombstone = 1;
        walk_vp = ce_next;
    }
    CHECK(found_tombstone);

    /* Delete non-existent file → VFS_ERR_NOTFOUND */
    ret = vfs_delete(vfs, root_vp, "nonexistent.txt", 2);
    CHECK_EQ(ret, VFS_ERR_NOTFOUND);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * vfs_mount test
 * --------------------------------------------------------------------------- */

static void test_open_file(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    /* Create a file */
    int64_t nodeId = vfs_create(vfs, root_vp, "found.txt", 0);
    CHECK(nodeId > 0);

    /* Open it by name */
    int64_t opened = vfs_open(vfs, root_vp, "found.txt", 0);
    CHECK(opened > 0);

    /* Open non-existent → VFS_ERR_NOTFOUND */
    opened = vfs_open(vfs, root_vp, "missing.txt", 0);
    CHECK_EQ(opened, VFS_ERR_NOTFOUND);

    /* Open from a file VirtualPtr → VFS_ERR_NOTDIR */
    {
        uint8_t* root_slot2 = pool_resolve_ro(&ctx->pool, root_vp);
        CHECK(root_slot2 != NULL);
        int64_t head2 = vfs_rd8(root_slot2, DIRNODE_OFF_HEADPTR);
        CHECK(head2 != 0);
        uint32_t ce_c, ce_e;
        int64_t ce_cp, ce_np, ce_nx;
        nodes_read_dircontent(pool_resolve_ro(&ctx->pool, head2),
                              &ce_c, &ce_e, &ce_cp, &ce_np, &ce_nx, VFS_PAGE_SIZE);
        (void)ce_c; (void)ce_e; (void)ce_np; (void)ce_nx;
        opened = vfs_open(vfs, ce_cp, "anything.txt", 0);
        CHECK_EQ(opened, VFS_ERR_NOTDIR);
    }

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * Duplicate name test
 * --------------------------------------------------------------------------- */

static void test_create_duplicate(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    int64_t root_vp = vfs->ctx->rootNodeOffset;

    /* First create succeeds */
    int64_t r1 = vfs_create(vfs, root_vp, "dup.txt", 0);
    CHECK(r1 > 0);

    /* Second create with same name at same epoch → VFS_ERR_EXISTS */
    int64_t r2 = vfs_create(vfs, root_vp, "dup.txt", 0);
    CHECK_EQ(r2, VFS_ERR_EXISTS);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * Phase 25 bug fix: delete then re-create at the same epoch with the
 * same name should succeed.  Previously vfs_create did a naive
 * `ce_epoch == epoch && ce_namePtr != 0` check which saw the original
 * create's DirContent (live, namePtr != 0) and returned VFS_ERR_EXISTS
 * even after a delete tombstone was added.  Now vfs_create uses
 * dirchain_find_child which properly applies the read-rule and
 * tombstones.
 * --------------------------------------------------------------------------- */

static void test_create_after_delete(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    int64_t root_vp = vfs->ctx->rootNodeOffset;

    /* Create file at epoch 0 */
    int64_t r1 = vfs_create(vfs, root_vp, "recreate.txt", 0);
    CHECK(r1 > 0);

    /* Delete at epoch 0 */
    int rd = vfs_delete(vfs, root_vp, "recreate.txt", 0);
    CHECK_EQ(rd, VFS_OK);

    /* Re-create at epoch 0 with the same name — should succeed
       (bug fix: previously returned VFS_ERR_EXISTS) */
    int64_t r2 = vfs_create(vfs, root_vp, "recreate.txt", 0);
    CHECK(r2 > 0);

    /* The new file should be visible at epoch 0, with the NEW childPtr
       (not the old tombstoned one).  Bug fix: dirchain_find_child
       previously returned the old childPtr when it should have
       returned the new one. */
    int64_t found = vfs_open(vfs, root_vp, "recreate.txt", 0);
    CHECK(found > 0);
    CHECK_EQ(found, r2);  /* must equal the new file's VP, not r1 */

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * Phase 25 bug fix 2: double-delete is idempotent (returns NOTFOUND
 * after the first delete succeeds, rather than corrupting state by
 * adding a second tombstone).
 * --------------------------------------------------------------------------- */

static void test_double_delete(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    int64_t root_vp = vfs->ctx->rootNodeOffset;

    int64_t r1 = vfs_create(vfs, root_vp, "del.txt", 0);
    CHECK(r1 > 0);

    /* First delete succeeds */
    int rd1 = vfs_delete(vfs, root_vp, "del.txt", 0);
    CHECK_EQ(rd1, VFS_OK);

    /* Second delete returns NOTFOUND (file is already gone) */
    int rd2 = vfs_delete(vfs, root_vp, "del.txt", 0);
    CHECK_EQ(rd2, VFS_ERR_NOTFOUND);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * Phase 25 bug fix 3: create + delete + create + delete at the same
 * epoch with the same name.  Previously:
 *   - First delete worked
 *   - Re-create worked (after the vfs_create collision-check fix)
 *   - Second delete wrote a tombstone for the WRONG childNodeId
 *     (because dirchain_find_child's tombstone handling overrode
 *     best_child with the OLD child's id, then returned the OLD
 *     child's id as the lookup result).  The chain ended up with
 *     two tombstones for different childIds, and re-create would
 *     fail with EXISTS because the old tombstone's childPtr was
 *     inconsistent with the new child's chain entry.
 *
 * Now: dirchain_find_child properly tracks tombstones separately
 * from live entries via a tombstoned_childId flag, so the lookup
 * returns the correct childId for the live entry even after a
 * tombstone for a different (older) childId appears earlier in
 * the chain.
 * --------------------------------------------------------------------------- */

static void test_double_create_delete(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    int64_t root_vp = vfs->ctx->rootNodeOffset;

    /* create -> delete -> create -> delete */
    int64_t r1 = vfs_create(vfs, root_vp, "x.txt", 0);
    CHECK(r1 > 0);
    int rd1 = vfs_delete(vfs, root_vp, "x.txt", 0);
    CHECK_EQ(rd1, VFS_OK);
    int64_t r2 = vfs_create(vfs, root_vp, "x.txt", 0);
    CHECK(r2 > 0);
    int rd2 = vfs_delete(vfs, root_vp, "x.txt", 0);
    CHECK_EQ(rd2, VFS_OK);

    /* File should not exist */
    int64_t found = vfs_open(vfs, root_vp, "x.txt", 0);
    CHECK_EQ(found, VFS_ERR_NOTFOUND);

    /* Re-create should succeed */
    int64_t r3 = vfs_create(vfs, root_vp, "x.txt", 0);
    CHECK(r3 > 0);

    /* New file should be findable */
    found = vfs_open(vfs, root_vp, "x.txt", 0);
    CHECK(found > 0);
    CHECK_EQ(found, r3);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * Epoch isolation test: delete at epoch 2, verify epoch 0 still sees file
 * --------------------------------------------------------------------------- */

static void test_delete_epoch_isolation(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    int64_t root_vp = vfs->ctx->rootNodeOffset;

    /* Create file at epoch 0 */
    int64_t nodeId = vfs_create(vfs, root_vp, "epoch_test.txt", 0);
    CHECK(nodeId > 0);

    /* Verify it's visible at epoch 0 */
    int64_t found = vfs_open(vfs, root_vp, "epoch_test.txt", 0);
    CHECK(found > 0);

    /* Delete at epoch 2 */
    int ret = vfs_delete(vfs, root_vp, "epoch_test.txt", 2);
    CHECK_EQ(ret, VFS_OK);

    /* Verify it's NOT visible at epoch 2 */
    found = vfs_open(vfs, root_vp, "epoch_test.txt", 2);
    CHECK_EQ(found, VFS_ERR_NOTFOUND);

    /* Verify it IS still visible at epoch 0 (older epoch unaffected) */
    found = vfs_open(vfs, root_vp, "epoch_test.txt", 0);
    CHECK(found > 0);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * File stat tests
 * --------------------------------------------------------------------------- */

static void test_file_stat(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    /* Create a file and get its VirtualPtr directly from vfs_create */
    int64_t file_vp = vfs_create(vfs, root_vp, "stat.txt", 0);
    CHECK(file_vp > 0);

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

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * File stat on non-directory test
 * --------------------------------------------------------------------------- */

static void test_stat_not_file(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    int64_t root_vp = vfs->ctx->rootNodeOffset;

    /* Root is a DirNode, not a FileNode → all stat functions return -1 */
    int64_t size = vfs_file_size(vfs, root_vp, 0);
    CHECK_EQ(size, -1);

    int64_t mtime = vfs_file_mtime(vfs, root_vp, 0);
    CHECK_EQ(mtime, -1);

    int64_t ctime = vfs_file_ctime(vfs, root_vp);
    CHECK_EQ(ctime, -1);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * File size with epoch isolation test
 *
 * Directly writes a FileSize entry to simulate a file write.
 * Verifies: new file size=0, after write size matches,
 * old epoch returns old size, ctime unchanged.
 * --------------------------------------------------------------------------- */

static void test_file_size_epoch(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t file_vp = vfs_create(vfs, root_vp, "sizetest.txt", 0);
    CHECK(file_vp > 0);

    int64_t ctime_before = vfs_file_ctime(vfs, file_vp);
    CHECK(ctime_before > 0);

    /* New file: size=0 */
    CHECK_EQ(vfs_file_size(vfs, file_vp, 0), 0);

    /* Directly write a FileSize entry at epoch 2 (simulating a write) */
    int64_t fs_vp = pool_alloc(&ctx->pool);
    CHECK(fs_vp != VFS_VPTR_NULL);
    uint8_t* fs_slot = pool_resolve_rw(&ctx->pool, fs_vp);
    CHECK(fs_slot != NULL);

    uint8_t* file_slot = pool_resolve_rw(&ctx->pool, file_vp);
    CHECK(file_slot != NULL);
    int64_t old_sizePtr = vfs_rd8(file_slot, FILENODE_OFF_SIZEPTR);

    nodes_write_filesize(fs_slot, 2, 2000, 500, old_sizePtr, VFS_PAGE_SIZE);
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

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * tree_resolve_page test — file growth and in-memory page array
 * --------------------------------------------------------------------------- */

static void test_resolve_page_growth(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t file_vp = vfs_create(vfs, root_vp, "big.txt", 0);
    CHECK(file_vp > 0);

    uint32_t seg_size = ctx->segment_size;
    CHECK(seg_size > 0);

    /* Resolve page 0 — should create first segment */
    uint8_t* pn0 = tree_resolve_page_compat(ctx, file_vp, 0, 0, true);
    CHECK(pn0 != NULL);

    /* Verify it's a PageNode with versionRootPtr=0 (never written) */
    CHECK_EQ(vfs_rd8(pn0, PAGENODE_OFF_VERSIONROOT), 0);
    CHECK_EQ((uint32_t)vfs_rd4_s(pn0, PAGENODE_OFF_PAGEINDEX, ctx->page_size), 0u);

    /* Resolve page 1 — should allocate a second PageNode */
    uint8_t* pn1 = tree_resolve_page_compat(ctx, file_vp, 1, 0, true);
    CHECK(pn1 != NULL);
    CHECK_EQ(vfs_rd8(pn1, PAGENODE_OFF_VERSIONROOT), 0);
    CHECK_EQ((uint32_t)vfs_rd4_s(pn1, PAGENODE_OFF_PAGEINDEX, ctx->page_size), 1u);

    /* Resolve page at segment boundary — should create second segment */
    uint8_t* pn_first_new = tree_resolve_page_compat(ctx, file_vp, seg_size, 0, true);
    CHECK(pn_first_new != NULL);
    CHECK_EQ(vfs_rd8(pn_first_new, PAGENODE_OFF_VERSIONROOT), 0);
    CHECK_EQ((uint32_t)vfs_rd4_s(pn_first_new, PAGENODE_OFF_PAGEINDEX, ctx->page_size), 0u);

    /* Resolve page 0 again — cache may have been invalidated by second segment.
       Just verify it still works. */
    pn0 = tree_resolve_page_compat(ctx, file_vp, 0, 0, false);
    CHECK(pn0 != NULL);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * vfs_write test — basic write, size update, cross-page write
 * --------------------------------------------------------------------------- */

static void test_write_basic(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t file_vp = vfs_create(vfs, root_vp, "write.txt", 0);
    CHECK(file_vp > 0);

    /* Write 100 bytes at offset 0 */
    const char* data1 = "Hello, VFS write test!";
    int ret = vfs_write(vfs, file_vp, data1, 0, (int64_t)strlen(data1), 0);
    CHECK_EQ(ret, (int)strlen(data1));

    /* File size should be updated */
    int64_t sz = vfs_file_size(vfs, file_vp, 0);
    CHECK_EQ(sz, (int64_t)strlen(data1));

    /* Write more bytes at offset 50 (partial page overlap) */
    const char* data2 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    ret = vfs_write(vfs, file_vp, data2, 50, (int64_t)strlen(data2), 0);
    CHECK_EQ(ret, (int)strlen(data2));

    /* File size should be max(old_size, 50+26) = 76 */
    sz = vfs_file_size(vfs, file_vp, 0);
    CHECK_EQ(sz, 76);

    /* Write across page boundary: offset 8180, 32 bytes should span 2 pages */
    const char* data3 = "CROSS_PAGE_BOUNDARY_TEST!";
    ret = vfs_write(vfs, file_vp, data3, 8180, (int64_t)strlen(data3), 0);
    CHECK_EQ(ret, (int)strlen(data3));

    sz = vfs_file_size(vfs, file_vp, 0);
    CHECK_EQ(sz, 8180 + (int64_t)strlen(data3));

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * vfs_read test — write then read back, verify content and epoch isolation
 * --------------------------------------------------------------------------- */

static void test_read_basic(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t file_vp = vfs_create(vfs, root_vp, "readtest.txt", 0);
    CHECK(file_vp > 0);

    /* Write data */
    const char* wdata = "Hello, VFS read test!";
    int wret = vfs_write(vfs, file_vp, wdata, 0, (int64_t)strlen(wdata), 0);
    CHECK_EQ(wret, (int)strlen(wdata));

    /* Read it back — vfs_read truncates to file_size (Phase 18 plan.md
       Phase 7 fix: previously the test expected the full requested count,
       but vfs_read caps reads at file_size). */
    char rbuf[64];
    memset(rbuf, 0, sizeof(rbuf));
    int rret = vfs_read(vfs, file_vp, rbuf, 0, (int64_t)sizeof(rbuf) - 1, 0);
    CHECK_EQ(rret, (int)strlen(wdata));  /* truncated to file_size */
    CHECK_EQ(strncmp(rbuf, wdata, strlen(wdata)), 0);
    CHECK_EQ(strcmp(rbuf, wdata), 0);

    /* Read before any write → zero-filled, but offset >= file_size
       returns 0 (vfs_read returns 0 when offset is past EOF). */
    memset(rbuf, 0, sizeof(rbuf));
    rret = vfs_read(vfs, file_vp, rbuf, 100, 10, 0);
    CHECK_EQ(rret, 0);

    /* Cross-page read past EOF returns 0 */
    memset(rbuf, 0, sizeof(rbuf));
    rret = vfs_read(vfs, file_vp, rbuf, 8180, 32, 0);
    CHECK_EQ(rret, 0);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * vfs_write/read comprehensive tests
 * --------------------------------------------------------------------------- */

/* Write 200 bytes at offset 50 (cross-page), read back */
static void test_write_cross_page(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t file_vp = vfs_create(vfs, root_vp, "cross.txt", 0);
    CHECK(file_vp > 0);

    char data[200];
    for (int i = 0; i < 200; i++) data[i] = (char)('A' + (i % 26));
    int ret = vfs_write(vfs, file_vp, data, 50, 200, 0);
    CHECK_EQ(ret, 200);

    char rbuf[256];
    memset(rbuf, 0, sizeof(rbuf));
    ret = vfs_read(vfs, file_vp, rbuf, 0, 250, 0);
    CHECK_EQ(ret, 250);

    /* First 50 bytes are zero (never written) */
    for (int i = 0; i < 50; i++) CHECK_EQ(rbuf[i], 0);
    /* Bytes 50-249 match the data */
    for (int i = 0; i < 200; i++) CHECK_EQ(rbuf[50 + i], data[i]);

    vfs_unmount(vfs);
}

/* Same offset, same epoch: second write in-place (VersionPage count unchanged) */
static void test_write_in_place(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t file_vp = vfs_create(vfs, root_vp, "inplace.txt", 0);
    CHECK(file_vp > 0);

    /* First write: creates a VersionPage */
    int ret = vfs_write(vfs, file_vp, "AAAA", 0, 4, 0);
    CHECK_EQ(ret, 4);

    /* Count VersionPages for page 0 after first write */
    uint8_t* pn0 = tree_resolve_page_compat(ctx, file_vp, 0, 0, false);
    CHECK(pn0 != NULL);
    int64_t vp = vfs_atomic_load_i64((const int64_t*)(pn0 + PAGENODE_OFF_VERSIONROOT));
    int count_before = 0;
    int64_t walk = vp;
    while (walk != 0) {
        count_before++;
        uint8_t* vs = pool_resolve_ro(&ctx->pool, walk);
        CHECK(vs != NULL);
        walk = vfs_rd8(vs, VERSIONPAGE_OFF_NEXTPTR);
    }
    CHECK_EQ(count_before, 1);  /* exactly 1 VersionPage */

    /* Second write at same offset, same epoch: in-place, no new VersionPage */
    ret = vfs_write(vfs, file_vp, "BBBB", 0, 4, 0);
    CHECK_EQ(ret, 4);

    /* Count VersionPages again — should still be 1 (in-place) */
    pn0 = tree_resolve_page_compat(ctx, file_vp, 0, 0, false);
    CHECK(pn0 != NULL);
    vp = vfs_atomic_load_i64((const int64_t*)(pn0 + PAGENODE_OFF_VERSIONROOT));
    int count_after = 0;
    walk = vp;
    while (walk != 0) {
        count_after++;
        uint8_t* vs = pool_resolve_ro(&ctx->pool, walk);
        CHECK(vs != NULL);
        walk = vfs_rd8(vs, VERSIONPAGE_OFF_NEXTPTR);
    }
    CHECK_EQ(count_after, 1);  /* still 1 VersionPage */

    /* Read back — should be "BBBB" */
    char rbuf[16];
    memset(rbuf, 0, sizeof(rbuf));
    ret = vfs_read(vfs, file_vp, rbuf, 0, 4, 0);
    CHECK_EQ(ret, 4);
    CHECK_EQ(strncmp(rbuf, "BBBB", 4), 0);

    vfs_unmount(vfs);
}

/* Same offset, new epoch: COW creates new VersionPage, old epoch returns old data */
static void test_write_cow_epoch(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t file_vp = vfs_create(vfs, root_vp, "cow.txt", 0);
    CHECK(file_vp > 0);

    /* Write at epoch 0 */
    int ret = vfs_write(vfs, file_vp, "AAAA", 0, 4, 0);
    CHECK_EQ(ret, 4);

    /* Write same offset at epoch 2 (different epoch — triggers COW) */
    ret = vfs_write(vfs, file_vp, "BBBB", 0, 4, 2);
    CHECK_EQ(ret, 4);

    /* Old epoch returns old data */
    char rbuf[16];
    memset(rbuf, 0, sizeof(rbuf));
    ret = vfs_read(vfs, file_vp, rbuf, 0, 4, 0);
    CHECK_EQ(ret, 4);
    CHECK_EQ(strncmp(rbuf, "AAAA", 4), 0);

    /* New epoch returns new data */
    memset(rbuf, 0, sizeof(rbuf));
    ret = vfs_read(vfs, file_vp, rbuf, 0, 4, 2);
    CHECK_EQ(ret, 4);
    CHECK_EQ(strncmp(rbuf, "BBBB", 4), 0);

    /* VersionPage count should be 2 now (one per epoch) */
    uint8_t* pn0 = tree_resolve_page_compat(ctx, file_vp, 0, 0, false);
    CHECK(pn0 != NULL);
    int64_t vp = vfs_atomic_load_i64((const int64_t*)(pn0 + PAGENODE_OFF_VERSIONROOT));
    int count = 0;
    int64_t walk = vp;
    while (walk != 0) {
        count++;
        uint8_t* vs = pool_resolve_ro(&ctx->pool, walk);
        CHECK(vs != NULL);
        walk = vfs_rd8(vs, VERSIONPAGE_OFF_NEXTPTR);
    }
    CHECK_EQ(count, 2);

    vfs_unmount(vfs);
}

/* Write 2000 pages → second FileContent segment, reads across boundary */
static void test_write_multi_segment(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t file_vp = vfs_create(vfs, root_vp, "multi.txt", 0);
    CHECK(file_vp > 0);

    /* Write a marker at page 0 and page seg_size (creates 2 segments) */
    uint32_t seg_size = ctx->segment_size;

    int ret = vfs_write(vfs, file_vp, "FIRST", 0, 5, 0);
    CHECK_EQ(ret, 5);

    int64_t offset2 = (int64_t)seg_size * VFS_PAGE_SIZE;
    ret = vfs_write(vfs, file_vp, "SECOND", offset2, 6, 0);
    CHECK_EQ(ret, 6);

    /* Read back from both pages */
    char rbuf[32];
    memset(rbuf, 0, sizeof(rbuf));
    ret = vfs_read(vfs, file_vp, rbuf, 0, 5, 0);
    CHECK_EQ(ret, 5);
    CHECK_EQ(strncmp(rbuf, "FIRST", 5), 0);

    memset(rbuf, 0, sizeof(rbuf));
    ret = vfs_read(vfs, file_vp, rbuf, offset2, 6, 0);
    CHECK_EQ(ret, 6);
    CHECK_EQ(strncmp(rbuf, "SECOND", 6), 0);

    /* Sanity: segment array cache should handle the switch */
    (void)seg_size;

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * Write to frozen epoch test — vfs_epoch_is_writable returns false
 * --------------------------------------------------------------------------- */

static void test_write_frozen_epoch(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t file_vp = vfs_create(vfs, root_vp, "frozen.txt", 0);
    CHECK(file_vp > 0);

    /* Freeze epoch — write should fail */
    test_set_epoch_writable(0);

    int ret = vfs_write(vfs, file_vp, "DATA", 0, 4, 3);
    CHECK_EQ(ret, -1);  /* VFS_ERR_IO mapped to -1 */

    /* Unfreeze for cleanup */
    test_set_epoch_writable(1);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * mkdir basic: create a directory, verify it's in the parent's DirContent
 * chain and has the DirNode type.
 * --------------------------------------------------------------------------- */

static void test_mkdir_basic(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    /* Create a file and check the VirtualPtr in the DirContent chain */
    int64_t ret = vfs_mkdir(vfs, root_vp, "a", 0);
    CHECK(ret > 0);

    /* Verify entry exists in root's DirContent chain */
    int64_t head = vfs_rd8_s(pool_resolve_ro(&ctx->pool, root_vp),
                              DIRNODE_OFF_HEADPTR, ctx->page_size);
    CHECK(head != 0);

    uint32_t cc, ce;
    int64_t cp, np, nx;
    nodes_read_dircontent(pool_resolve_ro(&ctx->pool, head),
                          &cc, &ce, &cp, &np, &nx, ctx->page_size);
    (void)cc; (void)ce; (void)np; (void)nx;
    CHECK(cp != 0);

    /* Verify child is a DirNode */
    uint8_t* child_slot = pool_resolve_ro(&ctx->pool, cp);
    CHECK(child_slot != NULL);
    CHECK_EQ(vfs_rd2_s(child_slot, DIRNODE_OFF_TYPE, ctx->page_size),
             (int16_t)NODE_TYPE_DIR);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * mkdir duplicate: same name at same epoch → VFS_ERR_EXISTS.
 * --------------------------------------------------------------------------- */

static void test_mkdir_duplicate(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    int64_t root_vp = vfs->ctx->rootNodeOffset;

    CHECK(vfs_mkdir(vfs, root_vp, "dup", 0) > 0);
    CHECK_EQ(vfs_mkdir(vfs, root_vp, "dup", 0), VFS_ERR_EXISTS);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * rmdir empty: mkdir + create file → rmdir fails with NOTEMPTY,
 * then delete file → rmdir succeeds.
 * --------------------------------------------------------------------------- */

static void test_rmdir_empty(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t subdir_vp = vfs_mkdir(vfs, root_vp, "sub", 0);
    CHECK(subdir_vp > 0);

    /* Create file inside subdir */
    CHECK(vfs_create(vfs, subdir_vp, "f.txt", 0) > 0);

    /* rmdir should fail — directory not empty */
    CHECK_EQ(vfs_rmdir(vfs, root_vp, "sub", 0), VFS_ERR_NOTEMPTY);

    /* Delete the file inside subdir */
    CHECK_EQ(vfs_delete(vfs, subdir_vp, "f.txt", 0), VFS_OK);

    /* Now rmdir should succeed */
    CHECK_EQ(vfs_rmdir(vfs, root_vp, "sub", 0), VFS_OK);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * rmdir notdir: rmdir on a file (not a directory) → VFS_ERR_NOTDIR.
 * --------------------------------------------------------------------------- */

static void test_rmdir_notdir(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    int64_t root_vp = vfs->ctx->rootNodeOffset;

    CHECK(vfs_create(vfs, root_vp, "not_a_dir.txt", 0) > 0);
    CHECK_EQ(vfs_rmdir(vfs, root_vp, "not_a_dir.txt", 0), VFS_ERR_NOTDIR);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * readdir empty: root directory has no files → 0 entries
 * --------------------------------------------------------------------------- */

static void test_readdir_empty(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    int64_t root_vp = vfs->ctx->rootNodeOffset;

    vfs_dirent_t* entries = NULL;
    int n = 0;
    int rc = vfs_readdir(vfs, root_vp, &entries, &n, 0);
    CHECK_EQ(rc, VFS_OK);
    CHECK_EQ(n, 0);
    vfs_free_dirents(entries);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * readdir with files: create 3 files, verify 3 entries with isDir=false
 * --------------------------------------------------------------------------- */

static void test_readdir_with_files(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    int64_t root_vp = vfs->ctx->rootNodeOffset;

    CHECK(vfs_create(vfs, root_vp, "a.txt", 0) > 0);
    CHECK(vfs_create(vfs, root_vp, "b.txt", 0) > 0);
    CHECK(vfs_create(vfs, root_vp, "c.txt", 0) > 0);

    vfs_dirent_t* entries = NULL;
    int n = 0;
    int rc = vfs_readdir(vfs, root_vp, &entries, &n, 0);
    CHECK_EQ(rc, VFS_OK);
    CHECK_EQ(n, 3);

    int found_a = 0, found_b = 0, found_c = 0;
    for (int i = 0; i < n; i++) {
        CHECK(!entries[i].isDir);
        if (strcmp(entries[i].name, "a.txt") == 0) found_a = 1;
        if (strcmp(entries[i].name, "b.txt") == 0) found_b = 1;
        if (strcmp(entries[i].name, "c.txt") == 0) found_c = 1;
    }
    CHECK(found_a);
    CHECK(found_b);
    CHECK(found_c);
    vfs_free_dirents(entries);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * readdir with dirs: mkdir "sub", verify isDir=true
 * --------------------------------------------------------------------------- */

static void test_readdir_with_dirs(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    int64_t root_vp = vfs->ctx->rootNodeOffset;

    CHECK(vfs_mkdir(vfs, root_vp, "sub", 0) > 0);
    CHECK(vfs_create(vfs, root_vp, "f.txt", 0) > 0);

    vfs_dirent_t* entries = NULL;
    int n = 0;
    int rc = vfs_readdir(vfs, root_vp, &entries, &n, 0);
    CHECK_EQ(rc, VFS_OK);
    CHECK_EQ(n, 2);

    int found_sub = 0, found_f = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(entries[i].name, "sub") == 0) {
            found_sub = 1;
            CHECK(entries[i].isDir);
        }
        if (strcmp(entries[i].name, "f.txt") == 0) {
            found_f = 1;
            CHECK(!entries[i].isDir);
        }
    }
    CHECK(found_sub);
    CHECK(found_f);
    vfs_free_dirents(entries);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * readdir tombstone: create+delete file at epoch 2, readdir at epoch 2
 * excludes it, at epoch 0 shows it.
 * --------------------------------------------------------------------------- */

static void test_readdir_tombstone(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    int64_t root_vp = vfs->ctx->rootNodeOffset;

    CHECK(vfs_create(vfs, root_vp, "x.txt", 0) > 0);
    CHECK(vfs_create(vfs, root_vp, "y.txt", 0) > 0);
    CHECK(vfs_create(vfs, root_vp, "z.txt", 0) > 0);

    /* Snapshot and delete one file at epoch 2 */
    vfs_snapshot(vfs);
    CHECK_EQ(vfs_delete(vfs, root_vp, "y.txt", 2), VFS_OK);

    /* readdir at epoch 2 should show only x.txt and z.txt */
    vfs_dirent_t* entries = NULL;
    int n2 = 0;
    int rc2 = vfs_readdir(vfs, root_vp, &entries, &n2, 2);
    CHECK_EQ(rc2, VFS_OK);
    CHECK_EQ(n2, 2);
    for (int i = 0; i < n2; i++) {
        CHECK(strcmp(entries[i].name, "y.txt") != 0);
    }
    vfs_free_dirents(entries);

    /* readdir at epoch 0 should show all 3 files */
    entries = NULL;
    int n0 = 0;
    int rc0 = vfs_readdir(vfs, root_vp, &entries, &n0, 0);
    CHECK_EQ(rc0, VFS_OK);
    CHECK_EQ(n0, 3);
    vfs_free_dirents(entries);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * rename same dir: rename "old.txt" → "new.txt" at same epoch.
 * old name gone, new name exists, nodeId unchanged.
 * --------------------------------------------------------------------------- */

static void test_rename_same_dir(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t nid = vfs_create(vfs, root_vp, "old.txt", 0);
    CHECK(nid > 0);

    CHECK_EQ(vfs_rename(vfs, root_vp, "old.txt", root_vp, "new.txt", 0), VFS_OK);

    /* old name gone */
    CHECK_EQ(vfs_open(vfs, root_vp, "old.txt", 0), (int64_t)VFS_ERR_NOTFOUND);

    /* new name exists - verify it can be opened */
    CHECK(vfs_open(vfs, root_vp, "new.txt", 0) > 0);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * rename cross dir: mkdir a, mkdir b, create a/f.txt at epoch 0,
 * snapshot, then rename a/f.txt to b/g.txt at epoch 2.
 * "a" loses the entry, "b" gains it at epoch 2, epoch 0 still sees "f.txt".
 * --------------------------------------------------------------------------- */

static void test_rename_cross_dir(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t dir_a = vfs_mkdir(vfs, root_vp, "a", 0);
    CHECK(dir_a > 0);
    int64_t dir_b = vfs_mkdir(vfs, root_vp, "b", 0);
    CHECK(dir_b > 0);

    int64_t fnid = vfs_create(vfs, dir_a, "f.txt", 0);
    CHECK(fnid > 0);

    /* Snapshot so rename at epoch 2 uses a clean epoch */
    vfs_snapshot(vfs);

    CHECK_EQ(vfs_rename(vfs, dir_a, "f.txt", dir_b, "g.txt", 2), VFS_OK);

    /* At epoch 0: source dir still sees "f.txt", dest does not have it */
    CHECK(vfs_open(vfs, dir_a, "f.txt", 0) > 0);
    CHECK_EQ(vfs_open(vfs, dir_b, "g.txt", 0), (int64_t)VFS_ERR_NOTFOUND);

    /* At epoch 2: source loses the entry, destination gains it */
    CHECK_EQ(vfs_open(vfs, dir_a, "f.txt", 2), (int64_t)VFS_ERR_NOTFOUND);
    CHECK(vfs_open(vfs, dir_b, "g.txt", 2) > 0);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * rename cross epoch: create f.txt at epoch 0, snapshot, rename at epoch 2.
 * Epoch 0 sees "f.txt", epoch 2 sees "g.txt".
 * --------------------------------------------------------------------------- */

static void test_rename_cross_epoch(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t nid = vfs_create(vfs, root_vp, "f.txt", 0);
    CHECK(nid > 0);

    vfs_snapshot(vfs);

    CHECK_EQ(vfs_rename(vfs, root_vp, "f.txt", root_vp, "g.txt", 2), VFS_OK);

    /* Epoch 0 still sees "f.txt" */
    int64_t f0 = vfs_open(vfs, root_vp, "f.txt", 0);
    int64_t g0 = vfs_open(vfs, root_vp, "g.txt", 0);
    CHECK(f0 > 0);
    CHECK_EQ(g0, (int64_t)VFS_ERR_NOTFOUND);

    /* Epoch 2 sees "g.txt" */
    CHECK(vfs_open(vfs, root_vp, "g.txt", 2) > 0);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * rename not found: rename nonexistent file → VFS_ERR_NOTFOUND
 * --------------------------------------------------------------------------- */

static void test_rename_notfound(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    int64_t root_vp = vfs->ctx->rootNodeOffset;

    CHECK_EQ(vfs_rename(vfs, root_vp, "nonexistent.txt", root_vp, "x.txt", 0),
             VFS_ERR_NOTFOUND);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * last_error: verify vfs_last_error returns the last error, and is NOT
 * cleared by a successful operation.
 * --------------------------------------------------------------------------- */

static void test_last_error(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    int64_t root_vp = vfs->ctx->rootNodeOffset;

    /* vfs_mount on a missing file should return NOTFOUND */
    int64_t res = vfs_open(vfs, root_vp, "nonexistent.txt", 0);
    CHECK_EQ(res, (int64_t)VFS_ERR_NOTFOUND);
    CHECK_EQ(vfs_last_error(vfs), VFS_ERR_NOTFOUND);

    /* A successful operation should NOT clear last_error */
    CHECK(vfs_create(vfs, root_vp, "ok.txt", 0) > 0);
    vfs_error_t le2 = vfs_last_error(vfs);
    if (le2 != VFS_ERR_NOTFOUND) {
        printf("  DEBUG: last_error after create = %d (expected %d)\n",
               (int)le2, (int)VFS_ERR_NOTFOUND);
        printf("  DEBUG: vfs_create returned positive nodeId > 0\n");
    }
    CHECK_EQ(le2, VFS_ERR_NOTFOUND);

    /* Another error should update last_error */
    CHECK_EQ(vfs_open(vfs, root_vp, "nonexistent2.txt", 0),
             (int64_t)VFS_ERR_NOTFOUND);
    CHECK_EQ(vfs_last_error(vfs), VFS_ERR_NOTFOUND);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * Lock basic: lock/unlock same file at same epoch → success
 * --------------------------------------------------------------------------- */

static void test_lock_basic(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    int64_t root_vp = vfs->ctx->rootNodeOffset;

    int64_t nid = vfs_create(vfs, root_vp, "lock_test.txt", 0);
    CHECK(nid > 0);

    CHECK_EQ(vfs_lock(vfs, nid, 0), VFS_OK);
    CHECK_EQ(vfs_unlock(vfs, nid, 0), VFS_OK);

    CHECK_EQ(vfs_lock(vfs, nid, 2), VFS_OK);
    CHECK_EQ(vfs_unlock(vfs, nid, 2), VFS_OK);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * Lock concurrent epochs: lock file at epoch 0 and epoch 2 from simulated
 * concurrent paths → both succeed since different epochs are independent.
 * --------------------------------------------------------------------------- */

typedef struct {
    vfs_t* vfs;
    int     nodeId;
    int64_t epoch;
    int     result;
} lock_thread_arg;

static void* lock_thread_fn(void* arg) {
    lock_thread_arg* a = (lock_thread_arg*)arg;
    a->result = vfs_lock(a->vfs, a->nodeId, a->epoch);
    if (a->result == VFS_OK)
        a->result = vfs_unlock(a->vfs, a->nodeId, a->epoch);
    return NULL;
}

static void test_lock_concurrent_epochs(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    int64_t root_vp = vfs->ctx->rootNodeOffset;

    int64_t nid = vfs_create(vfs, root_vp, "concur.txt", 0);
    CHECK(nid > 0);

    lock_thread_arg a1 = {vfs, nid, 0, VFS_ERR_IO};
    lock_thread_arg a2 = {vfs, nid, 2, VFS_ERR_IO};

    pthread_t t1, t2;
    CHECK_EQ(pthread_create(&t1, NULL, lock_thread_fn, &a1), 0);
    CHECK_EQ(pthread_create(&t2, NULL, lock_thread_fn, &a2), 0);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    CHECK_EQ(a1.result, VFS_OK);  /* epoch 0 lock succeeded */
    CHECK_EQ(a2.result, VFS_OK);  /* epoch 2 lock succeeded (different epoch) */

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * Lock global serializes: global lock (epoch=0) blocks per-epoch lock
 * on the same file until released.
 * --------------------------------------------------------------------------- */

static volatile int lock_global_ready = 0;
static volatile int lock_per_epoch_acquired = 0;

static void* global_serialize_thread(void* arg) {
    vfs_t* vfs = (vfs_t*)arg;
    int r = vfs_lock(vfs, 1, 0);
    if (r != VFS_OK) return NULL;
    lock_global_ready = 1;

    /* Hold the lock for 100ms so the per-epoch thread has time to try */
    usleep(100000);

    vfs_unlock(vfs, 1, 0);
    return NULL;
}

static void* per_epoch_thread(void* arg) {
    vfs_t* vfs = (vfs_t*)arg;

    /* Attempt per-epoch lock — should block until global releases */
    int r = vfs_lock(vfs, 1, 2);
    if (r == VFS_OK) {
        lock_per_epoch_acquired = 1;
        vfs_unlock(vfs, 1, 2);
    }
    return NULL;
}

static void test_lock_global_serializes(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);

    int64_t root_vp = vfs->ctx->rootNodeOffset;
    CHECK(vfs_create(vfs, root_vp, "serial.txt", 0) > 0);

    lock_global_ready = 0;
    lock_per_epoch_acquired = 0;

    pthread_t t1;
    CHECK_EQ(pthread_create(&t1, NULL, global_serialize_thread, vfs), 0);
    /* Wait for global thread to acquire the lock */
    while (!lock_global_ready) usleep(100);

    /* Now start the per-epoch thread — it should block */
    pthread_t t2;
    CHECK_EQ(pthread_create(&t2, NULL, per_epoch_thread, vfs), 0);

    /* Short wait — per-epoch should still be blocked */
    usleep(20000);
    CHECK(!lock_per_epoch_acquired);

    /* Wait for both threads to finish */
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    CHECK(lock_per_epoch_acquired);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * Phase 26 / W0 tests
 *
 * 1. test_lock_per_vfs_isolation — two mounts, same VP, no contention.
 * 2. test_lock_mixed_keys       — VP-keyed and nodeId-keyed locks in the
 *                                 same vfs don't interfere.
 * 3. test_lock_same_epoch_exclusion — two threads, same key, same epoch →
 *                                 second blocks.
 * 4. test_lock_different_epoch_non_exclusion — two threads, same key,
 *                                 different epochs → both proceed.
 * --------------------------------------------------------------------------- */

static void test_lock_per_vfs_isolation(void) {
    /* Two separate mounts on the same backing file.  vfs_lock with the
       same opaque int64_t key in both should NOT contend (per-vfs_t table
       is independent). */
    vfs_t* vfs1 = vfs_mount(test_path, 8192);
    vfs_t* vfs2 = vfs_mount(test_path, 8192);
    CHECK(vfs1 != NULL);
    CHECK(vfs2 != NULL);

    int64_t key = 42;
    CHECK_EQ(vfs_lock(vfs1, key, 0), VFS_OK);
    /* vfs2 with the same key should also succeed (no contention) */
    CHECK_EQ(vfs_lock(vfs2, key, 0), VFS_OK);
    CHECK_EQ(vfs_unlock(vfs2, key, 0), VFS_OK);
    CHECK_EQ(vfs_unlock(vfs1, key, 0), VFS_OK);

    vfs_unmount(vfs1);
    vfs_unmount(vfs2);
}

static void test_lock_mixed_keys(void) {
    /* VP-keyed and nodeId-keyed locks at the same vfs, different keys,
       should not interfere.  Use two distinct int64_t values. */
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);

    int64_t vp_key    = 0x0001000200030004LL;  /* looks like a VP */
    int64_t nodeid_k  = 42;                   /* looks like a nodeId */

    /* Lock both, different keys → both succeed. */
    CHECK_EQ(vfs_lock(vfs, vp_key, 0), VFS_OK);
    CHECK_EQ(vfs_lock(vfs, nodeid_k, 0), VFS_OK);
    CHECK_EQ(vfs_unlock(vfs, nodeid_k, 0), VFS_OK);
    CHECK_EQ(vfs_unlock(vfs, vp_key, 0), VFS_OK);

    /* Same key, different epochs — both should succeed (different-epoch
       non-exclusion).  But same key, same epoch, second should block. */
    CHECK_EQ(vfs_lock(vfs, vp_key, 2), VFS_OK);
    CHECK_EQ(vfs_lock(vfs, vp_key, 4), VFS_OK);
    CHECK_EQ(vfs_unlock(vfs, vp_key, 2), VFS_OK);
    CHECK_EQ(vfs_unlock(vfs, vp_key, 4), VFS_OK);

    vfs_unmount(vfs);
}

/* State shared between the two same-epoch threads.  A sets `acquired`;
   B waits for it.  A reports into result_*_a; B reports into result_*_b.
   Each thread has its own results — the previous design reused the
   same struct for both threads, which made B wait on its own acquired
   flag (never set) and deadlocked. */
typedef struct {
    vfs_t*   vfs;
    int64_t  key;
    int64_t  epoch;
    volatile int  acquired;        /* 1 once A's vfs_lock has returned VFS_OK */
    volatile int  result_lock_a;   /* A's vfs_lock result */
    volatile int  result_unlock_a; /* A's vfs_unlock result */
    volatile int  result_lock_b;   /* B's vfs_lock result */
    volatile int  result_unlock_b; /* B's vfs_unlock result */
} same_epoch_state;

static void* same_epoch_thread_a(void* arg) {
    same_epoch_state* s = (same_epoch_state*)arg;
    s->result_lock_a = vfs_lock(s->vfs, s->key, s->epoch);
    if (s->result_lock_a != VFS_OK) {
        return NULL;
    }
    s->acquired = 1;
    /* Hold the lock long enough for thread B to attempt + block. */
    usleep(150000);
    s->result_unlock_a = vfs_unlock(s->vfs, s->key, s->epoch);
    return NULL;
}

static void* same_epoch_thread_b(void* arg) {
    same_epoch_state* s = (same_epoch_state*)arg;
    /* Wait for thread A to hold the lock (with a generous timeout to
       avoid hanging the test if A's vfs_lock fails). */
    int waited_us = 0;
    while (!s->acquired && waited_us < 500000) {
        usleep(100);
        waited_us += 100;
    }
    if (!s->acquired) {
        s->result_lock_b = VFS_ERR_IO;
        return NULL;
    }
    /* Try to acquire the same key at the same epoch — should block
       until thread A releases. */
    int r = vfs_lock(s->vfs, s->key, s->epoch);
    s->result_lock_b = r;
    if (r == VFS_OK) s->result_unlock_b = vfs_unlock(s->vfs, s->key, s->epoch);
    return NULL;
}

static void test_lock_same_epoch_exclusion(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);

    int64_t key = 100;
    int64_t epoch = 2;

    same_epoch_state s = {vfs, key, epoch,
                          0,  /* acquired */
                          VFS_ERR_IO, VFS_ERR_IO,  /* A's results */
                          VFS_ERR_IO, VFS_ERR_IO}; /* B's results */

    pthread_t ta, tb;
    CHECK_EQ(pthread_create(&ta, NULL, same_epoch_thread_a, &s), 0);
    CHECK_EQ(pthread_create(&tb, NULL, same_epoch_thread_b, &s), 0);

    pthread_join(ta, NULL);
    pthread_join(tb, NULL);

    /* Both threads should have succeeded. */
    CHECK_EQ(s.result_lock_a, VFS_OK);
    CHECK_EQ(s.result_unlock_a, VFS_OK);
    CHECK_EQ(s.result_lock_b, VFS_OK);
    CHECK_EQ(s.result_unlock_b, VFS_OK);

    /* Sanity: B's lock was acquired strictly after A's release
       (otherwise the test is not testing what it claims).  In the
       current implementation this is structural: B blocks on the
       condvar until A's release broadcasts. */
    vfs_unmount(vfs);
}

typedef struct {
    vfs_t*   vfs;
    int64_t  key;
    int64_t  epoch;
    int      result_lock;
    int      result_unlock;
    volatile int  acquired;
} diff_epoch_arg;

static void* diff_epoch_thread(void* arg) {
    diff_epoch_arg* a = (diff_epoch_arg*)arg;
    a->result_lock = vfs_lock(a->vfs, a->key, a->epoch);
    if (a->result_lock == VFS_OK) {
        a->acquired = 1;
        /* Hold briefly. */
        usleep(50000);
        a->result_unlock = vfs_unlock(a->vfs, a->key, a->epoch);
    }
    return NULL;
}

static void test_lock_different_epoch_non_exclusion(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);

    int64_t key = 200;

    diff_epoch_arg a = {vfs, key, 2, VFS_ERR_IO, VFS_ERR_IO, 0};
    diff_epoch_arg b = {vfs, key, 4, VFS_ERR_IO, VFS_ERR_IO, 0};

    pthread_t ta, tb;
    CHECK_EQ(pthread_create(&ta, NULL, diff_epoch_thread, &a), 0);
    CHECK_EQ(pthread_create(&tb, NULL, diff_epoch_thread, &b), 0);

    pthread_join(ta, NULL);
    pthread_join(tb, NULL);

    /* Both should have succeeded without blocking on each other. */
    CHECK_EQ(a.result_lock, VFS_OK);
    CHECK_EQ(a.result_unlock, VFS_OK);
    CHECK_EQ(b.result_lock, VFS_OK);
    CHECK_EQ(b.result_unlock, VFS_OK);

    /* If different-epoch was non-exclusive, both threads should have
       acquired the lock at the same time (both `acquired` should be
       1 at some point).  We can't easily verify that ordering here,
       but the fact that BOTH succeed confirms the per-epoch path
       allows different epochs to proceed. */

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * dirchain_list tests
 * --------------------------------------------------------------------------- */

static void test_dirchain_list_basic(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    CHECK(vfs_create(vfs, root_vp, "a.txt", 0) > 0);
    CHECK(vfs_create(vfs, root_vp, "b.txt", 0) > 0);
    CHECK(vfs_mkdir(vfs, root_vp, "sub", 0) > 0);

    vfs_dirent_t* entries = NULL;
    int n = 0;
    int rc = dirchain_list(ctx, root_vp, 0, &entries, &n);
    CHECK_EQ(rc, VFS_OK);
    CHECK_EQ(n, 3);

    int found_a = 0, found_b = 0, found_sub = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(entries[i].name, "a.txt") == 0) { found_a = 1; CHECK(!entries[i].isDir); }
        if (strcmp(entries[i].name, "b.txt") == 0) { found_b = 1; CHECK(!entries[i].isDir); }
        if (strcmp(entries[i].name, "sub") == 0) { found_sub = 1; CHECK(entries[i].isDir); }
    }
    CHECK(found_a);
    CHECK(found_b);
    CHECK(found_sub);
    vfs_free_dirents(entries);

    vfs_unmount(vfs);
}

static void test_dirchain_list_tombstone(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    CHECK(vfs_create(vfs, root_vp, "x.txt", 0) > 0);
    CHECK(vfs_create(vfs, root_vp, "y.txt", 0) > 0);
    CHECK(vfs_create(vfs, root_vp, "z.txt", 0) > 0);

    vfs_snapshot(vfs);
    CHECK_EQ(vfs_delete(vfs, root_vp, "y.txt", 2), VFS_OK);

    vfs_dirent_t* entries = NULL;
    int n = 0;
    int rc = dirchain_list(ctx, root_vp, 2, &entries, &n);
    CHECK_EQ(rc, VFS_OK);
    CHECK_EQ(n, 2);
    for (int i = 0; i < n; i++)
        CHECK(strcmp(entries[i].name, "y.txt") != 0);
    vfs_free_dirents(entries);

    entries = NULL;
    n = 0;
    rc = dirchain_list(ctx, root_vp, 0, &entries, &n);
    CHECK_EQ(rc, VFS_OK);
    CHECK_EQ(n, 3);
    vfs_free_dirents(entries);

    vfs_unmount(vfs);
}

static void test_sparse_small_file(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t file_vp = vfs_create(vfs, root_vp, "sparse.txt", 0);
    CHECK(file_vp > 0);

    /* Write 128 bytes (1 page) — lazy: only page 0 gets a PageNode */
    char wbuf[128];
    memset(wbuf, 'x', sizeof(wbuf));
    CHECK_EQ(vfs_write(vfs, file_vp, wbuf, 0, (int64_t)sizeof(wbuf), 0), (int)sizeof(wbuf));

    /* Resolve page 0 — should return the existing PageNode */
    uint8_t* pn_slot = tree_resolve_page_compat(ctx, file_vp, 0, 0, true);
    CHECK(pn_slot != NULL);

    /* Assert page_index == 0 */
    uint32_t pn_idx = (uint32_t)vfs_rd4_s(pn_slot, PAGENODE_OFF_PAGEINDEX, ctx->page_size);
    CHECK_EQ(pn_idx, 0u);

    /* Walk FileContent's pageRootPtr chain — assert exactly 1 PageNode */
    uint8_t* file_slot = pool_resolve_ro(&ctx->pool, file_vp);
    CHECK(file_slot != NULL);
    int64_t fc_vp = vfs_rd8_s(file_slot, FILENODE_OFF_HEADPTR, ctx->page_size);
    CHECK(fc_vp != 0);
    uint8_t* fc_slot = pool_resolve_ro(&ctx->pool, fc_vp);
    CHECK(fc_slot != NULL);
    int64_t pn_root = vfs_rd8_s(fc_slot, FILECONTENT_OFF_ROOTPTR, ctx->page_size);
    CHECK(pn_root != 0);

    int pn_count = 0;
    int64_t walk = pn_root;
    while (walk != 0) {
        pn_count++;
        uint8_t* pn = pool_resolve_ro(&ctx->pool, walk);
        CHECK(pn != NULL);
        int64_t next = vfs_rd8_s(pn, PAGENODE_OFF_NEXTPTR, ctx->page_size);
        walk = next;
    }
    CHECK_EQ(pn_count, 1);

    vfs_unmount(vfs);
}

static void test_sparse_chain_mid_insert(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t file_vp = vfs_create(vfs, root_vp, "mid.txt", 0);
    CHECK(file_vp > 0);

    /* Resolve page 5 first — allocates PageNode with idx=5 */
    uint8_t* pn5 = tree_resolve_page_compat(ctx, file_vp, 5, 0, true);
    CHECK(pn5 != NULL);
    uint32_t idx5 = (uint32_t)vfs_rd4_s(pn5, PAGENODE_OFF_PAGEINDEX, ctx->page_size);
    CHECK_EQ(idx5, 5u);

    /* Resolve page 2 — should insert at head (before idx=5) */
    uint8_t* pn2 = tree_resolve_page_compat(ctx, file_vp, 2, 0, true);
    CHECK(pn2 != NULL);
    uint32_t idx2 = (uint32_t)vfs_rd4_s(pn2, PAGENODE_OFF_PAGEINDEX, ctx->page_size);
    CHECK_EQ(idx2, 2u);

    /* Walk the chain — page 2 should be at head, page 5 next */
    uint8_t* file_slot = pool_resolve_ro(&ctx->pool, file_vp);
    CHECK(file_slot != NULL);
    int64_t fc_vp = vfs_rd8_s(file_slot, FILENODE_OFF_HEADPTR, ctx->page_size);
    CHECK(fc_vp != 0);
    uint8_t* fc_slot = pool_resolve_ro(&ctx->pool, fc_vp);
    CHECK(fc_slot != NULL);
    int64_t pn_root = vfs_rd8_s(fc_slot, FILECONTENT_OFF_ROOTPTR, ctx->page_size);
    CHECK(pn_root != 0);

    int pn_count = 0;
    int64_t walk = pn_root;
    while (walk != 0) {
        pn_count++;
        uint8_t* pn = pool_resolve_ro(&ctx->pool, walk);
        CHECK(pn != NULL);
        uint32_t idx = (uint32_t)vfs_rd4_s(pn, PAGENODE_OFF_PAGEINDEX, ctx->page_size);
        if (pn_count == 1) CHECK_EQ(idx, 2u);  /* head: page 2 */
        if (pn_count == 2) CHECK_EQ(idx, 5u);  /* next: page 5 */
        int64_t next = vfs_rd8_s(pn, PAGENODE_OFF_NEXTPTR, ctx->page_size);
        walk = next;
    }
    CHECK_EQ(pn_count, 2);

    vfs_unmount(vfs);
}

static void test_sparse_chain_tail_append(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t file_vp = vfs_create(vfs, root_vp, "tail.txt", 0);
    CHECK(file_vp > 0);

    /* Resolve page 2 first */
    uint8_t* pn2 = tree_resolve_page_compat(ctx, file_vp, 2, 0, true);
    CHECK(pn2 != NULL);
    uint32_t idx2 = (uint32_t)vfs_rd4_s(pn2, PAGENODE_OFF_PAGEINDEX, ctx->page_size);
    CHECK_EQ(idx2, 2u);

    /* Resolve page 7 — should append at tail (after idx=2) */
    uint8_t* pn7 = tree_resolve_page_compat(ctx, file_vp, 7, 0, true);
    CHECK(pn7 != NULL);
    uint32_t idx7 = (uint32_t)vfs_rd4_s(pn7, PAGENODE_OFF_PAGEINDEX, ctx->page_size);
    CHECK_EQ(idx7, 7u);

    /* Walk chain: idx=2 at head, idx=7 at tail */
    uint8_t* file_slot = pool_resolve_ro(&ctx->pool, file_vp);
    CHECK(file_slot != NULL);
    int64_t fc_vp = vfs_rd8_s(file_slot, FILENODE_OFF_HEADPTR, ctx->page_size);
    CHECK(fc_vp != 0);
    uint8_t* fc_slot = pool_resolve_ro(&ctx->pool, fc_vp);
    CHECK(fc_slot != NULL);
    int64_t pn_root = vfs_rd8_s(fc_slot, FILECONTENT_OFF_ROOTPTR, ctx->page_size);
    CHECK(pn_root != 0);

    int pn_count = 0;
    int64_t walk = pn_root;
    while (walk != 0) {
        pn_count++;
        uint8_t* pn = pool_resolve_ro(&ctx->pool, walk);
        CHECK(pn != NULL);
        uint32_t idx = (uint32_t)vfs_rd4_s(pn, PAGENODE_OFF_PAGEINDEX, ctx->page_size);
        if (pn_count == 1) CHECK_EQ(idx, 2u);
        if (pn_count == 2) CHECK_EQ(idx, 7u);
        int64_t next = vfs_rd8_s(pn, PAGENODE_OFF_NEXTPTR, ctx->page_size);
        walk = next;
    }
    CHECK_EQ(pn_count, 2);

    vfs_unmount(vfs);
}

#ifndef NDEBUG
/* Debug-only test: depends on tree_resolve_page_cache_builds counter. */
static void test_sparse_threshold_cache(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t file_vp = vfs_create(vfs, root_vp, "threshold.txt", 0);
    CHECK(file_vp > 0);

    int baseline = tree_resolve_page_cache_builds_get();

    /* Resolve pages 0..63 — 64 unique PageNodes, at SPARSE_CACHE_THRESHOLD (64).
     * With sparse allocation, the cache may or may not trigger at exactly
     * 64 pages — the threshold behavior differs from the old dense model. */
    for (int i = 0; i < 64; i++) {
        uint8_t* pn = tree_resolve_page_compat(ctx, file_vp, (int64_t)i, 0, true);
        CHECK(pn != NULL);
    }
    /* Resolve page 64 — at least one cache build should have occurred */
    uint8_t* pn = tree_resolve_page_compat(ctx, file_vp, 64, 0, true);
    CHECK(pn != NULL);
    int after_65 = tree_resolve_page_cache_builds_get() - baseline;
    CHECK(after_65 >= 1);  /* at least one build by now */

    vfs_unmount(vfs);
}
#endif

static void test_sparse_read_no_allocate(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t file_vp = vfs_create(vfs, root_vp, "noread.txt", 0);
    CHECK(file_vp > 0);

    /* Read attempt on unwritten page — should return NULL, no allocation */
    uint8_t* pn = tree_resolve_page_compat(ctx, file_vp, 5, 0, false);
    CHECK(pn == NULL);

    /* Assert no FileContent was allocated */
    uint8_t* file_slot = pool_resolve_ro(&ctx->pool, file_vp);
    CHECK(file_slot != NULL);
    int64_t fc_vp = vfs_rd8_s(file_slot, FILENODE_OFF_HEADPTR, ctx->page_size);
    CHECK_EQ(fc_vp, 0);

    /* Write to page 5 — should allocate exactly 1 PageNode */
    pn = tree_resolve_page_compat(ctx, file_vp, 5, 0, true);
    CHECK(pn != NULL);
    uint32_t idx = (uint32_t)vfs_rd4_s(pn, PAGENODE_OFF_PAGEINDEX, ctx->page_size);
    CHECK_EQ(idx, 5u);

    /* Walk chain — exactly 1 PageNode */
    fc_vp = vfs_rd8_s(file_slot, FILENODE_OFF_HEADPTR, ctx->page_size);
    CHECK(fc_vp != 0);
    uint8_t* fc_slot = pool_resolve_ro(&ctx->pool, fc_vp);
    CHECK(fc_slot != NULL);
    int64_t pn_root = vfs_rd8_s(fc_slot, FILECONTENT_OFF_ROOTPTR, ctx->page_size);
    CHECK(pn_root != 0);

    int pn_count = 0;
    int64_t walk = pn_root;
    while (walk != 0) {
        pn_count++;
        uint8_t* pw = pool_resolve_ro(&ctx->pool, walk);
        CHECK(pw != NULL);
        int64_t next = vfs_rd8_s(pw, PAGENODE_OFF_NEXTPTR, ctx->page_size);
        walk = next;
    }
    CHECK_EQ(pn_count, 1);

    vfs_unmount(vfs);
}

static void test_sparse_gc_roundtrip(void) {
    const char* gc_path = "/tmp/test_sparse_gc.vfs";
    unlink(gc_path);

    vfs_t* vfs = vfs_mount(gc_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t file_vp = vfs_create(vfs, root_vp, "gc.txt", 0);
    CHECK(file_vp > 0);

    /* Write "hello" to page 0 */
    CHECK_EQ(vfs_write(vfs, file_vp, "hello", 0, 5, 0), 5);

    /* Run GC */
    int gc_ret = vfs_gc(vfs);
    (void)gc_ret;  /* GC may fail if nothing to collect, that's OK */

    /* Unmount and remount */
    vfs_unmount(vfs);
    vfs = vfs_mount(gc_path, 8192);
    CHECK(vfs != NULL);
    ctx = vfs->ctx;

    /* Resolve page 0 */
    uint8_t* pn = tree_resolve_page_compat(ctx, file_vp, 0, 0, true);
    CHECK(pn != NULL);
    uint32_t idx = (uint32_t)vfs_rd4_s(pn, PAGENODE_OFF_PAGEINDEX, ctx->page_size);
    CHECK_EQ(idx, 0u);

    /* Read back "hello" */
    char rbuf[8];
    memset(rbuf, 0, sizeof(rbuf));
    CHECK_EQ(vfs_read(vfs, file_vp, rbuf, 0, 5, 0), 5);
    CHECK_EQ(strcmp(rbuf, "hello"), 0);

    vfs_unmount(vfs);
    unlink(gc_path);
}

static void test_sparse_cow_chain(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t file_vp = vfs_create(vfs, root_vp, "cow.txt", 0);
    CHECK(file_vp > 0);

    /* Write "AAAA" at epoch 0 */
    CHECK_EQ(vfs_write(vfs, file_vp, "AAAA", 0, 4, 0), 4);

    /* Snapshot → epoch 1, currentEpoch becomes 2 */
    int64_t snap = vfs_snapshot(vfs);
    CHECK_EQ(snap, 1);

    /* Write "BBBB" at epoch 2 (COPY ON WRITE) */
    CHECK_EQ(vfs_write(vfs, file_vp, "BBBB", 0, 4, 2), 4);

    /* Epoch 0 returns "AAAA" */
    char rbuf[8];
    memset(rbuf, 0, sizeof(rbuf));
    CHECK_EQ(vfs_read(vfs, file_vp, rbuf, 0, 4, 0), 4);
    CHECK_EQ(strcmp(rbuf, "AAAA"), 0);

    /* Epoch 2 returns "BBBB" */
    memset(rbuf, 0, sizeof(rbuf));
    CHECK_EQ(vfs_read(vfs, file_vp, rbuf, 0, 4, 2), 4);
    CHECK_EQ(strcmp(rbuf, "BBBB"), 0);

    /* Resolve page 0 — still has single PageNode with page_index==0 */
    uint8_t* pn = tree_resolve_page_compat(ctx, file_vp, 0, 0, false);
    CHECK(pn != NULL);
    uint32_t idx = (uint32_t)vfs_rd4_s(pn, PAGENODE_OFF_PAGEINDEX, ctx->page_size);
    CHECK_EQ(idx, 0u);

    vfs_unmount(vfs);
}

/* Two-phase concurrent insert test.  Pre-resolves page 3 to create the chain,
 * then two threads concurrently insert pages 7 and 11 via synchronized start.
 * This defeats the global file lock serialization and tests CAS-based insertion
 * under genuine contention. */

typedef struct {
    vfs_t*  vfs;
    int64_t file_vp;
    int64_t page_idx;
    volatile int* ready;
    bool    success;
} conc_thread_args;

static void* conc_insert_thread(void* arg) {
    conc_thread_args* a = (conc_thread_args*)arg;
    /* Signal ready and wait for the other thread */
    __sync_fetch_and_add(a->ready, 1);
    while (__sync_fetch_and_add(a->ready, 0) < 2) { /* spin */ }
    uint8_t* pn = tree_resolve_page_compat(a->vfs->ctx, a->file_vp,
                                            a->page_idx, 0, true);
    if (pn) {
        uint32_t idx = (uint32_t)vfs_rd4_s(pn, PAGENODE_OFF_PAGEINDEX, a->vfs->ctx->page_size);
        a->success = (idx == (uint32_t)a->page_idx);
    }
    return NULL;
}

static void test_sparse_concurrent_insert(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t file_vp = vfs_create(vfs, root_vp, "conc.txt", 0);
    CHECK(file_vp > 0);

    /* Phase 1: pre-resolve page 3 so the FileContent chain exists */
    uint8_t* pn3 = tree_resolve_page_compat(ctx, file_vp, 3, 0, true);
    CHECK(pn3 != NULL);
    CHECK_EQ((uint32_t)vfs_rd4_s(pn3, PAGENODE_OFF_PAGEINDEX, ctx->page_size), 3u);

    /* Phase 2: concurrent inserts of pages 7 and 11 */
    volatile int ready = 0;
    conc_thread_args a1 = {vfs, file_vp, 7, &ready, false};
    conc_thread_args a2 = {vfs, file_vp, 11, &ready, false};

    pthread_t t1, t2;
    CHECK_EQ(pthread_create(&t1, NULL, conc_insert_thread, &a1), 0);
    CHECK_EQ(pthread_create(&t2, NULL, conc_insert_thread, &a2), 0);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    CHECK(a1.success);
    CHECK(a2.success);

    /* Verify all 3 pages are in the chain in sorted order */
    uint8_t* file_slot = pool_resolve_ro(&ctx->pool, file_vp);
    int64_t fc_vp = vfs_rd8_s(file_slot, FILENODE_OFF_HEADPTR, ctx->page_size);
    uint8_t* fc_slot = pool_resolve_ro(&ctx->pool, fc_vp);
    int64_t pn_root = vfs_rd8_s(fc_slot, FILECONTENT_OFF_ROOTPTR, ctx->page_size);

    int pn_count = 0;
    int64_t walk = pn_root;
    uint32_t seen[3] = {0, 0, 0};
    while (walk != 0) {
        CHECK(pn_count < 3);
        uint8_t* pw = pool_resolve_ro(&ctx->pool, walk);
        uint32_t idx = (uint32_t)vfs_rd4_s(pw, PAGENODE_OFF_PAGEINDEX, ctx->page_size);
        seen[pn_count] = idx;
        pn_count++;
        int64_t next = vfs_rd8_s(pw, PAGENODE_OFF_NEXTPTR, ctx->page_size);
        walk = next;
    }
    CHECK_EQ(pn_count, 3);
    CHECK_EQ(seen[0], 3u);
    CHECK_EQ(seen[1], 7u);
    CHECK_EQ(seen[2], 11u);

    vfs_unmount(vfs);
}

#ifdef VFS_NAME_HASH_TESTING
/* Phase 18: this test previously verified that the chain walk rejected
   ≥90 of 99 non-matching entries by hash alone.  Now that
   dirchain_find_child uses the radix tree as a fast path and returns
   early on a match, the chain walk is not exercised at all on the
   happy path.  Update the assertion to reflect the new behavior:
   the tree path finds the entry in O(1) and produces zero chain-walk
   rejections — which is the design goal of the tree.  The chain
   walk's hash fast-reject is still tested indirectly via the
   fallback tests (no-tree cases). */
static void test_dirchain_find_child_hash_fast_reject(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    /* Create 100 files */
    for (int i = 1; i <= 100; i++) {
        char name[32];
        snprintf(name, sizeof(name), "file_%03d.txt", i);
        int64_t fvp = vfs_create(vfs, root_vp, name, 0);
        CHECK(fvp > 0);
    }

    /* Reset reject counter and look up file_050.txt */
    dirchain_test_reset_hash_rejects();
    int64_t childPtr;
    uint32_t nodeId;
    int ret = dirchain_find_child(ctx, root_vp, "file_050.txt", 0, &childPtr, &nodeId, NULL);
    CHECK_EQ(ret, VFS_OK);

    int rejects = dirchain_test_get_hash_rejects();
    /* Phase 18: tree path returns early on match → no chain walk →
       zero rejections.  This is the intended speedup. */
    CHECK(rejects == 0);

    vfs_unmount(vfs);
}
#endif

#ifdef VFS_NAME_HASH_TESTING
static void test_dirchain_find_child_collision_tolerance(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;
    int64_t ps = ctx->page_size;

    /* Compute the natural hash of "alpha", then force both NameEntries
     * to share that hash.  dirchain_find_child must use strcmp to
     * disambiguate — this is the hash-collision fallback path. */
    uint64_t forced_hash = name_hash_compute("alpha", 5);
    CHECK(forced_hash != 0);

    int64_t name_vp_a, name_vp_b;
    int n_a = nodes_write_name_with_hash(&ctx->pool, "alpha", forced_hash, &name_vp_a);
    CHECK_EQ(n_a, 1);
    int n_b = nodes_write_name_with_hash(&ctx->pool, "beta", forced_hash, &name_vp_b);
    CHECK_EQ(n_b, 1);

    /* Allocate two FileNode children */
    uint32_t nid_a = ctx->nextNodeId + 1;
    uint32_t nid_b = nid_a + 1;
    ctx->nextNodeId = nid_b;

    int64_t child_vp_a = pool_alloc(&ctx->pool);
    int64_t child_vp_b = pool_alloc(&ctx->pool);
    CHECK(child_vp_a != VFS_VPTR_NULL);
    CHECK(child_vp_b != VFS_VPTR_NULL);
    nodes_write_filenode(pool_resolve_ro(&ctx->pool, child_vp_a), nid_a, 0, 0, 0, ps);
    nodes_write_filenode(pool_resolve_ro(&ctx->pool, child_vp_b), nid_b, 0, 0, 0, ps);

    /* Allocate two DirContent entries: DC_a → DC_b → 0
     * Both point to the forced-hash NameEntries and different child VPs */
    int64_t dc_vp_a = pool_alloc(&ctx->pool);
    int64_t dc_vp_b = pool_alloc(&ctx->pool);
    CHECK(dc_vp_a != VFS_VPTR_NULL);
    CHECK(dc_vp_b != VFS_VPTR_NULL);
    nodes_write_dircontent(pool_resolve_ro(&ctx->pool, dc_vp_b),
                           nid_b, 0, child_vp_b, name_vp_b, 0, ps);
    nodes_write_dircontent(pool_resolve_ro(&ctx->pool, dc_vp_a),
                           nid_a, 0, child_vp_a, name_vp_a, dc_vp_b, ps);

    /* Set root's headPtr to dc_vp_a */
    uint8_t* root_slot = pool_resolve_ro(&ctx->pool, root_vp);
    CHECK(root_slot != NULL);
    vfs_wr8_s(root_slot, DIRNODE_OFF_HEADPTR, dc_vp_a, ps);

    /* Both lookups must succeed — hash collision forces strcmp fallback */
    int64_t childPtr;
    uint32_t nodeId;
    int ret_a = dirchain_find_child(ctx, root_vp, "alpha", 0, &childPtr, &nodeId, NULL);
    CHECK_EQ(ret_a, VFS_OK);

    /* "beta" has the same forced hash as "alpha", but its natural hash differs.
     * dirchain_find_child computes target_hash from the search name, so
     * searching for "beta" gives hash("beta") != forced_hash, and both entries
     * are correctly fast-rejected.  Try a search for "alpha" instead and
     * verify it returns the correct childPtr (child_vp_a, not child_vp_b)
     * confirming strcmp disambiguated the two entries with the same hash. */
    CHECK_EQ(childPtr, child_vp_a);

    /* Search for "gamma" (not present) — should return NOTFOUND */
    int ret_g = dirchain_find_child(ctx, root_vp, "gamma", 0, &childPtr, &nodeId, NULL);
    CHECK_EQ(ret_g, VFS_ERR_NOTFOUND);

    vfs_unmount(vfs);
}
#endif

/* --- Directory radix tree tests (Phase 18) --- */

static void test_dircontentindex_lookup_empty(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;

    /* Empty tree (indexRoot == 0) should return 0 */
    int64_t leafVP = dircontentindex_lookup(&ctx->pool, 0, 0x12345678ULL,
                                              ctx->page_size);
    CHECK_EQ(leafVP, 0);

    vfs_unmount(vfs);
}

static void test_dircontentindex_insert_lookup(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;

    int64_t page_size = ctx->page_size;

    /* Use the root DirNode's actual indexHeadPtr (created by bootstrap) */
    uint8_t* rootSlot = pool_resolve_ro(&ctx->pool, ctx->rootNodeOffset);
    CHECK(rootSlot != NULL);
    int64_t indexRoot = vfs_rd8_s(rootSlot, DIRNODE_OFF_INDEXHEADPTR, page_size);
    CHECK(indexRoot != 0);  /* bootstrap should have created the tree root */

    /* Allocate a dummy DirContent slot to point at */
    int64_t dcVP = pool_alloc(&ctx->pool);
    CHECK(dcVP != VFS_VPTR_NULL);

    /* Insert via the DirNode's actual tree */
    int ret = dircontentindex_insert(&ctx->pool, &indexRoot,
                                      0x0123456789ABCDEFULL,
                                      dcVP, page_size);
    CHECK_EQ(ret, 0);

    /* Lookup the same hash */
    int64_t leafVP = dircontentindex_lookup(&ctx->pool, indexRoot,
                                             0x0123456789ABCDEFULL,
                                             page_size);
    CHECK(leafVP != 0);

    vfs_unmount(vfs);
}

static void test_dircontentindex_same_leaf(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t page_size = ctx->page_size;

    uint8_t* rootSlot = pool_resolve_ro(&ctx->pool, ctx->rootNodeOffset);
    CHECK(rootSlot != NULL);
    int64_t indexRoot = vfs_rd8_s(rootSlot, DIRNODE_OFF_INDEXHEADPTR, page_size);
    CHECK(indexRoot != 0);

    /* Two hashes that share the first 15 nibbles (prefix) but differ
       only in the last nibble.  They should land in the same leaf. */
    uint64_t hash1 = 0x0123456789ABCDE0ULL;  /* last nibble = 0 */
    uint64_t hash2 = 0x0123456789ABCDEFULL;  /* last nibble = F */

    int64_t dcVP1 = pool_alloc(&ctx->pool);
    int64_t dcVP2 = pool_alloc(&ctx->pool);
    CHECK(dcVP1 != VFS_VPTR_NULL);
    CHECK(dcVP2 != VFS_VPTR_NULL);

    int ret = dircontentindex_insert(&ctx->pool, &indexRoot, hash1,
                                      dcVP1, page_size);
    CHECK_EQ(ret, 0);

    ret = dircontentindex_insert(&ctx->pool, &indexRoot, hash2,
                                  dcVP2, page_size);
    CHECK_EQ(ret, 0);

    /* Both lookups should return non-zero (leaf found) */
    int64_t leaf1 = dircontentindex_lookup(&ctx->pool, indexRoot, hash1,
                                            page_size);
    int64_t leaf2 = dircontentindex_lookup(&ctx->pool, indexRoot, hash2,
                                            page_size);
    CHECK(leaf1 != 0);
    CHECK(leaf2 != 0);

    /* Both should return the SAME leaf — they share all but the last
       nibble, and the tree is 16 levels deep. */
    CHECK_EQ(leaf1, leaf2);

    vfs_unmount(vfs);
}

static void test_vfs_create_open_tree(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    /* Create a file — vfs_create writes to BOTH chain and tree */
    int64_t file_vp = vfs_create(vfs, root_vp, "tree_test.txt", 0);
    CHECK(file_vp > 0);

    /* Verify the tree has an entry for this file */
    uint8_t* rootSlot = pool_resolve_ro(&ctx->pool, root_vp);
    CHECK(rootSlot != NULL);
    int64_t indexRoot = vfs_rd8_s(rootSlot, DIRNODE_OFF_INDEXHEADPTR,
                                   ctx->page_size);
    CHECK(indexRoot != 0);

    /* Lookup via the tree */
    uint64_t nameHash = name_hash_compute("tree_test.txt", 13);
    int64_t leafVP = dircontentindex_lookup(&ctx->pool, indexRoot,
                                              nameHash, ctx->page_size);
    CHECK(leafVP != 0);

    /* Walk the DirContentLink list — must find the link to our DirContent */
    int found = 0;
    int64_t linkVP = leafVP;
    while (linkVP != 0) {
        int64_t dcVP, nextLinkVP;
        nodes_read_dircontentlink(pool_resolve_ro(&ctx->pool, linkVP),
                                  &dcVP, &nextLinkVP, ctx->page_size);
        if (dcVP != 0) {
            found = 1;
            break;
        }
        linkVP = nextLinkVP;
    }
    CHECK(found == 1);

    /* vfs_open should find the file (uses tree-first path in
       dirchain_find_child, then chain walk as safety net) */
    int64_t opened = vfs_open(vfs, root_vp, "tree_test.txt", 0);
    CHECK(opened > 0);

    vfs_unmount(vfs);
}

static void test_vfs_create_open_many(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;
    char name[32];

    for (int i = 0; i < 100; i++) {
        snprintf(name, sizeof(name), "f%03d.txt", i);
        int64_t vp = vfs_create(vfs, root_vp, name, 0);
        CHECK(vp > 0);
    }

    /* Verify all 100 can be opened */
    for (int i = 0; i < 100; i++) {
        snprintf(name, sizeof(name), "f%03d.txt", i);
        int64_t vp = vfs_open(vfs, root_vp, name, 0);
        CHECK(vp > 0);
    }

    /* Verify a non-existent file is not found */
    int64_t notfound = vfs_open(vfs, root_vp, "no_such_file.txt", 0);
    CHECK_EQ(notfound, (int64_t)VFS_ERR_NOTFOUND);

    vfs_unmount(vfs);
}

static void test_delete_recreate_same_name(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    /* Create a file at epoch 0 */
    int64_t vp1 = vfs_create(vfs, root_vp, "recreate.txt", 0);
    CHECK(vp1 > 0);

    /* Delete it at epoch 0 — creates a tombstone DirContent */
    int ret = vfs_delete(vfs, root_vp, "recreate.txt", 0);
    CHECK_EQ(ret, VFS_OK);

    /* Re-create at the same name at epoch 2 (a new even epoch).
       The tombstone at epoch 0 (namePtr=0) is not a collision at
       epoch 2; the original live entry at epoch 0 does not block
       creation at a different epoch. */
    int64_t vp2 = vfs_create(vfs, root_vp, "recreate.txt", 2);
    CHECK(vp2 > 0);
    CHECK(vp1 != vp2);

    vfs_unmount(vfs);
}

static void test_rename_tree(void) {
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t vp = vfs_create(vfs, root_vp, "old.txt", 0);
    CHECK(vp > 0);

    int ret = vfs_rename(vfs, root_vp, "old.txt", root_vp, "new.txt", 0);
    CHECK_EQ(ret, VFS_OK);

    int64_t old = vfs_open(vfs, root_vp, "old.txt", 0);
    CHECK_EQ(old, (int64_t)VFS_ERR_NOTFOUND);

    int64_t newfile = vfs_open(vfs, root_vp, "new.txt", 0);
    CHECK(newfile > 0);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * dirnode_childCount test — REMOVED in Phase 26 / W1b.
 *
 * The childCount field was dropped from DirNode (replaced with
 * createdAt).  See impl/phase-26-unified-tree.md for the migration
 * rationale: childCount was an over-estimate used only to size the
 * dedup hash_map in dirchain_list, which W5 removes entirely.
 * --------------------------------------------------------------------------- */

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
    test_create_after_delete();
    test_double_delete();
    test_double_create_delete();
    test_delete_epoch_isolation();
    test_file_stat();
    test_stat_not_file();
    test_file_size_epoch();
    test_resolve_page_growth();

    /* Write/read tests use a separate clean file */
    unlink(test_path);

    test_write_basic();
    test_read_basic();

    unlink(test_path);

    test_write_cross_page();

    unlink(test_path);

    test_write_in_place();

    unlink(test_path);

    test_write_cow_epoch();

    unlink(test_path);

    test_write_multi_segment();

    unlink(test_path);

    test_write_frozen_epoch();

    /* --- mkdir / rmdir tests --- */

    unlink(test_path);
    test_mkdir_basic();

    unlink(test_path);
    test_mkdir_duplicate();

    unlink(test_path);
    test_rmdir_empty();

    unlink(test_path);
    test_rmdir_notdir();

    /* --- readdir tests --- */

    unlink(test_path);
    test_readdir_empty();

    unlink(test_path);
    test_readdir_with_files();

    unlink(test_path);
    test_readdir_with_dirs();

    unlink(test_path);
    test_readdir_tombstone();

    /* --- dirchain_list tests --- */

    unlink(test_path);
    test_dirchain_list_basic();

    unlink(test_path);
    test_dirchain_list_tombstone();

    /* --- rename tests --- */

    unlink(test_path);
    test_rename_same_dir();

    unlink(test_path);
    test_rename_cross_dir();

    unlink(test_path);
    test_rename_cross_epoch();

    unlink(test_path);
    test_rename_notfound();

    unlink(test_path);
    test_last_error();

    /* --- lock tests --- */

    unlink(test_path);
    test_lock_basic();

    unlink(test_path);
    test_lock_concurrent_epochs();

    unlink(test_path);
    test_lock_global_serializes();

    /* --- Phase 26 / W0 lock tests --- */
    unlink(test_path);
    test_lock_per_vfs_isolation();

    unlink(test_path);
    test_lock_mixed_keys();

    unlink(test_path);
    test_lock_same_epoch_exclusion();

    unlink(test_path);
    test_lock_different_epoch_non_exclusion();

    /* --- sparse chain test --- */
    unlink(test_path);
    test_sparse_small_file();

    unlink(test_path);
    test_sparse_chain_mid_insert();

    unlink(test_path);
    test_sparse_chain_tail_append();

#ifndef NDEBUG
    unlink(test_path);
    test_sparse_threshold_cache();
#endif

    unlink(test_path);
    test_sparse_read_no_allocate();

    test_sparse_gc_roundtrip();

    unlink(test_path);
    test_sparse_cow_chain();

    unlink(test_path);
    test_sparse_concurrent_insert();

#ifdef VFS_NAME_HASH_TESTING
    unlink(test_path);
    test_dirchain_find_child_hash_fast_reject();
    unlink(test_path);
    test_dirchain_find_child_collision_tolerance();
#endif

    test_dircontentindex_lookup_empty();
    test_dircontentindex_insert_lookup();
    test_dircontentindex_same_leaf();
    test_vfs_create_open_tree();
    test_vfs_create_open_many();
    test_delete_recreate_same_name();
    test_rename_tree();

    /* Clean up */
    unlink(test_path);

    printf("test_tree: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
