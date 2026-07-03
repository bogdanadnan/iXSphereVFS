/* Phase 6c: Comprehensive epoch system tests */
#include "ixsphere/vfs_internal.h"
#include "epoch.h"
#include "mapper.h"
#include "touched.h"
#include "tree.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

static int tests_run = 0, tests_passed = 0;

#define CHECK(expr) do { \
    tests_run++; \
    if (expr) { tests_passed++; } \
    else { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } \
} while(0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

static const char* test_path = "/tmp/test_epoch_suite.tmp";

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------- */

static vfs_t* epoch_setup(void) {
    unlink(test_path);
    return vfs_open(test_path, 8192);
}

static void epoch_teardown(vfs_t* vfs) {
    if (vfs) vfs_close(vfs);
    unlink(test_path);
}

/* Get the root DirNode's VirtualPtr */
static int64_t get_root_vp(vfs_t* vfs) {
    return vfs->ctx ? vfs->ctx->rootNodeOffset : 0;
}

/* ---------------------------------------------------------------------------
 * vfs_snapshot tests
 * --------------------------------------------------------------------------- */

static void test_snapshot_basic(void) {
    vfs_t* vfs = epoch_setup();
    CHECK(vfs != NULL);
    CHECK_EQ(vfs->ctx->currentEpoch, 0);

    /* First snapshot */
    int64_t snap1 = vfs_snapshot(vfs);
    CHECK_EQ(snap1, 1);    /* odd: 0+1 */
    CHECK_EQ(vfs->ctx->currentEpoch, 2);

    /* Second snapshot */
    int64_t snap2 = vfs_snapshot(vfs);
    CHECK_EQ(snap2, 3);    /* odd: 2+1 */
    CHECK_EQ(vfs->ctx->currentEpoch, 4);

    /* Snapshot at NULL vfs returns -1 */
    CHECK_EQ(vfs_snapshot(NULL), -1);

    epoch_teardown(vfs);
}

/* ---------------------------------------------------------------------------
 * vfs_epoch_is_writable tests
 * --------------------------------------------------------------------------- */

static void test_epoch_writable(void) {
    vfs_t* vfs = epoch_setup();
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;

    /* Default override (test_set_epoch_writable=1): all writable */
    CHECK(vfs_epoch_is_writable(ctx, 0));
    CHECK(vfs_epoch_is_writable(ctx, 2));
    CHECK(vfs_epoch_is_writable(ctx, 1));

    /* Freeze: none writable */
    test_set_epoch_writable(0);
    CHECK(!vfs_epoch_is_writable(ctx, 0));
    CHECK(!vfs_epoch_is_writable(ctx, 5));

    /* Restore all-writable */
    test_set_epoch_writable(1);
    CHECK(vfs_epoch_is_writable(ctx, 0));

    epoch_teardown(vfs);
}

/* ---------------------------------------------------------------------------
 * Full epoch lifecycle: snapshot, write, commit/revert
 * --------------------------------------------------------------------------- */

static void test_epoch_lifecycle(void) {
    vfs_t* vfs = epoch_setup();
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = get_root_vp(vfs);
    CHECK(root_vp != 0);

    /* Create a file and write at epoch 0 */
    int nodeId = vfs_create(vfs, root_vp, "epoch_file.txt", 0);
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
                              &cc, &ce, &cp, &np, &nx, VFS_PAGE_SIZE);
        (void)cc; (void)ce; (void)np; (void)nx;
        file_vp = cp;
    }
    CHECK(file_vp != 0);

    /* Write at epoch 0 */
    CHECK_EQ(vfs_write(vfs, file_vp, "AAAA", 0, 4, 0), 4);

    /* Snapshot → epoch 1 (odd) */
    int64_t snap_epoch = vfs_snapshot(vfs);
    CHECK_EQ(snap_epoch, 1);
    CHECK_EQ(ctx->currentEpoch, 2);

    /* Write at epoch 2 (live head after snapshot) */
    CHECK_EQ(vfs_write(vfs, file_vp, "BBBB", 0, 4, 2), 4);

    /* Read at epoch 0 → "AAAA" */
    char rbuf[16];
    CHECK_EQ(vfs_read(vfs, file_vp, rbuf, 0, 4, 0), 4);
    CHECK_EQ(strncmp(rbuf, "AAAA", 4), 0);

    /* Read at epoch 1 (snapshot, uses mapper resolve) → "AAAA" (no mapping yet) */
    memset(rbuf, 0, sizeof(rbuf));
    CHECK_EQ(vfs_read(vfs, file_vp, rbuf, 0, 4, 1), 4);
    CHECK_EQ(strncmp(rbuf, "AAAA", 4), 0);

    /* Read at epoch 2 → "BBBB" */
    memset(rbuf, 0, sizeof(rbuf));
    CHECK_EQ(vfs_read(vfs, file_vp, rbuf, 0, 4, 2), 4);
    CHECK_EQ(strncmp(rbuf, "BBBB", 4), 0);

    /* Commit snapshot 1 */
    int ret = vfs_commit(vfs, snap_epoch);
    CHECK_EQ(ret, VFS_OK);

    /* After commit, reading at epoch 1 should still show old data (mapper forwards
       epoch 1 → 2, but read-rule processes the remapped epoch) */
    /* Actually: mapper resolves 1→2 (toEpoch=2), and traversalApply=true means
       the VersionPage at epoch 2 gets remapped to epoch 2 for read-rule checking.
       Reading at epoch 0 still shows "AAAA" */
    memset(rbuf, 0, sizeof(rbuf));
    CHECK_EQ(vfs_read(vfs, file_vp, rbuf, 0, 4, 0), 4);
    CHECK_EQ(strncmp(rbuf, "AAAA", 4), 0);

    epoch_teardown(vfs);
}

/* ---------------------------------------------------------------------------
 * Snapshot → write → delete snapshot (soft-delete) test
 * --------------------------------------------------------------------------- */

static void test_snapshot_soft_delete(void) {
    vfs_t* vfs = epoch_setup();
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = get_root_vp(vfs);
    CHECK(root_vp != 0);

    int nodeId = vfs_create(vfs, root_vp, "sd.txt", 0);
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
        file_vp = cp;
    }
    CHECK(file_vp != 0);

    /* Write at epoch 0 */
    CHECK_EQ(vfs_write(vfs, file_vp, "XXXX", 0, 4, 0), 4);

    /* Snapshot */
    int64_t snap = vfs_snapshot(vfs);
    CHECK_EQ(snap, 1);

    /* Write at epoch 2 */
    CHECK_EQ(vfs_write(vfs, file_vp, "YYYY", 0, 4, 2), 4);

    /* Delete snapshot (soft-delete) */
    int ret = vfs_delete_snapshot(vfs, snap);
    CHECK_EQ(ret, VFS_OK);

    /* After soft-delete, reading at epoch 1 should fall through to epoch 0 data */
    char rbuf[16];
    memset(rbuf, 0, sizeof(rbuf));
    CHECK_EQ(vfs_read(vfs, file_vp, rbuf, 0, 4, 1), 4);
    CHECK_EQ(strncmp(rbuf, "XXXX", 4), 0);

    epoch_teardown(vfs);
}

/* ---------------------------------------------------------------------------
 * Invalid inputs
 * --------------------------------------------------------------------------- */

static void test_epoch_invalid(void) {
    vfs_t* vfs = epoch_setup();
    CHECK(vfs != NULL);

    /* Commit even epoch → VFS_ERR_IO */
    CHECK_EQ(vfs_commit(vfs, 2), VFS_ERR_IO);

    /* Delete-snapshot even epoch → VFS_ERR_IO */
    CHECK_EQ(vfs_delete_snapshot(vfs, 2), VFS_ERR_IO);

    /* Commit non-existent (active) odd epoch → succeeds (no touched files, no conflict)
       because odd epochs with no mapper entry are treated as active snapshots. */
    CHECK_EQ(vfs_commit(vfs, 99), VFS_OK);

    /* Snapshot on NULL vfs → -1 */
    CHECK_EQ(vfs_snapshot(NULL), -1);

    epoch_teardown(vfs);
}

/* ---------------------------------------------------------------------------
 * TouchedFile tests
 * --------------------------------------------------------------------------- */

static void test_touchedfile_chain(void) {
    vfs_t* vfs = epoch_setup();
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;

    /* Add entries */
    CHECK_EQ(touchedfile_add(&ctx->pool, &ctx->touchedFilesPtr, 1, 100), VFS_OK);
    CHECK_EQ(touchedfile_add(&ctx->pool, &ctx->touchedFilesPtr, 1, 200), VFS_OK);
    /* Duplicate (epoch=1, nodeId=100) → idempotent */
    CHECK_EQ(touchedfile_add(&ctx->pool, &ctx->touchedFilesPtr, 1, 100), VFS_OK);
    /* Different epoch */
    CHECK_EQ(touchedfile_add(&ctx->pool, &ctx->touchedFilesPtr, 3, 300), VFS_OK);

    /* Collect epoch 1 → should have [100, 200] */
    uint32_t ids[8];
    int n = touchedfile_collect(&ctx->pool, ctx->touchedFilesPtr, 1, ids, 8);
    CHECK_EQ(n, 2);
    int found_a = 0, found_b = 0;
    for (int i = 0; i < n; i++) {
        if (ids[i] == 100) found_a = 1;
        if (ids[i] == 200) found_b = 1;
    }
    CHECK(found_a && found_b);

    /* Drop epoch 1 */
    touchedfile_drop(&ctx->pool, &ctx->touchedFilesPtr, 1);

    /* After drop, collect epoch 1 → should return 0 */
    n = touchedfile_collect(&ctx->pool, ctx->touchedFilesPtr, 1, ids, 8);
    CHECK_EQ(n, 0);

    /* Epoch 3 entries still present */
    n = touchedfile_collect(&ctx->pool, ctx->touchedFilesPtr, 3, ids, 8);
    CHECK_EQ(n, 1);
    CHECK_EQ(ids[0], 300);

    epoch_teardown(vfs);
}

/* ---------------------------------------------------------------------------
 * Commit subdir conflict: mkdir sub, create file inside, snapshot, modify
 * at live head, commit → VFS_ERR_CONFLICT.
 * --------------------------------------------------------------------------- */

static void test_commit_subdir_conflict(void) {
    vfs_t* vfs = epoch_setup();
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;
    test_set_epoch_writable(-1);  /* use real epoch implementation */

    CHECK_EQ(vfs_mkdir(vfs, root_vp, "sub", 0), VFS_OK);

    /* Resolve subdir VirtualPtr via pool (vfs_open_file returns nodeId, not VP) */
    int64_t sub_vp = 0;
    {
        uint8_t* rs = pool_resolve(&ctx->pool, root_vp);
        CHECK(rs != NULL);
        int64_t h = vfs_rd8_s(rs, DIRNODE_OFF_HEADPTR, ctx->page_size);
        int64_t w = h;
        while (w != 0) {
            uint8_t* dc = pool_resolve(&ctx->pool, w);
            CHECK(dc != NULL);
            uint32_t cc, ce;
            int64_t cp, np, nx;
            nodes_read_dircontent(dc, &cc, &ce, &cp, &np, &nx, ctx->page_size);
            (void)cc; (void)ce;
            char en[64];
            int nl = nodes_read_name(&ctx->pool, np, en, (int)sizeof(en));
            if (nl > 0 && strcmp(en, "sub") == 0) {
                sub_vp = cp; break;
            }
            w = nx;
        }
    }
    CHECK(sub_vp != 0);

    int nid = vfs_create(vfs, sub_vp, "a.txt", 0);
    CHECK(nid > 0);

    /* Resolve file VirtualPtr from subdir's DirContent chain */
    int64_t file_vp = 0;
    {
        uint8_t* rs = pool_resolve(&ctx->pool, sub_vp);
        CHECK(rs != NULL);
        int64_t h = vfs_rd8_s(rs, DIRNODE_OFF_HEADPTR, ctx->page_size);
        int64_t w = h;
        while (w != 0) {
            uint8_t* dc = pool_resolve(&ctx->pool, w);
            CHECK(dc != NULL);
            uint32_t cc, ce;
            int64_t cp, np, nx;
            nodes_read_dircontent(dc, &cc, &ce, &cp, &np, &nx, ctx->page_size);
            (void)cc; (void)ce;
            char en[64];
            int nl = nodes_read_name(&ctx->pool, np, en, (int)sizeof(en));
            if (nl > 0 && strcmp(en, "a.txt") == 0) {
                file_vp = cp; break;
            }
            w = nx;
        }
    }
    CHECK(file_vp != 0);

    CHECK_EQ(vfs_write(vfs, file_vp, "AAAA", 0, 4, 0), 4);

    int64_t snap = vfs_snapshot(vfs);
    CHECK(snap > 0);

    /* Write at snapshot epoch (epoch 1) — creates TouchedFile for epoch 1 */
    CHECK_EQ(vfs_write(vfs, file_vp, "SNAP", 0, 4, 1), 4);

    /* Write at live head (epoch 2) — creates conflict (same page) */
    CHECK_EQ(vfs_write(vfs, file_vp, "BBBB", 0, 4, 2), 4);

    int commit_ret = vfs_commit(vfs, snap);
    if (commit_ret != VFS_ERR_CONFLICT) {
        printf("  DEBUG: commit returned %d (expected %d)\n",
               commit_ret, VFS_ERR_CONFLICT);
    }
    CHECK_EQ(commit_ret, VFS_ERR_CONFLICT);

    epoch_teardown(vfs);
}

int main(void) {
    test_snapshot_basic();
    test_epoch_writable();
    test_epoch_lifecycle();
    test_snapshot_soft_delete();
    test_epoch_invalid();
    test_touchedfile_chain();

    unlink(test_path);
    test_commit_subdir_conflict();

    printf("test_epoch: %d/%d passed\n", tests_passed, tests_run);
    unlink(test_path);
    return (tests_passed == tests_run) ? 0 : 1;
}
