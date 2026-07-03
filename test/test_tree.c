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
    vfs_t* vfs = vfs_open(test_path, 8192);
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
    vfs_t* vfs = vfs_open(test_path, 8192);
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
    vfs_t* vfs = vfs_open(test_path, 8192);
    CHECK(vfs != NULL);

    CHECK(vfs->ctx->pool_list_head_value != 0);

    vfs_close(vfs);
}

/* ---------------------------------------------------------------------------
 * vfs_create test
 * --------------------------------------------------------------------------- */

static void test_create_file(void) {
    vfs_t* vfs = vfs_open(test_path, 8192);
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

    vfs_close(vfs);
}

/* ---------------------------------------------------------------------------
 * vfs_delete test
 * --------------------------------------------------------------------------- */

static void test_delete_file(void) {
    vfs_t* vfs = vfs_open(test_path, 8192);
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

    vfs_close(vfs);
}

/* ---------------------------------------------------------------------------
 * vfs_open_file test
 * --------------------------------------------------------------------------- */

static void test_open_file(void) {
    vfs_t* vfs = vfs_open(test_path, 8192);
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
                              &ce_c, &ce_e, &ce_cp, &ce_np, &ce_nx, VFS_PAGE_SIZE);
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
    vfs_t* vfs = vfs_open(test_path, 8192);
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
    vfs_t* vfs = vfs_open(test_path, 8192);
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
    vfs_t* vfs = vfs_open(test_path, 8192);
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
                              &cc, &ce, &cp, &np, &nx, VFS_PAGE_SIZE);
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
    vfs_t* vfs = vfs_open(test_path, 8192);
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
                          &cc, &ce, &cp, &np, &nx, VFS_PAGE_SIZE);
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
    vfs_t* vfs = vfs_open(test_path, 8192);
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

    vfs_close(vfs);
}

/* ---------------------------------------------------------------------------
 * tree_resolve_page test — file growth and in-memory page array
 * --------------------------------------------------------------------------- */

static void test_resolve_page_growth(void) {
    vfs_t* vfs = vfs_open(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int nodeId = vfs_create(vfs, root_vp, "big.txt", 0);
    CHECK(nodeId > 0);
    int64_t file_vp = get_file_vp(&ctx->pool, root_vp);
    CHECK(file_vp != 0);

    uint32_t seg_size = ctx->segment_size;
    CHECK(seg_size > 0);

    /* Resolve page 0 — should create first segment */
    uint8_t* pn0 = tree_resolve_page(ctx, file_vp, 0, 0);
    CHECK(pn0 != NULL);

    /* Verify it's a PageNode with versionRootPtr=0 (never written) */
    CHECK_EQ(vfs_rd8(pn0, PAGENODE_OFF_VERSIONROOT), 0);

    /* Cache should now be populated for this segment */
    CHECK(ctx->seg_array_fc_vp != 0);
    CHECK(ctx->seg_array_cache.built);

    /* Resolve page 1 — should hit cached array */
    uint8_t* pn1 = tree_resolve_page(ctx, file_vp, 1, 0);
    CHECK(pn1 != NULL);
    CHECK_EQ(vfs_rd8(pn1, PAGENODE_OFF_VERSIONROOT), 0);

    /* Page 1 should be at nextPtr offset from page 0 in the chain */
    uint8_t* fc_slot = pool_resolve(&ctx->pool, ctx->seg_array_fc_vp);
    CHECK(fc_slot != NULL);
    int64_t fc_root = vfs_rd8(fc_slot, FILECONTENT_OFF_ROOTPTR);
    CHECK(fc_root != 0);

    /* Walk from root to page 1 slot via nextPtr */
    int64_t pn_vp = fc_root;
    for (int i = 0; i < 2 && pn_vp != 0; i++) {
        uint8_t* slot = pool_resolve(&ctx->pool, pn_vp);
        CHECK(slot != NULL);
        if (i == 0) {
            CHECK_EQ(slot, pn0);  /* first PageNode matches */
        }
        if (i == 1) {
            CHECK_EQ(slot, pn1);  /* second PageNode matches */
        }
        pn_vp = vfs_rd8(slot, PAGENODE_OFF_NEXTPTR);
    }
    CHECK(pn_vp != 0);  /* chain has more than 2 entries */

    /* Resolve page at segment boundary — should create second segment */
    uint8_t* pn_first_new = tree_resolve_page(ctx, file_vp, seg_size, 0);
    CHECK(pn_first_new != NULL);
    CHECK_EQ(vfs_rd8(pn_first_new, PAGENODE_OFF_VERSIONROOT), 0);

    /* Cache should now point to the second segment */
    CHECK(ctx->seg_array_fc_vp != 0);

    /* Resolve page 0 again — cache may have been invalidated by second segment.
       Just verify it still works. */
    pn0 = tree_resolve_page(ctx, file_vp, 0, 0);
    CHECK(pn0 != NULL);

    vfs_close(vfs);
}

/* ---------------------------------------------------------------------------
 * vfs_write test — basic write, size update, cross-page write
 * --------------------------------------------------------------------------- */

static void test_write_basic(void) {
    vfs_t* vfs = vfs_open(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int nodeId = vfs_create(vfs, root_vp, "write.txt", 0);
    CHECK(nodeId > 0);
    int64_t file_vp = get_file_vp(&ctx->pool, root_vp);
    CHECK(file_vp != 0);

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

    vfs_close(vfs);
}

/* ---------------------------------------------------------------------------
 * vfs_read test — write then read back, verify content and epoch isolation
 * --------------------------------------------------------------------------- */

static void test_read_basic(void) {
    vfs_t* vfs = vfs_open(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int nodeId = vfs_create(vfs, root_vp, "readtest.txt", 0);
    CHECK(nodeId > 0);
    int64_t file_vp = get_file_vp(&ctx->pool, root_vp);
    CHECK(file_vp != 0);

    /* Write data */
    const char* wdata = "Hello, VFS read test!";
    int wret = vfs_write(vfs, file_vp, wdata, 0, (int64_t)strlen(wdata), 0);
    CHECK_EQ(wret, (int)strlen(wdata));

    /* Read it back — vfs_read returns total bytes transferred (including zero-fill) */
    char rbuf[64];
    memset(rbuf, 0, sizeof(rbuf));
    int rret = vfs_read(vfs, file_vp, rbuf, 0, (int64_t)sizeof(rbuf) - 1, 0);
    CHECK_EQ(rret, (int)sizeof(rbuf) - 1);  /* all bytes transferred */
    CHECK_EQ(strncmp(rbuf, wdata, strlen(wdata)), 0);
    CHECK_EQ(strcmp(rbuf, wdata), 0);

    /* Read before any write → zero-filled */
    memset(rbuf, 0, sizeof(rbuf));
    rret = vfs_read(vfs, file_vp, rbuf, 100, 10, 0);
    CHECK_EQ(rret, 10);
    int all_zero = 1;
    for (int i = 0; i < 10; i++) { if (rbuf[i] != 0) { all_zero = 0; break; } }
    CHECK(all_zero);

    /* Cross-page read */
    memset(rbuf, 0, sizeof(rbuf));
    rret = vfs_read(vfs, file_vp, rbuf, 8180, 32, 0);
    CHECK_EQ(rret, 32);
    all_zero = 1;
    for (int i = 0; i < 32; i++) { if (rbuf[i] != 0) { all_zero = 0; break; } }
    CHECK(all_zero);  /* never written at offset 8180 in this test */

    vfs_close(vfs);
}

/* ---------------------------------------------------------------------------
 * vfs_write/read comprehensive tests
 * --------------------------------------------------------------------------- */

/* Write 200 bytes at offset 50 (cross-page), read back */
static void test_write_cross_page(void) {
    vfs_t* vfs = vfs_open(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int nodeId = vfs_create(vfs, root_vp, "cross.txt", 0);
    CHECK(nodeId > 0);
    int64_t file_vp = get_file_vp(&ctx->pool, root_vp);
    CHECK(file_vp != 0);

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

    vfs_close(vfs);
}

/* Same offset, same epoch: second write in-place (VersionPage count unchanged) */
static void test_write_in_place(void) {
    vfs_t* vfs = vfs_open(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int nodeId = vfs_create(vfs, root_vp, "inplace.txt", 0);
    CHECK(nodeId > 0);
    int64_t file_vp = get_file_vp(&ctx->pool, root_vp);
    CHECK(file_vp != 0);

    /* First write: creates a VersionPage */
    int ret = vfs_write(vfs, file_vp, "AAAA", 0, 4, 0);
    CHECK_EQ(ret, 4);

    /* Count VersionPages for page 0 after first write */
    uint8_t* pn0 = tree_resolve_page(ctx, file_vp, 0, 0);
    CHECK(pn0 != NULL);
    int64_t vp = vfs_atomic_load_i64((const int64_t*)(pn0 + PAGENODE_OFF_VERSIONROOT));
    int count_before = 0;
    int64_t walk = vp;
    while (walk != 0) {
        count_before++;
        uint8_t* vs = pool_resolve(&ctx->pool, walk);
        CHECK(vs != NULL);
        walk = vfs_rd8(vs, VERSIONPAGE_OFF_NEXTPTR);
    }
    CHECK_EQ(count_before, 1);  /* exactly 1 VersionPage */

    /* Second write at same offset, same epoch: in-place, no new VersionPage */
    ret = vfs_write(vfs, file_vp, "BBBB", 0, 4, 0);
    CHECK_EQ(ret, 4);

    /* Count VersionPages again — should still be 1 (in-place) */
    pn0 = tree_resolve_page(ctx, file_vp, 0, 0);
    CHECK(pn0 != NULL);
    vp = vfs_atomic_load_i64((const int64_t*)(pn0 + PAGENODE_OFF_VERSIONROOT));
    int count_after = 0;
    walk = vp;
    while (walk != 0) {
        count_after++;
        uint8_t* vs = pool_resolve(&ctx->pool, walk);
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

    vfs_close(vfs);
}

/* Same offset, new epoch: COW creates new VersionPage, old epoch returns old data */
static void test_write_cow_epoch(void) {
    vfs_t* vfs = vfs_open(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int nodeId = vfs_create(vfs, root_vp, "cow.txt", 0);
    CHECK(nodeId > 0);
    int64_t file_vp = get_file_vp(&ctx->pool, ctx->rootNodeOffset);
    CHECK(file_vp != 0);

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
    uint8_t* pn0 = tree_resolve_page(ctx, file_vp, 0, 0);
    CHECK(pn0 != NULL);
    int64_t vp = vfs_atomic_load_i64((const int64_t*)(pn0 + PAGENODE_OFF_VERSIONROOT));
    int count = 0;
    int64_t walk = vp;
    while (walk != 0) {
        count++;
        uint8_t* vs = pool_resolve(&ctx->pool, walk);
        CHECK(vs != NULL);
        walk = vfs_rd8(vs, VERSIONPAGE_OFF_NEXTPTR);
    }
    CHECK_EQ(count, 2);

    vfs_close(vfs);
}

/* Write 2000 pages → second FileContent segment, reads across boundary */
static void test_write_multi_segment(void) {
    vfs_t* vfs = vfs_open(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int nodeId = vfs_create(vfs, root_vp, "multi.txt", 0);
    CHECK(nodeId > 0);
    int64_t file_vp = get_file_vp(&ctx->pool, ctx->rootNodeOffset);
    CHECK(file_vp != 0);

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
    CHECK(ctx->seg_array_fc_vp != 0);
    (void)seg_size;

    vfs_close(vfs);
}

/* ---------------------------------------------------------------------------
 * Write to frozen epoch test — vfs_epoch_is_writable returns false
 * --------------------------------------------------------------------------- */

static void test_write_frozen_epoch(void) {
    vfs_t* vfs = vfs_open(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int nodeId = vfs_create(vfs, root_vp, "frozen.txt", 0);
    CHECK(nodeId > 0);
    int64_t file_vp = get_file_vp(&ctx->pool, root_vp);
    CHECK(file_vp != 0);

    /* Freeze epoch — write should fail */
    test_set_epoch_writable(0);

    int ret = vfs_write(vfs, file_vp, "DATA", 0, 4, 3);
    CHECK_EQ(ret, -1);  /* VFS_ERR_IO mapped to -1 */

    /* Unfreeze for cleanup */
    test_set_epoch_writable(1);

    vfs_close(vfs);
}

/* ---------------------------------------------------------------------------
 * mkdir basic: create a directory, verify it's in the parent's DirContent
 * chain and has the DirNode type.
 * --------------------------------------------------------------------------- */

static void test_mkdir_basic(void) {
    vfs_t* vfs = vfs_open(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int ret = vfs_mkdir(vfs, root_vp, "a", 0);
    CHECK_EQ(ret, VFS_OK);

    /* Verify entry exists in root's DirContent chain */
    int64_t head = vfs_rd8_s(pool_resolve(&ctx->pool, root_vp),
                              DIRNODE_OFF_HEADPTR, ctx->page_size);
    CHECK(head != 0);

    uint32_t cc, ce;
    int64_t cp, np, nx;
    nodes_read_dircontent(pool_resolve(&ctx->pool, head),
                          &cc, &ce, &cp, &np, &nx, ctx->page_size);
    (void)cc; (void)ce; (void)np; (void)nx;
    CHECK(cp != 0);

    /* Verify child is a DirNode */
    uint8_t* child_slot = pool_resolve(&ctx->pool, cp);
    CHECK(child_slot != NULL);
    CHECK_EQ(vfs_rd2_s(child_slot, DIRNODE_OFF_TYPE, ctx->page_size),
             (int16_t)NODE_TYPE_DIR);

    vfs_close(vfs);
}

/* ---------------------------------------------------------------------------
 * mkdir duplicate: same name at same epoch → VFS_ERR_EXISTS.
 * --------------------------------------------------------------------------- */

static void test_mkdir_duplicate(void) {
    vfs_t* vfs = vfs_open(test_path, 8192);
    CHECK(vfs != NULL);
    int64_t root_vp = vfs->ctx->rootNodeOffset;

    CHECK_EQ(vfs_mkdir(vfs, root_vp, "dup", 0), VFS_OK);
    CHECK_EQ(vfs_mkdir(vfs, root_vp, "dup", 0), VFS_ERR_EXISTS);

    vfs_close(vfs);
}

/* ---------------------------------------------------------------------------
 * rmdir empty: mkdir + create file → rmdir fails with NOTEMPTY,
 * then delete file → rmdir succeeds.
 * --------------------------------------------------------------------------- */

static void test_rmdir_empty(void) {
    vfs_t* vfs = vfs_open(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    CHECK_EQ(vfs_mkdir(vfs, root_vp, "sub", 0), VFS_OK);

    /* Resolve subdir VirtualPtr */
    int64_t subdir_vp = 0;
    {
        uint8_t* rs = pool_resolve(&ctx->pool, root_vp);
        CHECK(rs != NULL);
        int64_t h = vfs_rd8_s(rs, DIRNODE_OFF_HEADPTR, ctx->page_size);
        CHECK(h != 0);
        uint32_t cc, ce;
        int64_t cp, np, nx;
        nodes_read_dircontent(pool_resolve(&ctx->pool, h),
                              &cc, &ce, &cp, &np, &nx, ctx->page_size);
        (void)cc; (void)ce; (void)np; (void)nx;
        subdir_vp = cp;
    }
    CHECK(subdir_vp != 0);

    /* Create file inside subdir */
    CHECK(vfs_create(vfs, subdir_vp, "f.txt", 0) > 0);

    /* rmdir should fail — directory not empty */
    CHECK_EQ(vfs_rmdir(vfs, root_vp, "sub", 0), VFS_ERR_NOTEMPTY);

    /* Delete the file inside subdir */
    CHECK_EQ(vfs_delete(vfs, subdir_vp, "f.txt", 0), VFS_OK);

    /* Now rmdir should succeed */
    CHECK_EQ(vfs_rmdir(vfs, root_vp, "sub", 0), VFS_OK);

    vfs_close(vfs);
}

/* ---------------------------------------------------------------------------
 * rmdir notdir: rmdir on a file (not a directory) → VFS_ERR_NOTDIR.
 * --------------------------------------------------------------------------- */

static void test_rmdir_notdir(void) {
    vfs_t* vfs = vfs_open(test_path, 8192);
    CHECK(vfs != NULL);
    int64_t root_vp = vfs->ctx->rootNodeOffset;

    CHECK(vfs_create(vfs, root_vp, "not_a_dir.txt", 0) > 0);
    CHECK_EQ(vfs_rmdir(vfs, root_vp, "not_a_dir.txt", 0), VFS_ERR_NOTDIR);

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

    /* Clean up */
    unlink(test_path);

    printf("test_tree: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
