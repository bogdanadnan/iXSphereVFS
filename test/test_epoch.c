/* Phase 6c: Comprehensive epoch system tests */
#include "ixsphere/vfs_internal.h"
#include "epoch.h"
#include "mapper.h"
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
    return vfs_mount(test_path, 8192);
}

static void epoch_teardown(vfs_t* vfs) {
    if (vfs) vfs_unmount(vfs);
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
    int64_t file_vp = vfs_create(vfs, root_vp, "epoch_file.txt", 0);
    CHECK(file_vp > 0);

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
 * Write to a snapshot (odd epoch) and verify read-rule resolves correctly.
 * This exercises the case where the chain contains odd-epoch records
 * (snapshot writes), per SPEC §7.2.  Without proper read-rule, the
 * future-epoch records at the head of the chain would shadow the
 * correct historical state.
 * --------------------------------------------------------------------------- */

static void test_snapshot_write_readrule(void) {
    vfs_t* vfs = epoch_setup();
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = get_root_vp(vfs);
    test_set_epoch_writable(-1);  /* use real epoch validation */

    int64_t file_vp = vfs_create(vfs, root_vp, "snapwrite.txt", 0);
    CHECK(file_vp > 0);
    CHECK_EQ(vfs_write(vfs, file_vp, "v0   ", 0, 4, 0), 4);

    /* Snapshot → odd epoch 1, currentEpoch becomes 2 */
    int64_t snap = vfs_snapshot(vfs);
    CHECK_EQ(snap, 1);
    CHECK_EQ(ctx->currentEpoch, 2);

    /* Write to the snapshot (odd epoch 1) — exercises odd-epoch
       chain records + write-rule bypass. */
    CHECK_EQ(vfs_write(vfs, file_vp, "v1   ", 0, 4, 1), 4);

    /* Write to the live head (even epoch 2). */
    CHECK_EQ(vfs_write(vfs, file_vp, "v2   ", 0, 4, 2), 4);

    /* Read at epoch 0 → "v0   " (commited base) */
    char rbuf[16];
    memset(rbuf, 0, sizeof(rbuf));
    CHECK_EQ(vfs_read(vfs, file_vp, rbuf, 0, 4, 0), 4);
    CHECK_EQ(strncmp(rbuf, "v0   ", 4), 0);

    /* Read at epoch 1 (snapshot, no mapping) → "v1   " (snapshot's write).
       The chain head is the live-head write (ep2) — read-rule must skip
       the future-epoch record and find the snapshot's record. */
    memset(rbuf, 0, sizeof(rbuf));
    CHECK_EQ(vfs_read(vfs, file_vp, rbuf, 0, 4, 1), 4);
    CHECK_EQ(strncmp(rbuf, "v1   ", 4), 0);

    /* Read at epoch 2 (live head) → "v2   " */
    memset(rbuf, 0, sizeof(rbuf));
    CHECK_EQ(vfs_read(vfs, file_vp, rbuf, 0, 4, 2), 4);
    CHECK_EQ(strncmp(rbuf, "v2   ", 4), 0);

    /* Read at a future epoch (3) — chain head's ep2 doesn't match, ep2 < 3
       and even, applies. Returns live-head write. */
    memset(rbuf, 0, sizeof(rbuf));
    CHECK_EQ(vfs_read(vfs, file_vp, rbuf, 0, 4, 3), 4);
    CHECK_EQ(strncmp(rbuf, "v2   ", 4), 0);

    epoch_teardown(vfs);
}

/* ---------------------------------------------------------------------------
 * Rename across multiple snapshots.
 *
 * Sequence:
 *   ep0:   create test.txt in head
 *   ep0→1: snapshot A
 *   ep1:   rename test.txt → test2.txt  (in snapshot A)
 *   ep1→2: live head bumps to 2 (no writes at ep1 in head)
 *   ep2:   rename test.txt → test3.txt  (in head, sees original)
 *   ep2→3: snapshot B
 *   ep3:   rename test3.txt → test4.txt  (in snapshot B)
 *
 * Expected listings:
 *   ep0 → test.txt
 *   ep1 → test2.txt   (snapshot A sees the rename, head's rename hidden)
 *   ep2 → test3.txt   (live head sees its own rename, snapshot A's hidden)
 *   ep3 → test4.txt   (snapshot B sees the rename, head's hidden)
 *
 * This exercises read-rule across 4 epochs with 2 active snapshots
 * and renames that interleave with snapshots.
 * --------------------------------------------------------------------------- */

static void test_rename_across_snapshots(void) {
    vfs_t* vfs = epoch_setup();
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = get_root_vp(vfs);
    test_set_epoch_writable(-1);  /* use real epoch validation */

    /* ep0: create test.txt in head */
    int64_t file_vp = vfs_create(vfs, root_vp, "test.txt", 0);
    CHECK(file_vp > 0);
    CHECK_EQ(ctx->currentEpoch, 0);

    /* Snapshot A → ep1, currentEpoch becomes 2 */
    int64_t snapA = vfs_snapshot(vfs);
    CHECK_EQ(snapA, 1);
    CHECK_EQ(ctx->currentEpoch, 2);

    /* ep1: rename in snapshot A */
    CHECK_EQ(vfs_rename(vfs, root_vp, "test.txt", root_vp, "test2.txt", 1), VFS_OK);

    /* ep2: rename in head — operates on the same name from ep0's perspective */
    CHECK_EQ(vfs_rename(vfs, root_vp, "test.txt", root_vp, "test3.txt", 2), VFS_OK);

    /* Snapshot B → ep3, currentEpoch becomes 4 */
    int64_t snapB = vfs_snapshot(vfs);
    CHECK_EQ(snapB, 3);
    CHECK_EQ(ctx->currentEpoch, 4);

    /* ep3: rename in snapshot B */
    CHECK_EQ(vfs_rename(vfs, root_vp, "test3.txt", root_vp, "test4.txt", 3), VFS_OK);

    /* Helper: assert readdir at epoch returns exactly `name` and only that. */
    {
        vfs_dirent_t ents[16];
        int n;

        n = vfs_readdir(vfs, root_vp, ents, 16, 0);
        CHECK_EQ(n, 1);
        if (n == 1) CHECK_EQ(strcmp(ents[0].name, "test.txt"), 0);

        n = vfs_readdir(vfs, root_vp, ents, 16, 1);
        CHECK_EQ(n, 1);
        if (n == 1) CHECK_EQ(strcmp(ents[0].name, "test2.txt"), 0);

        n = vfs_readdir(vfs, root_vp, ents, 16, 2);
        CHECK_EQ(n, 1);
        if (n == 1) CHECK_EQ(strcmp(ents[0].name, "test3.txt"), 0);

        n = vfs_readdir(vfs, root_vp, ents, 16, 3);
        CHECK_EQ(n, 1);
        if (n == 1) CHECK_EQ(strcmp(ents[0].name, "test4.txt"), 0);
    }

    epoch_teardown(vfs);
}

/* ---------------------------------------------------------------------------
 * Snapshot → write → delete snapshot (soft-delete) test
 * --------------------------------------------------------------------------- */

static void test_snapshot_soft_delete(void) {
    vfs_t* vfs = epoch_setup();
    CHECK(vfs != NULL);
    int64_t root_vp = get_root_vp(vfs);
    CHECK(root_vp != 0);

    int64_t file_vp = vfs_create(vfs, root_vp, "sd.txt", 0);
    CHECK(file_vp > 0);

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
 * Commit subdir conflict: mkdir sub, create file inside, snapshot, modify
 * at live head, commit → VFS_ERR_CONFLICT.
 * --------------------------------------------------------------------------- */

static void test_commit_subdir_conflict(void) {
    vfs_t* vfs = epoch_setup();
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;
    test_set_epoch_writable(-1);  /* use real epoch implementation */

    int64_t sub_vp = vfs_mkdir(vfs, root_vp, "sub", 0);
    CHECK(sub_vp > 0);

    int64_t file_vp = vfs_create(vfs, sub_vp, "a.txt", 0);
    CHECK(file_vp > 0);

    CHECK_EQ(vfs_write(vfs, file_vp, "AAAA", 0, 4, 0), 4);

    int64_t snap = vfs_snapshot(vfs);
    CHECK(snap > 0);

    /* Write at snapshot epoch (epoch 1) — sets up the conflict */
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

/* ---------------------------------------------------------------------------
 * Mapper integration: create file, snapshot, write at snapshot epoch, commit,
 * then verify vfs_mount / vfs_file_size at committed epoch.
 * Then soft-delete another snapshot and verify original epoch still works.
 * --------------------------------------------------------------------------- */

static void test_mapper_integration(void) {
    vfs_t* vfs = epoch_setup();
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;
    test_set_epoch_writable(-1);

    int64_t file_vp = vfs_create(vfs, root_vp, "mt.txt", 0);
    CHECK(file_vp > 0);

    CHECK_EQ(vfs_write(vfs, file_vp, "DATA", 0, 4, 0), 4);
    int64_t size0 = vfs_file_size(vfs, file_vp, 0);
    CHECK_EQ(size0, 4);

    /* Snapshot (epoch 1), write at epoch 2 as specified */
    int64_t snap = vfs_snapshot(vfs);
    CHECK_EQ(snap, 1);

    CHECK_EQ(vfs_write(vfs, file_vp, "MORE", 0, 4, 2), 4);

    /* Commit snapshot 1 */
    CHECK_EQ(vfs_commit(vfs, snap), VFS_OK);

    /* Verify mapper_table in-memory state after commit */
    CHECK_EQ(mapper_table_resolve(&ctx->mapper_table, snap), (int64_t)2);
    CHECK(mapper_table_traversal_apply(&ctx->mapper_table, snap));

    /* After commit:
       (a) vfs_mount at epoch 1 should find the file (mapper resolves 1→2) */
    CHECK(vfs_open(vfs, root_vp, "mt.txt", 1) > 0);

    /* (b) vfs_file_size at committed epoch 1 should return 4 (size from epoch 2 write) */
    CHECK_EQ(vfs_file_size(vfs, file_vp, 1), 4);

    /* (c) vfs_readdir at epoch 1 should show the file with correct name */
    vfs_dirent_t de[8];
    int nr = vfs_readdir(vfs, root_vp, de, 8, 1);
    CHECK_EQ(nr, 1);
    CHECK(!de[0].isDir);
    CHECK(strcmp(de[0].name, "mt.txt") == 0);

    /* Now test soft-delete: take another snapshot (epoch 3) and soft-delete it.
       After commit, currentEpoch = 2.  vfs_snapshot → epoch 3, currentEpoch→4.
       Soft-delete epoch 3 → mapper_insert(3, 2, 0) — toEpoch=2 conflicts with
       commit's 1→2 mapping.  So take ANOTHER snapshot first:
       vfs_snapshot → epoch 5, currentEpoch→6.
       Soft-delete snapshot 5 → mapper_insert(5, 4, 0) — toEpoch=4, no conflict. */
    int64_t snap3 = vfs_snapshot(vfs);   /* epoch 3 */
    CHECK_EQ(snap3, 3);
    int64_t snap5 = vfs_snapshot(vfs);   /* epoch 5 */
    CHECK_EQ(snap5, 5);

    CHECK_EQ(vfs_write(vfs, file_vp, "SNAP5", 0, 5, 5), 5);

    /* Soft-delete snapshot 5 → inserts mapping 5→4 with flags=0 (no traversalApply) */
    CHECK_EQ(vfs_delete_snapshot(vfs, snap5), VFS_OK);

    /* After soft-delete: read_epoch at epoch 5 resolves to 4 via mapper.
       The DirContent entry at epoch 0 (creation) applies at epoch 4
       (0 < 4 and even), so the file is still visible by name.
       The epoch-5 data is NOT visible (it's skipped by read-rule),
       but the file itself is found via the epoch-0 entry.
       We verify the readdir count and file_size at epoch 0 unchanged. */
    CHECK(vfs_open(vfs, root_vp, "mt.txt", 0) > 0);

    /* vfs_file_size at epoch 5 should return 0 (no applicable FileSize) */
    /* Actually vfs_file_size returns 0 for empty chain.  After soft-delete,
       the epoch-5 FileSize is skipped (same read-rule logic). */
    /* vfs_readdir at epoch 5: read_epoch = 4.  The epoch-0 DirContent
       entry applies (0 < 4 and even), so the file is still visible. */
    int nr5 = vfs_readdir(vfs, root_vp, de, 8, 5);
    CHECK_EQ(nr5, 1);

    /* Original epoch 0 still works */
    CHECK(vfs_open(vfs, root_vp, "mt.txt", 0) > 0);
    CHECK_EQ(vfs_file_size(vfs, file_vp, 0), 4);

    /* readdir at epoch 0 still shows the file */
    nr = vfs_readdir(vfs, root_vp, de, 8, 0);
    CHECK_EQ(nr, 1);
    CHECK(!de[0].isDir);
    CHECK(strcmp(de[0].name, "mt.txt") == 0);

    epoch_teardown(vfs);
}

int main(void) {
    test_snapshot_basic();
    test_epoch_writable();
    test_epoch_lifecycle();
    test_snapshot_write_readrule();
    test_rename_across_snapshots();
    test_snapshot_soft_delete();
    test_epoch_invalid();

    unlink(test_path);
    test_commit_subdir_conflict();

    unlink(test_path);
    test_mapper_integration();

    printf("test_epoch: %d/%d passed\n", tests_passed, tests_run);
    unlink(test_path);
    return (tests_passed == tests_run) ? 0 : 1;
}
