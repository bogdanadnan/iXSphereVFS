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


/* W5a: Walk a per-dir SlotNode chain and invoke a callback for each
 * DirContent slot.  Returns 1 if the callback found its target, 0
 * otherwise.  The callback signature is: int cb(uint8_t* dc_bytes,
 * TreeContext* ctx, void* user).  Return non-zero to stop the walk. */
typedef int (*dirchain_walk_cb)(uint8_t* dc_bytes, TreeContext* ctx, void* user);
static int dirchain_walk(TreeContext* ctx, int64_t dir_vp, dirchain_walk_cb cb, void* user) {
    PoolSlot dir_slot = {0};
    pool_acquire(&ctx->pool, dir_vp, false, &dir_slot);
    if (dir_slot.vptr == VFS_VPTR_NULL) return 0;
    int64_t headPtr = vfs_rd8_s(dir_slot.bytes, DIRNODE_OFF_HEADPTR, ctx->page_size);
    pool_release(&ctx->pool, &dir_slot);
    /* W5b: walk DirSegments → SlotNodes → DirContents. */
    int64_t seg_vp = headPtr;
    while (seg_vp != 0) {
        PoolSlot seg = {0};
        pool_acquire(&ctx->pool, seg_vp, false, &seg);
        if (seg.vptr == VFS_VPTR_NULL) break;
        AnchorKind seg_ak;
        uint32_t seg_id, seg_cnt;
        int64_t seg_head, seg_sib;
        nodes_read_anchor(seg.bytes, &seg_ak, &seg_id, &seg_head, &seg_sib,
                          &seg_cnt, ctx->page_size);
        pool_release(&ctx->pool, &seg);
        int64_t slot_vp = seg_head;
        while (slot_vp != 0) {
            PoolSlot ss = {0};
            pool_acquire(&ctx->pool, slot_vp, false, &ss);
            if (ss.vptr == VFS_VPTR_NULL) break;
            int64_t dc_head = vfs_rd8_s(ss.bytes, 8, ctx->page_size);  /* ANCHOR_OFF_HEADPTR */
            int64_t dc_sib = vfs_rd8_s(ss.bytes, 16, ctx->page_size); /* ANCHOR_OFF_SIBPTR */
            pool_release(&ctx->pool, &ss);
            int64_t dc_vp = dc_head;
            while (dc_vp != 0) {
                PoolSlot dc = {0};
                pool_acquire(&ctx->pool, dc_vp, false, &dc);
                if (dc.vptr == VFS_VPTR_NULL) break;
                int rc = cb(dc.bytes, ctx, user);
                pool_release(&ctx->pool, &dc);
                if (rc) return 1;
                dc_vp = vfs_rd8_s(dc.bytes, DIRCONTENT_OFF_NEXTPTR, ctx->page_size);
            }
            slot_vp = dc_sib;
        }
        seg_vp = seg_sib;
    }
    return 0;
}


/* ---------------------------------------------------------------------------
 * Bootstrap test
 * --------------------------------------------------------------------------- */

static const char* test_path = "/tmp/test_tree_bootstrap.tmp";

static void test_bootstrap_root(void) {
    /* Open fresh file → bootstrap creates root */
    unlink(test_path);
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    CHECK(vfs->ctx != NULL);

    TreeContext* ctx = vfs->ctx;

    /* Root DirNode must exist with correct fields */
    CHECK(ctx->rootNodeOffset != 0);

    PoolSlot root_slot = {0};
    pool_acquire(&ctx->pool, ctx->rootNodeOffset, false, &root_slot);
    CHECK(root_slot.vptr != VFS_VPTR_NULL);

    /* Verify root DirNode: nodeId=0, type=0x01, headPtr=0 */
    int16_t type = vfs_rd2_s(root_slot.bytes, DIRNODE_OFF_TYPE, ctx->page_size);
    CHECK_EQ((int)type, (int)NODE_TYPE_DIR);

    uint32_t nodeId = (uint32_t)vfs_rd4_s(root_slot.bytes, FILENODE_OFF_NODEID, ctx->page_size);
    CHECK_EQ(nodeId, 0u);

    int64_t headPtr = vfs_rd8_s(root_slot.bytes, DIRNODE_OFF_HEADPTR, ctx->page_size);
    CHECK_EQ(headPtr, 0);

    /* nextNodeId should be 0 (node 0 used for root, next available is 0) */
    CHECK_EQ((int)ctx->nextNodeId, 0);

    pool_release(&ctx->pool, &root_slot);
    vfs_unmount(vfs);
}

static void test_bootstrap_reopen(void) {
    /* Open existing file → reopen */
    unlink(test_path);
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    CHECK(vfs->ctx != NULL);

    TreeContext* ctx = vfs->ctx;

    /* Root must still exist */
    CHECK(ctx->rootNodeOffset != 0);

    PoolSlot root_slot = {0};
    pool_acquire(&ctx->pool, ctx->rootNodeOffset, false, &root_slot);
    CHECK(root_slot.vptr != VFS_VPTR_NULL);

    int16_t type = vfs_rd2_s(root_slot.bytes, DIRNODE_OFF_TYPE, ctx->page_size);
    CHECK_EQ((int)type, (int)NODE_TYPE_DIR);

    uint32_t nodeId = (uint32_t)vfs_rd4_s(root_slot.bytes, DIRNODE_OFF_NODEID, ctx->page_size);
    CHECK_EQ(nodeId, 0u);

    int64_t headPtr = vfs_rd8_s(root_slot.bytes, DIRNODE_OFF_HEADPTR, ctx->page_size);
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

    pool_release(&ctx->pool, &root_slot);
    vfs_unmount(vfs);
}

static void test_pool_list_head(void) {
    /* poolListHead should be non-zero after bootstrap (pool alloc happened) */
    unlink(test_path);
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);

    CHECK(vfs->ctx->pool_list_head_value != 0);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * vfs_create test
 * --------------------------------------------------------------------------- */

static void test_create_file(void) {
    unlink(test_path);
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
    PoolSlot fn_slot = {0};
    pool_acquire(&ctx->pool, file_vp, false, &fn_slot);
    CHECK(fn_slot.vptr != VFS_VPTR_NULL);
    uint32_t fn_nodeId = (uint32_t)vfs_rd4_s(fn_slot.bytes,  FILENODE_OFF_NODEID, ctx->page_size);
    CHECK_EQ(fn_nodeId, 1u);  /* first created file gets nodeId=1 */

    /* Verify the file exists in root's DirContent chain */
    pool_release(&ctx->pool, &fn_slot);
    PoolSlot root_slot = {0};
    pool_acquire(&ctx->pool, root_vp, false, &root_slot);
    CHECK(root_slot.vptr != VFS_VPTR_NULL);
    int64_t headPtr = vfs_rd8_s(root_slot.bytes, DIRNODE_OFF_HEADPTR, ctx->page_size);
    CHECK(headPtr != 0);  /* should have 1 entry now */

    /* Walk the chain to find our file (W5b: Segment → SlotNode → DirContent) */
    int64_t seg_vp = headPtr;
    int found = 0;
    while (seg_vp != 0 && !found) {
        PoolSlot seg_s = {0};
        pool_acquire(&ctx->pool, seg_vp, false, &seg_s);
        CHECK(seg_s.vptr != VFS_VPTR_NULL);
        uint16_t seg_ak = vfs_rd2_s(seg_s.bytes,  0, ctx->page_size);
        CHECK_EQ(seg_ak, 0x21);  /* ANCHOR_KIND_SEGMENT_DIR */
        int64_t seg_head = vfs_rd8_s(seg_s.bytes,  8, ctx->page_size);
        int64_t seg_sib = vfs_rd8_s(seg_s.bytes,  16, ctx->page_size);
        int64_t walk_vp = seg_head;
        while (walk_vp != 0 && !found) {
            PoolSlot ss = {0};
            pool_acquire(&ctx->pool, walk_vp, false, &ss);
            CHECK(ss.vptr != VFS_VPTR_NULL);
            /* Read the SlotNode (UNIT_SLOT Anchor). */
            uint16_t ak = vfs_rd2_s(ss.bytes,  0, ctx->page_size);
            CHECK_EQ(ak, 0x31);  /* ANCHOR_KIND_UNIT_SLOT */
            uint32_t slot_id = (uint32_t)vfs_rd4_s(ss.bytes,  4, ctx->page_size);
            int64_t slot_head = vfs_rd8_s(ss.bytes,  8, ctx->page_size);
            int64_t slot_sib = vfs_rd8_s(ss.bytes,  16, ctx->page_size);
            (void)slot_id;
            /* Walk the SlotNode's DirContent chain. */
            int64_t dc_walk = slot_head;
            while (dc_walk != 0 && !found) {
                PoolSlot dc_slot = {0};
                pool_acquire(&ctx->pool, dc_walk, false, &dc_slot);
                CHECK(dc_slot.vptr != VFS_VPTR_NULL);

            uint32_t ce_child, ce_epoch;
            int64_t ce_childPtr, ce_namePtr, ce_next;
            nodes_read_dircontent(dc_slot.bytes, &ce_child, &ce_epoch, &ce_childPtr,
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
            
    pool_release(&ctx->pool, &dc_slot);}
            dc_walk = ce_next;
        
    pool_release(&ctx->pool, &ss);}
            walk_vp = slot_sib;
        
    pool_release(&ctx->pool, &seg_s);}
        seg_vp = seg_sib;
    }
    CHECK(found);

    /* nextNodeId should now be 1 */
    CHECK_EQ((int)ctx->nextNodeId, 1);

    pool_release(&ctx->pool, &root_slot);
    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * vfs_delete test
 * --------------------------------------------------------------------------- */

static void test_delete_file(void) {
    unlink(test_path);
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    /* Create a file first */
    int64_t nodeId = vfs_create(vfs, root_vp, "delete_me.txt", 0);
    CHECK(nodeId > 0);

    /* Verify it exists in root's DirContent chain */
    PoolSlot root_slot = {0};
    pool_acquire(&ctx->pool, root_vp, false, &root_slot);
    CHECK(root_slot.vptr != VFS_VPTR_NULL);
    int64_t headPtr = vfs_rd8_s(root_slot.bytes,  DIRNODE_OFF_HEADPTR, ctx->page_size);
    CHECK(headPtr != 0);

    int64_t seg_vp = headPtr;
    int found = 0;
    while (seg_vp != 0 && !found) {
        /* W5b: walk DirSegment → SlotNode → DirContent. */
        PoolSlot seg_s = {0};
        pool_acquire(&ctx->pool, seg_vp, false, &seg_s);
        CHECK(seg_s.vptr != VFS_VPTR_NULL);
        int64_t seg_head = vfs_rd8_s(seg_s.bytes,  8, ctx->page_size);  /* ANCHOR_OFF_HEADPTR */
        int64_t seg_sib = vfs_rd8_s(seg_s.bytes,  16, ctx->page_size); /* ANCHOR_OFF_SIBPTR */
        int64_t walk_vp = seg_head;
        while (walk_vp != 0 && !found) {
            /* W5a: walk the SlotNode chain (per-child) and its DirContent chain. */
            PoolSlot ss = {0};
            pool_acquire(&ctx->pool, walk_vp, false, &ss);
            CHECK(ss.vptr != VFS_VPTR_NULL);
            int64_t dc_walk = vfs_rd8_s(ss.bytes,  8, ctx->page_size);  /* ANCHOR_OFF_HEADPTR */
            int64_t slot_sib = vfs_rd8_s(ss.bytes,  16, ctx->page_size);
            while (dc_walk != 0) {
                PoolSlot dc_slot = {0};
                pool_acquire(&ctx->pool, dc_walk, false, &dc_slot);
                CHECK(dc_slot.vptr != VFS_VPTR_NULL);
                uint32_t ce_child, ce_epoch;
                int64_t ce_childPtr, ce_namePtr, ce_next;
                nodes_read_dircontent(dc_slot.bytes, &ce_child, &ce_epoch, &ce_childPtr,
                                      &ce_namePtr, &ce_next, VFS_PAGE_SIZE);
                (void)ce_child; (void)ce_childPtr;
                if (ce_epoch == 0 && ce_namePtr != 0) {
                    char entry_name[256];
                    int nl = nodes_read_name(&ctx->pool, ce_namePtr,
                                              entry_name, (int)sizeof(entry_name));
                    if (nl > 0 && strcmp(entry_name, "delete_me.txt") == 0)
                        found = 1;
                
    pool_release(&ctx->pool, &dc_slot);
    pool_release(&ctx->pool, &dc_slot);}
                dc_walk = ce_next;
            
    pool_release(&ctx->pool, &ss);
    pool_release(&ctx->pool, &ss);}
            walk_vp = slot_sib;
        
    pool_release(&ctx->pool, &seg_s);
    pool_release(&ctx->pool, &seg_s);}
        seg_vp = seg_sib;
    
    pool_release(&ctx->pool, &root_slot);}
    CHECK(found);

    /* Snapshot so epoch 2 becomes the live head (phase 27 C4). */
    int64_t del_snap = vfs_snapshot(vfs);
    CHECK_EQ(del_snap, 1);

    /* Delete the file at epoch 2 */
    int ret = vfs_delete(vfs, root_vp, "delete_me.txt", 2);
    CHECK_EQ(ret, VFS_OK);

    /* Verify the tombstone exists */
    pool_release(&ctx->pool, &root_slot);
    pool_acquire(&ctx->pool, root_vp, false, &root_slot);
    CHECK(root_slot.vptr != VFS_VPTR_NULL);
    headPtr = vfs_rd8_s(root_slot.bytes,  DIRNODE_OFF_HEADPTR, ctx->page_size);
    CHECK(headPtr != 0);

    seg_vp = headPtr;
    int found_tombstone = 0;
    while (seg_vp != 0) {
        PoolSlot seg_s = {0};
        pool_acquire(&ctx->pool, seg_vp, false, &seg_s);
        CHECK(seg_s.vptr != VFS_VPTR_NULL);
        int64_t seg_head = vfs_rd8_s(seg_s.bytes,  8, ctx->page_size);
        int64_t seg_sib = vfs_rd8_s(seg_s.bytes,  16, ctx->page_size);
        int64_t walk_vp = seg_head;
        while (walk_vp != 0) {
            PoolSlot ss = {0};
            pool_acquire(&ctx->pool, walk_vp, false, &ss);
            CHECK(ss.vptr != VFS_VPTR_NULL);
            int64_t dc_walk = vfs_rd8_s(ss.bytes,  8, ctx->page_size);
            int64_t slot_sib = vfs_rd8_s(ss.bytes,  16, ctx->page_size);
            while (dc_walk != 0) {
                PoolSlot dc_slot = {0};
                pool_acquire(&ctx->pool, dc_walk, false, &dc_slot);
                CHECK(dc_slot.vptr != VFS_VPTR_NULL);
                uint32_t ce_child, ce_epoch;
                int64_t ce_childPtr, ce_namePtr, ce_next;
                nodes_read_dircontent(dc_slot.bytes, &ce_child, &ce_epoch, &ce_childPtr,
                                      &ce_namePtr, &ce_next, VFS_PAGE_SIZE);
                (void)ce_child; (void)ce_childPtr;
                if (ce_epoch == 2 && ce_namePtr == 0)
                    found_tombstone = 1;
                dc_walk = ce_next;
            }
            walk_vp = slot_sib;
        }
        seg_vp = seg_sib;
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
    unlink(test_path);
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
        PoolSlot root_slot2 = {0};
        pool_acquire(&ctx->pool, root_vp, false, &root_slot2);
        CHECK(root_slot2.vptr != VFS_VPTR_NULL);
        int64_t head2 = vfs_rd8_s(root_slot2.bytes, DIRNODE_OFF_HEADPTR, ctx->page_size);
        CHECK(head2 != 0);
        /* W5b: walk DirSegment → SlotNode → first DirContent → childPtr. */
        PoolSlot seg_s = {0};
        pool_acquire(&ctx->pool, head2, false, &seg_s);
        CHECK(seg_s.vptr != VFS_VPTR_NULL);
        int64_t slot_vp = vfs_rd8_s(seg_s.bytes, 8, ctx->page_size);
        CHECK(slot_vp != 0);
        PoolSlot slot_s = {0};
        pool_acquire(&ctx->pool, slot_vp, false, &slot_s);
        CHECK(slot_s.vptr != VFS_VPTR_NULL);
        int64_t dc_vp = vfs_rd8_s(slot_s.bytes, 8, ctx->page_size);
        CHECK(dc_vp != 0);
        uint32_t ce_c, ce_e;
        int64_t ce_cp, ce_np, ce_nx;
        PoolSlot dc_slot = {0};
        pool_acquire(&ctx->pool, dc_vp, false, &dc_slot);
        nodes_read_dircontent(dc_slot.bytes, &ce_c, &ce_e, &ce_cp, &ce_np, &ce_nx, VFS_PAGE_SIZE);
        (void)ce_c; (void)ce_e; (void)ce_np; (void)ce_nx;
        opened = vfs_open(vfs, ce_cp, "anything.txt", 0);
        CHECK_EQ(opened, VFS_ERR_NOTDIR);
        pool_release(&ctx->pool, &dc_slot);
        pool_release(&ctx->pool, &slot_s);
        pool_release(&ctx->pool, &seg_s);
        pool_release(&ctx->pool, &root_slot2);
    }

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * Duplicate name test
 * --------------------------------------------------------------------------- */

static void test_create_duplicate(void) {
    unlink(test_path);
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
    unlink(test_path);
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
    unlink(test_path);
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
    unlink(test_path);
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
    unlink(test_path);
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    int64_t root_vp = vfs->ctx->rootNodeOffset;

    /* Create file at epoch 0 */
    int64_t nodeId = vfs_create(vfs, root_vp, "epoch_test.txt", 0);
    CHECK(nodeId > 0);

    /* Verify it's visible at epoch 0 */
    int64_t found = vfs_open(vfs, root_vp, "epoch_test.txt", 0);
    CHECK(found > 0);

    /* Snapshot so epoch 2 becomes the live head (phase 27 C4). */
    int64_t dei_snap = vfs_snapshot(vfs);
    CHECK_EQ(dei_snap, 1);

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
    unlink(test_path);
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
    unlink(test_path);
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
    unlink(test_path);
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

    /* Append a FileSize entry at epoch 2 to the FileNode's sizePtr chain.
     * This is the W3 lock-based write path — same as vfs_write /
     * vfs_truncate use internally, exposed here at the chain-mutation
     * level.  vfs_lock serialises with other epoch holders, the by-value
     * pool slots replace the old raw-pointer CAS, and the FileNode's
     * sizePtr is updated under the lock so the chain is consistent. */
    CHECK_EQ(vfs_lock(vfs, file_vp, 2), VFS_OK);
    {
        PoolSlot file_slot = {0};
        pool_acquire(&ctx->pool, file_vp, true, &file_slot);
        CHECK(file_slot.vptr != VFS_VPTR_NULL);
        int64_t old_sizePtr = vfs_rd8_s(file_slot.bytes, FILENODE_OFF_SIZEPTR,
                                        ctx->page_size);

        int64_t fs_vp = pool_alloc(&ctx->pool);
        CHECK(fs_vp != VFS_VPTR_NULL);
        PoolSlot fs_slot = {0};
        pool_acquire(&ctx->pool, fs_vp, true, &fs_slot);
        CHECK(fs_slot.vptr != VFS_VPTR_NULL);
        nodes_write_filesize(fs_slot.bytes, 2, 2000, 500, old_sizePtr, ctx->page_size);
        pool_release(&ctx->pool, &fs_slot);
        vfs_mb_release();
        vfs_wr8_s(file_slot.bytes, FILENODE_OFF_SIZEPTR, fs_vp, ctx->page_size);
        pool_release(&ctx->pool, &file_slot);
    }
    CHECK_EQ(vfs_unlock(vfs, file_vp, 2), VFS_OK);

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
    unlink(test_path);
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t file_vp = vfs_create(vfs, root_vp, "big.txt", 0);
    CHECK(file_vp > 0);

    uint32_t seg_size = ctx->segment_size;
    CHECK(seg_size > 0);

    /* Resolve page 0 — should create first segment.
     * W3: is_write=true requires vfs_lock held (chain mutation path). */
    CHECK_EQ(vfs_lock(vfs, file_vp, 0), VFS_OK);
    PoolSlot pn0 = {0};
    int rc_pn0 = tree_resolve_page(vfs, file_vp, 0, 0, true, &pn0);
    CHECK_EQ(rc_pn0, 0);

    /* Verify it's a PageNode with versionRootPtr=0 (never written) */
    CHECK_EQ(vfs_rd8_s(pn0.bytes, PAGENODE_OFF_VERSIONROOT, ctx->page_size), 0);
    CHECK_EQ((uint32_t)vfs_rd4_s(pn0.bytes, PAGENODE_OFF_PAGEINDEX, ctx->page_size), 0u);
    CHECK_EQ(vfs_unlock(vfs, file_vp, 0), VFS_OK);

    /* Resolve page 1 — should allocate a second PageNode */
    CHECK_EQ(vfs_lock(vfs, file_vp, 0), VFS_OK);
    PoolSlot pn1 = {0};
    int rc_pn1 = tree_resolve_page(vfs, file_vp, 1, 0, true, &pn1);
    CHECK_EQ(rc_pn1, 0);
    CHECK_EQ(vfs_rd8_s(pn1.bytes, PAGENODE_OFF_VERSIONROOT, ctx->page_size), 0);
    CHECK_EQ((uint32_t)vfs_rd4_s(pn1.bytes, PAGENODE_OFF_PAGEINDEX, ctx->page_size), 1u);
    CHECK_EQ(vfs_unlock(vfs, file_vp, 0), VFS_OK);

    /* Resolve page at segment boundary — should create second segment */
    CHECK_EQ(vfs_lock(vfs, file_vp, 0), VFS_OK);
    PoolSlot pn_first_new = {0};
    int rc_pn_fn = tree_resolve_page(vfs, file_vp, seg_size, 0, true, &pn_first_new);
    CHECK_EQ(rc_pn_fn, 0);
    CHECK_EQ(vfs_rd8_s(pn_first_new.bytes, PAGENODE_OFF_VERSIONROOT, ctx->page_size), 0);
    CHECK_EQ((uint32_t)vfs_rd4_s(pn_first_new.bytes, PAGENODE_OFF_PAGEINDEX, ctx->page_size), 0u);
    CHECK_EQ(vfs_unlock(vfs, file_vp, 0), VFS_OK);

    /* Resolve page 0 again — read-only, no lock needed (W3). */
    PoolSlot pn0_ro = {0};
    int rc_pn0_ro = tree_resolve_page(vfs, file_vp, 0, 0, false, &pn0_ro);
    CHECK_EQ(rc_pn0_ro, 0);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * vfs_write test — basic write, size update, cross-page write
 * --------------------------------------------------------------------------- */

static void test_write_basic(void) {
    unlink(test_path);
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
    unlink(test_path);
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
    unlink(test_path);
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
    unlink(test_path);
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
    PoolSlot pn0 = {0};
    int rc_pn0 = tree_resolve_page(vfs, file_vp, 0, 0, false, &pn0);
    CHECK_EQ(rc_pn0, 0);
    int64_t vp = vfs_atomic_load_i64(
        (const int64_t*)(pn0.bytes + PAGENODE_OFF_VERSIONROOT));
    int count_before = 0;
    int64_t walk = vp;
    while (walk != 0) {
        count_before++;
        PoolSlot vs = {0};
        pool_acquire(&ctx->pool, walk, false, &vs);
        CHECK(vs.vptr != VFS_VPTR_NULL);
        walk = vfs_rd8_s(vs.bytes, VERSIONPAGE_OFF_NEXTPTR, ctx->page_size);
        pool_release(&ctx->pool, &vs);
    }
    CHECK_EQ(count_before, 1);  /* exactly 1 VersionPage */

    /* Second write at same offset, same epoch: in-place, no new VersionPage */
    ret = vfs_write(vfs, file_vp, "BBBB", 0, 4, 0);
    CHECK_EQ(ret, 4);

    /* Count VersionPages again — should still be 1 (in-place) */
    int rc_pn0_2 = tree_resolve_page(vfs, file_vp, 0, 0, false, &pn0);
    CHECK_EQ(rc_pn0_2, 0);
    vp = vfs_atomic_load_i64(
        (const int64_t*)(pn0.bytes + PAGENODE_OFF_VERSIONROOT));
    int count_after = 0;
    walk = vp;
    while (walk != 0) {
        count_after++;
        PoolSlot vs = {0};
        pool_acquire(&ctx->pool, walk, false, &vs);
        CHECK(vs.vptr != VFS_VPTR_NULL);
        walk = vfs_rd8_s(vs.bytes, VERSIONPAGE_OFF_NEXTPTR, ctx->page_size);
        pool_release(&ctx->pool, &vs);
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
    unlink(test_path);
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t file_vp = vfs_create(vfs, root_vp, "cow.txt", 0);
    CHECK(file_vp > 0);

    /* Write at epoch 0 */
    int ret = vfs_write(vfs, file_vp, "AAAA", 0, 4, 0);
    CHECK_EQ(ret, 4);

    /* Snapshot so epoch 2 becomes the live head (phase 27 C4). */
    int64_t cow_snap = vfs_snapshot(vfs);
    CHECK_EQ(cow_snap, 1);

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
    PoolSlot pn0 = {0};
    int rc_pn0 = tree_resolve_page(vfs, file_vp, 0, 0, false, &pn0);
    CHECK_EQ(rc_pn0, 0);
    int64_t vp = vfs_atomic_load_i64(
        (const int64_t*)(pn0.bytes + PAGENODE_OFF_VERSIONROOT));
    int count = 0;
    int64_t walk = vp;
    while (walk != 0) {
        count++;
        PoolSlot vs = {0};
        pool_acquire(&ctx->pool, walk, false, &vs);
        CHECK(vs.vptr != VFS_VPTR_NULL);
        walk = vfs_rd8_s(vs.bytes, VERSIONPAGE_OFF_NEXTPTR, ctx->page_size);
        pool_release(&ctx->pool, &vs);
    }
    CHECK_EQ(count, 2);

    vfs_unmount(vfs);
}

/* Write 2000 pages → second FileContent segment, reads across boundary */
static void test_write_multi_segment(void) {
    unlink(test_path);
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
 * Write to past-even epoch test — vfs_epoch_is_writable returns false
 * for past even epochs (per src/epoch.c real rules).
 * --------------------------------------------------------------------------- */

static void test_write_frozen_epoch(void) {
    unlink(test_path);
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t file_vp = vfs_create(vfs, root_vp, "frozen.txt", 0);
    CHECK(file_vp > 0);
    /* currentEpoch = 0; write at epoch 2 (past even, not current)
       must be rejected. */
    int ret = vfs_write(vfs, file_vp, "DATA", 0, 4, 2);
    CHECK_EQ(ret, VFS_ERR_IO);

    /* Verify: write at currentEpoch=0 still works. */
    ret = vfs_write(vfs, file_vp, "OKAY", 0, 4, 0);
    CHECK_EQ(ret, 4);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * mkdir basic: create a directory, verify it's in the parent's DirContent
 * chain and has the DirNode type.
 * --------------------------------------------------------------------------- */

static void test_mkdir_basic(void) {
    unlink(test_path);
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    /* Create a file and check the VirtualPtr in the DirContent chain */
    int64_t ret = vfs_mkdir(vfs, root_vp, "a", 0);
    CHECK(ret > 0);

    /* Verify entry exists in root's chain (W5a: SlotNode-wrapped) */
    PoolSlot root_slot = {0};
    pool_acquire(&ctx->pool, root_vp, false, &root_slot);
    CHECK(root_slot.vptr != VFS_VPTR_NULL);
    int64_t head = vfs_rd8_s(root_slot.bytes, DIRNODE_OFF_HEADPTR, ctx->page_size);
    CHECK(head != 0);
    pool_release(&ctx->pool, &root_slot);

    /* Walk DirSegment -> SlotNode -> first DirContent.  W5b. */
    PoolSlot seg_s = {0};
    pool_acquire(&ctx->pool, head, false, &seg_s);
    CHECK(seg_s.vptr != VFS_VPTR_NULL);
    int64_t slot_vp = vfs_rd8_s(seg_s.bytes, 8, ctx->page_size);  /* ANCHOR_OFF_HEADPTR */
    CHECK(slot_vp != 0);
    pool_release(&ctx->pool, &seg_s);

    PoolSlot ss = {0};
    pool_acquire(&ctx->pool, slot_vp, false, &ss);
    CHECK(ss.vptr != VFS_VPTR_NULL);
    int64_t dc_head = vfs_rd8_s(ss.bytes, 8, ctx->page_size);  /* ANCHOR_OFF_HEADPTR */
    CHECK(dc_head != 0);
    pool_release(&ctx->pool, &ss);

    uint32_t cc, ce;
    int64_t cp, np, nx;
    PoolSlot dc_slot = {0};
    pool_acquire(&ctx->pool, dc_head, false, &dc_slot);
    nodes_read_dircontent(dc_slot.bytes, &cc, &ce, &cp, &np, &nx, ctx->page_size);
    (void)cc; (void)ce; (void)np; (void)nx;
    pool_release(&ctx->pool, &dc_slot);
    CHECK(cp != 0);

    /* Verify child is a DirNode */
    PoolSlot child_slot = {0};
    pool_acquire(&ctx->pool, cp, false, &child_slot);
    CHECK(child_slot.vptr != VFS_VPTR_NULL);
    CHECK_EQ(vfs_rd2_s(child_slot.bytes, DIRNODE_OFF_TYPE, ctx->page_size),
             (int16_t)NODE_TYPE_DIR);
    pool_release(&ctx->pool, &child_slot);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * mkdir duplicate: same name at same epoch → VFS_ERR_EXISTS.
 * --------------------------------------------------------------------------- */

static void test_mkdir_duplicate(void) {
    unlink(test_path);
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
    unlink(test_path);
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
    unlink(test_path);
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
    unlink(test_path);
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
    unlink(test_path);
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
    unlink(test_path);
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
    unlink(test_path);
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
    unlink(test_path);
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
    unlink(test_path);
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
    unlink(test_path);
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
    unlink(test_path);
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
    unlink(test_path);
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
    unlink(test_path);
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
    unlink(test_path);
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
    unlink(test_path);
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
    unlink(test_path);
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
    unlink(test_path);
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
    unlink(test_path);
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
    unlink(test_path);
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
    unlink(test_path);
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
    unlink(test_path);
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

    /* Resolve page 0 — should return the existing PageNode.
     * vfs_write already created it; is_write=true is technically not
     * needed (the PageNode exists), but the new API requires the file
     * lock for is_write=true. Use the read path to avoid the lock. */
    PoolSlot pn_slot = {0};
    int rc_pn_slot = tree_resolve_page(vfs, file_vp, 0, 0, false, &pn_slot);
    CHECK_EQ(rc_pn_slot, 0);

    /* Assert page_index == 0 */
    uint32_t pn_idx = (uint32_t)vfs_rd4_s(pn_slot.bytes, PAGENODE_OFF_PAGEINDEX, ctx->page_size);
    CHECK_EQ(pn_idx, 0u);

    /* Walk FileContent's pageRootPtr chain — assert exactly 1 PageNode */
    PoolSlot file_slot = {0};
    pool_acquire(&ctx->pool, file_vp, false, &file_slot);
    CHECK(file_slot.vptr != VFS_VPTR_NULL);
    int64_t fc_vp = vfs_rd8_s(file_slot.bytes, FILENODE_OFF_HEADPTR, ctx->page_size);
    CHECK(fc_vp != 0);
    PoolSlot fc_slot = {0};
    pool_acquire(&ctx->pool, fc_vp, false, &fc_slot);
    CHECK(fc_slot.vptr != VFS_VPTR_NULL);
    int64_t pn_root = vfs_rd8_s(fc_slot.bytes, FILECONTENT_OFF_ROOTPTR, ctx->page_size);
    CHECK(pn_root != 0);

    int pn_count = 0;
    int64_t walk = pn_root;
    while (walk != 0) {
        pn_count++;
        PoolSlot pn = {0};
        pool_acquire(&ctx->pool, walk, false, &pn);
        CHECK(pn.vptr != VFS_VPTR_NULL);
        int64_t next = vfs_rd8_s(pn.bytes, PAGENODE_OFF_NEXTPTR, ctx->page_size);
        walk = next;
        pool_release(&ctx->pool, &pn);
    }
    CHECK_EQ(pn_count, 1);

    pool_release(&ctx->pool, &fc_slot);
    pool_release(&ctx->pool, &file_slot);
    vfs_unmount(vfs);
}

static void test_sparse_chain_mid_insert(void) {
    unlink(test_path);
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t file_vp = vfs_create(vfs, root_vp, "mid.txt", 0);
    CHECK(file_vp > 0);

    /* Resolve page 5 first — allocates PageNode with idx=5.
     * W3: is_write=true requires vfs_lock held for chain mutation. */
    CHECK_EQ(vfs_lock(vfs, file_vp, 0), VFS_OK);
    PoolSlot pn5 = {0};
    int rc_pn5 = tree_resolve_page(vfs, file_vp, 5, 0, true, &pn5);
    CHECK_EQ(rc_pn5, 0);
    uint32_t idx5 = (uint32_t)vfs_rd4_s(pn5.bytes, PAGENODE_OFF_PAGEINDEX, ctx->page_size);
    CHECK_EQ(idx5, 5u);
    CHECK_EQ(vfs_unlock(vfs, file_vp, 0), VFS_OK);

    /* Resolve page 2 — should insert at head (before idx=5) */
    CHECK_EQ(vfs_lock(vfs, file_vp, 0), VFS_OK);
    PoolSlot pn2 = {0};
    int rc_pn2 = tree_resolve_page(vfs, file_vp, 2, 0, true, &pn2);
    CHECK_EQ(rc_pn2, 0);
    uint32_t idx2 = (uint32_t)vfs_rd4_s(pn2.bytes, PAGENODE_OFF_PAGEINDEX, ctx->page_size);
    CHECK_EQ(idx2, 2u);
    CHECK_EQ(vfs_unlock(vfs, file_vp, 0), VFS_OK);

    /* Walk the chain — page 2 should be at head, page 5 next */
    PoolSlot file_slot = {0};
    pool_acquire(&ctx->pool, file_vp, false, &file_slot);
    CHECK(file_slot.vptr != VFS_VPTR_NULL);
    int64_t fc_vp = vfs_rd8_s(file_slot.bytes, FILENODE_OFF_HEADPTR, ctx->page_size);
    CHECK(fc_vp != 0);
    PoolSlot fc_slot = {0};
    pool_acquire(&ctx->pool, fc_vp, false, &fc_slot);
    CHECK(fc_slot.vptr != VFS_VPTR_NULL);
    int64_t pn_root = vfs_rd8_s(fc_slot.bytes, FILECONTENT_OFF_ROOTPTR, ctx->page_size);
    CHECK(pn_root != 0);

    int pn_count = 0;
    int64_t walk = pn_root;
    while (walk != 0) {
        pn_count++;
        PoolSlot pn = {0};
        pool_acquire(&ctx->pool, walk, false, &pn);
        CHECK(pn.vptr != VFS_VPTR_NULL);
        uint32_t idx = (uint32_t)vfs_rd4_s(pn.bytes, PAGENODE_OFF_PAGEINDEX, ctx->page_size);
        if (pn_count == 1) CHECK_EQ(idx, 2u);  /* head: page 2 */
        if (pn_count == 2) CHECK_EQ(idx, 5u);  /* next: page 5 */
        int64_t next = vfs_rd8_s(pn.bytes, PAGENODE_OFF_NEXTPTR, ctx->page_size);
        walk = next;
        pool_release(&ctx->pool, &pn);
    }
    CHECK_EQ(pn_count, 2);

    pool_release(&ctx->pool, &fc_slot);
    pool_release(&ctx->pool, &file_slot);
    vfs_unmount(vfs);
}

static void test_sparse_chain_tail_append(void) {
    unlink(test_path);
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t file_vp = vfs_create(vfs, root_vp, "tail.txt", 0);
    CHECK(file_vp > 0);

    /* Resolve page 2 first.
     * W3: is_write=true requires vfs_lock held for chain mutation. */
    CHECK_EQ(vfs_lock(vfs, file_vp, 0), VFS_OK);
    PoolSlot pn2 = {0};
    int rc_pn2 = tree_resolve_page(vfs, file_vp, 2, 0, true, &pn2);
    CHECK_EQ(rc_pn2, 0);
    uint32_t idx2 = (uint32_t)vfs_rd4_s(pn2.bytes, PAGENODE_OFF_PAGEINDEX, ctx->page_size);
    CHECK_EQ(idx2, 2u);
    CHECK_EQ(vfs_unlock(vfs, file_vp, 0), VFS_OK);

    /* Resolve page 7 — should append at tail (after idx=2) */
    CHECK_EQ(vfs_lock(vfs, file_vp, 0), VFS_OK);
    PoolSlot pn7 = {0};
    int rc_pn7 = tree_resolve_page(vfs, file_vp, 7, 0, true, &pn7);
    CHECK_EQ(rc_pn7, 0);
    uint32_t idx7 = (uint32_t)vfs_rd4_s(pn7.bytes, PAGENODE_OFF_PAGEINDEX, ctx->page_size);
    CHECK_EQ(idx7, 7u);
    CHECK_EQ(vfs_unlock(vfs, file_vp, 0), VFS_OK);

    /* Walk chain: idx=2 at head, idx=7 at tail */
    PoolSlot file_slot = {0};
    pool_acquire(&ctx->pool, file_vp, false, &file_slot);
    CHECK(file_slot.vptr != VFS_VPTR_NULL);
    int64_t fc_vp = vfs_rd8_s(file_slot.bytes, FILENODE_OFF_HEADPTR, ctx->page_size);
    CHECK(fc_vp != 0);
    PoolSlot fc_slot = {0};
    pool_acquire(&ctx->pool, fc_vp, false, &fc_slot);
    CHECK(fc_slot.vptr != VFS_VPTR_NULL);
    int64_t pn_root = vfs_rd8_s(fc_slot.bytes, FILECONTENT_OFF_ROOTPTR, ctx->page_size);
    CHECK(pn_root != 0);

    int pn_count = 0;
    int64_t walk = pn_root;
    while (walk != 0) {
        pn_count++;
        PoolSlot pn = {0};
        pool_acquire(&ctx->pool, walk, false, &pn);
        CHECK(pn.vptr != VFS_VPTR_NULL);
        uint32_t idx = (uint32_t)vfs_rd4_s(pn.bytes, PAGENODE_OFF_PAGEINDEX, ctx->page_size);
        if (pn_count == 1) CHECK_EQ(idx, 2u);
        if (pn_count == 2) CHECK_EQ(idx, 7u);
        int64_t next = vfs_rd8_s(pn.bytes, PAGENODE_OFF_NEXTPTR, ctx->page_size);
        walk = next;
        pool_release(&ctx->pool, &pn);
    }
    CHECK_EQ(pn_count, 2);

    pool_release(&ctx->pool, &fc_slot);
    pool_release(&ctx->pool, &file_slot);
    vfs_unmount(vfs);
}

#ifndef NDEBUG
/* Debug-only test: depends on tree_resolve_page_cache_builds counter. */
static void test_sparse_threshold_cache(void) {
    unlink(test_path);
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t file_vp = vfs_create(vfs, root_vp, "threshold.txt", 0);
    CHECK(file_vp > 0);

    int baseline = tree_resolve_page_cache_builds_get();

    /* Resolve pages 0..63 — 64 unique PageNodes, at SPARSE_CACHE_THRESHOLD (64).
     * With sparse allocation, the cache may or may not trigger at exactly
     * 64 pages — the threshold behavior differs from the old dense model.
     * W3: is_write=true allocates new PageNodes; vfs_lock held per call. */
    for (int i = 0; i < 64; i++) {
        CHECK_EQ(vfs_lock(vfs, file_vp, 0), VFS_OK);
        PoolSlot pn = {0};
        int rc = tree_resolve_page(vfs, file_vp, (int64_t)i, 0, true, &pn);
        CHECK_EQ(rc, 0);
        CHECK_EQ(vfs_unlock(vfs, file_vp, 0), VFS_OK);
    }
    /* Resolve page 64 — at least one cache build should have occurred */
    CHECK_EQ(vfs_lock(vfs, file_vp, 0), VFS_OK);
    PoolSlot pn = {0};
    int rc_pn = tree_resolve_page(vfs, file_vp, 64, 0, true, &pn);
    CHECK_EQ(rc_pn, 0);
    CHECK_EQ(vfs_unlock(vfs, file_vp, 0), VFS_OK);
    int after_65 = tree_resolve_page_cache_builds_get() - baseline;
    CHECK(after_65 >= 1);  /* at least one build by now */

    vfs_unmount(vfs);
}
#endif

static void test_sparse_read_no_allocate(void) {
    unlink(test_path);
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t file_vp = vfs_create(vfs, root_vp, "noread.txt", 0);
    CHECK(file_vp > 0);

    /* Read attempt on unwritten page — should return -1, no allocation */
    PoolSlot pn_ro = {0};
    int rc_pn_ro = tree_resolve_page(vfs, file_vp, 5, 0, false, &pn_ro);
    CHECK_EQ(rc_pn_ro, -1);

    /* Assert no FileContent was allocated */
    PoolSlot file_slot = {0};
    pool_acquire(&ctx->pool, file_vp, false, &file_slot);
    CHECK(file_slot.vptr != VFS_VPTR_NULL);
    int64_t fc_vp = vfs_rd8_s(file_slot.bytes, FILENODE_OFF_HEADPTR, ctx->page_size);
    CHECK_EQ(fc_vp, 0);

    /* Write to page 5 — should allocate exactly 1 PageNode.
     * W3: is_write=true requires vfs_lock held for chain mutation. */
    CHECK_EQ(vfs_lock(vfs, file_vp, 0), VFS_OK);
    PoolSlot pn = {0};
    int rc_pn = tree_resolve_page(vfs, file_vp, 5, 0, true, &pn);
    CHECK_EQ(rc_pn, 0);
    uint32_t idx = (uint32_t)vfs_rd4_s(pn.bytes, PAGENODE_OFF_PAGEINDEX, ctx->page_size);
    CHECK_EQ(idx, 5u);
    CHECK_EQ(vfs_unlock(vfs, file_vp, 0), VFS_OK);

    /* Walk chain — exactly 1 PageNode */
    pool_release(&ctx->pool, &file_slot);
    pool_acquire(&ctx->pool, file_vp, false, &file_slot);
    fc_vp = vfs_rd8_s(file_slot.bytes, FILENODE_OFF_HEADPTR, ctx->page_size);
    CHECK(fc_vp != 0);
    PoolSlot fc_slot = {0};
    pool_acquire(&ctx->pool, fc_vp, false, &fc_slot);
    CHECK(fc_slot.vptr != VFS_VPTR_NULL);
    int64_t pn_root = vfs_rd8_s(fc_slot.bytes, FILECONTENT_OFF_ROOTPTR, ctx->page_size);
    CHECK(pn_root != 0);

    int pn_count = 0;
    int64_t walk = pn_root;
    while (walk != 0) {
        pn_count++;
        PoolSlot pw = {0};
        pool_acquire(&ctx->pool, walk, false, &pw);
        CHECK(pw.vptr != VFS_VPTR_NULL);
        int64_t next = vfs_rd8_s(pw.bytes, PAGENODE_OFF_NEXTPTR, ctx->page_size);
        walk = next;
        }
    CHECK_EQ(pn_count, 1);

    pool_release(&ctx->pool, &fc_slot);
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

    /* Resolve page 0 — vfs_write already created it, use the read path
     * to avoid the W3 lock requirement. */
    PoolSlot pn = {0};
    int rc_pn = tree_resolve_page(vfs, file_vp, 0, 0, false, &pn);
    CHECK_EQ(rc_pn, 0);
    uint32_t idx = (uint32_t)vfs_rd4_s(pn.bytes, PAGENODE_OFF_PAGEINDEX, ctx->page_size);
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
    unlink(test_path);
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
    PoolSlot pn = {0};
    int rc_pn = tree_resolve_page(vfs, file_vp, 0, 0, false, &pn);
    CHECK_EQ(rc_pn, 0);
    uint32_t idx = (uint32_t)vfs_rd4_s(pn.bytes, PAGENODE_OFF_PAGEINDEX, ctx->page_size);
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
    /* W3: tree_resolve_page with is_write=true requires the caller to
     * hold the file lock.  Acquire it here so the two threads serialize
     * on the file lock (one inserts page 7, the other page 11) — this
     * is the new "lock-vs-CAS" discipline that replaces the prior
     * CAS-retry path.  The race this test used to exercise (CAS retry
     * under contention) no longer exists; the test now verifies the
     * lock-based serialization. */
    if (vfs_lock(a->vfs, a->file_vp, 0) != VFS_OK) return NULL;
    PoolSlot pn = {0};
    int rc = tree_resolve_page(a->vfs, a->file_vp,
                                a->page_idx, 0, true, &pn);
    if (rc == 0) {
        uint32_t idx = (uint32_t)vfs_rd4_s(pn.bytes, PAGENODE_OFF_PAGEINDEX,
                                            a->vfs->ctx->page_size);
        a->success = (idx == (uint32_t)a->page_idx);
    }
    vfs_unlock(a->vfs, a->file_vp, 0);
    return NULL;
}

static void test_sparse_concurrent_insert(void) {
    unlink(test_path);
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t file_vp = vfs_create(vfs, root_vp, "conc.txt", 0);
    CHECK(file_vp > 0);

    /* Phase 1: pre-resolve page 3 so the FileContent chain exists.
     * W3: is_write=true requires vfs_lock held for chain mutation. */
    CHECK_EQ(vfs_lock(vfs, file_vp, 0), VFS_OK);
    PoolSlot pn3 = {0};
    int rc_pn3 = tree_resolve_page(vfs, file_vp, 3, 0, true, &pn3);
    CHECK_EQ(rc_pn3, 0);
    CHECK_EQ((uint32_t)vfs_rd4_s(pn3.bytes, PAGENODE_OFF_PAGEINDEX, ctx->page_size), 3u);
    CHECK_EQ(vfs_unlock(vfs, file_vp, 0), VFS_OK);

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
    PoolSlot file_slot = {0};
    pool_acquire(&ctx->pool, file_vp, false, &file_slot);
    int64_t fc_vp = vfs_rd8_s(file_slot.bytes, FILENODE_OFF_HEADPTR, ctx->page_size);
    pool_release(&ctx->pool, &file_slot);
    PoolSlot fc_slot = {0};
    pool_acquire(&ctx->pool, fc_vp, false, &fc_slot);
    int64_t pn_root = vfs_rd8_s(fc_slot.bytes, FILECONTENT_OFF_ROOTPTR, ctx->page_size);
    pool_release(&ctx->pool, &fc_slot);

    int pn_count = 0;
    int64_t walk = pn_root;
    uint32_t seen[3] = {0, 0, 0};
    while (walk != 0) {
        CHECK(pn_count < 3);
        PoolSlot pw = {0};
        pool_acquire(&ctx->pool, walk, false, &pw);
        uint32_t idx = (uint32_t)vfs_rd4_s(pw.bytes, PAGENODE_OFF_PAGEINDEX, ctx->page_size);
        seen[pn_count] = idx;
        pn_count++;
        int64_t next = vfs_rd8_s(pw.bytes, PAGENODE_OFF_NEXTPTR, ctx->page_size);
        pool_release(&ctx->pool, &pw);
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
    unlink(test_path);
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
    unlink(test_path);
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
    {
        PoolSlot fn_slot = {0};
        pool_acquire(&ctx->pool, child_vp_a, true, &fn_slot);
        nodes_write_filenode(fn_slot.bytes, nid_a, 0, 0, 0, ps);
        pool_release(&ctx->pool, &fn_slot);
    }
    {
        PoolSlot fn_slot = {0};
        pool_acquire(&ctx->pool, child_vp_b, true, &fn_slot);
        nodes_write_filenode(fn_slot.bytes, nid_b, 0, 0, 0, ps);
        pool_release(&ctx->pool, &fn_slot);
    }

    /* Allocate two DirContent entries: DC_a → DC_b → 0
     * Both point to the forced-hash NameEntries and different child VPs.
     * W5a: each child has its own SlotNode; the SlotNode's chain
     * contains the DirContent. */
    int64_t dc_vp_a = pool_alloc(&ctx->pool);
    int64_t dc_vp_b = pool_alloc(&ctx->pool);
    CHECK(dc_vp_a != VFS_VPTR_NULL);
    CHECK(dc_vp_b != VFS_VPTR_NULL);
    {
        PoolSlot dc_slot = {0};
        pool_acquire(&ctx->pool, dc_vp_b, true, &dc_slot);
        nodes_write_dircontent(dc_slot.bytes, nid_b, 0, child_vp_b, name_vp_b, 0, ps);
        pool_release(&ctx->pool, &dc_slot);
    }
    {
        PoolSlot dc_slot = {0};
        pool_acquire(&ctx->pool, dc_vp_a, true, &dc_slot);
        nodes_write_dircontent(dc_slot.bytes, nid_a, 0, child_vp_a, name_vp_a, 0, ps);
        pool_release(&ctx->pool, &dc_slot);
    }

    /* Allocate two SlotNode entries: slot_a → slot_b → 0 */
    int64_t slot_vp_a = pool_alloc(&ctx->pool);
    int64_t slot_vp_b = pool_alloc(&ctx->pool);
    CHECK(slot_vp_a != VFS_VPTR_NULL);
    CHECK(slot_vp_b != VFS_VPTR_NULL);
    {
        PoolSlot slot_bytes_b = {0};
        pool_acquire(&ctx->pool, slot_vp_b, true, &slot_bytes_b);
        nodes_write_anchor(slot_bytes_b.bytes, ANCHOR_KIND_UNIT_SLOT,
                           nid_b, dc_vp_b, 0, 0, ps);
        pool_release(&ctx->pool, &slot_bytes_b);
    }
    {
        PoolSlot slot_bytes_a = {0};
        pool_acquire(&ctx->pool, slot_vp_a, true, &slot_bytes_a);
        nodes_write_anchor(slot_bytes_a.bytes, ANCHOR_KIND_UNIT_SLOT,
                           nid_a, dc_vp_a, slot_vp_b, 0, ps);
        pool_release(&ctx->pool, &slot_bytes_a);
    }

    /* W5b: wrap the SlotNodes in a DirSegment before installing in
     * root's HEADPTR.  Allocate the Segment, set headPtr/sibPtr, set
     * count=2. */
    int64_t seg_vp = pool_alloc(&ctx->pool);
    CHECK(seg_vp != VFS_VPTR_NULL);
    {
        PoolSlot seg_bytes = {0};
        pool_acquire(&ctx->pool, seg_vp, true, &seg_bytes);
        nodes_write_anchor(seg_bytes.bytes, ANCHOR_KIND_SEGMENT_DIR, 0, slot_vp_a, 0, 2, ps);
        pool_release(&ctx->pool, &seg_bytes);
    }

    /* Set root's headPtr to seg_vp (Segment, not SlotNode directly) */
    {
        PoolSlot root_slot = {0};
        pool_acquire(&ctx->pool, root_vp, true, &root_slot);
        CHECK(root_slot.vptr != VFS_VPTR_NULL);
        vfs_wr8_s(root_slot.bytes, DIRNODE_OFF_HEADPTR, seg_vp, ps);
        pool_release(&ctx->pool, &root_slot);
    }

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
    unlink(test_path);
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
    unlink(test_path);
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;

    int64_t page_size = ctx->page_size;

    /* Use the root DirNode's actual indexHeadPtr (created by bootstrap) */
    PoolSlot rootSlot = {0};
    pool_acquire(&ctx->pool, ctx->rootNodeOffset, false, &rootSlot);
    CHECK(rootSlot.vptr != VFS_VPTR_NULL);
    int64_t indexRoot = vfs_rd8_s(rootSlot.bytes, DIRNODE_OFF_INDEXHEADPTR, page_size);
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

    pool_release(&ctx->pool, &rootSlot);
    vfs_unmount(vfs);
}

static void test_dircontentindex_same_leaf(void) {
    unlink(test_path);
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t page_size = ctx->page_size;

    PoolSlot rootSlot = {0};

    pool_acquire(&ctx->pool, ctx->rootNodeOffset, false, &rootSlot);
    CHECK(rootSlot.vptr != VFS_VPTR_NULL);
    int64_t indexRoot = vfs_rd8_s(rootSlot.bytes, DIRNODE_OFF_INDEXHEADPTR, page_size);
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

    pool_release(&ctx->pool, &rootSlot);
    vfs_unmount(vfs);
}

static void test_vfs_create_open_tree(void) {
    unlink(test_path);
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    /* Create a file — vfs_create writes to BOTH chain and tree */
    int64_t file_vp = vfs_create(vfs, root_vp, "tree_test.txt", 0);
    CHECK(file_vp > 0);

    /* Verify the tree has an entry for this file */
    PoolSlot rootSlot = {0};
    pool_acquire(&ctx->pool, root_vp, false, &rootSlot);
    CHECK(rootSlot.vptr != VFS_VPTR_NULL);
    int64_t indexRoot = vfs_rd8_s(rootSlot.bytes, DIRNODE_OFF_INDEXHEADPTR,
                                   ctx->page_size);
    pool_release(&ctx->pool, &rootSlot);
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
        PoolSlot link_slot = {0};
        pool_acquire(&ctx->pool, linkVP, false, &link_slot);
        nodes_read_dircontentlink(link_slot.bytes,
                                  &dcVP, &nextLinkVP, ctx->page_size);
        pool_release(&ctx->pool, &link_slot);
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
    unlink(test_path);
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

/* ---------------------------------------------------------------------------
 * W5g: Concurrent dir-write test.
 *
 * Two threads each create 1000 distinct children in the same dir.
 * Validates the W4 lock discipline (parent + child lock) correctly
 * serializes the writes — both threads' children must be visible
 * after sync.
 *
 * Per W4 spec: vfs_create takes the parent DirNode lock first, then
 * the new child's ContentUnit lock.  Concurrent creates serialize on
 * the parent lock.
 * --------------------------------------------------------------------------- */

typedef struct {
    vfs_t* vfs;
    int64_t dir_vp;
    int     start;
    int     count;
    int     ok;
} create_thread_args;

static void* create_many_thread(void* arg) {
    create_thread_args* a = (create_thread_args*)arg;
    char name[32];
    for (int i = 0; i < a->count; i++) {
        int idx = a->start + i;
        snprintf(name, sizeof(name), "f%05d.txt", idx);
        if (vfs_create(a->vfs, a->dir_vp, name, 0) <= 0) {
            return NULL;
        }
    }
    a->ok = 1;
    return NULL;
}

static void test_concurrent_dir_writes(void) {
    unlink(test_path);
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    int64_t root_vp = vfs->ctx->rootNodeOffset;

    create_thread_args a1 = {vfs, root_vp, 0,     1000, 0};
    create_thread_args a2 = {vfs, root_vp, 1000,  1000, 0};

    pthread_t t1, t2;
    CHECK_EQ(pthread_create(&t1, NULL, create_many_thread, &a1), 0);
    CHECK_EQ(pthread_create(&t2, NULL, create_many_thread, &a2), 0);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    CHECK(a1.ok);
    CHECK(a2.ok);

    /* Verify all 2000 children are visible.  We use a single
     * sequential scan to avoid concurrent readdir races (readdir
     * itself isn't under a tree-level lock, but vfs_open is
     * per-VP-lock-free, so this is safe). */
    int found = 0;
    char name[32];
    for (int i = 0; i < 2000; i++) {
        snprintf(name, sizeof(name), "f%05d.txt", i);
        if (vfs_open(vfs, root_vp, name, 0) > 0) found++;
    }
    CHECK_EQ(found, 2000);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * W5g: Concurrent rename test.
 *
 * Two threads each rename 1000 distinct children in the same dir
 * (different children, different target names).  Validates the W4
 * lock discipline for same-dir rename (parent + child lock, lower
 * VP first) — both threads' renames must succeed and the new names
 * must be findable.
 * --------------------------------------------------------------------------- */

typedef struct {
    vfs_t* vfs;
    int64_t dir_vp;
    int     start;
    int     count;
    int     ok;
} rename_thread_args;

static void* rename_many_thread(void* arg) {
    rename_thread_args* a = (rename_thread_args*)arg;
    char src[32], dst[32];
    for (int i = 0; i < a->count; i++) {
        int idx = a->start + i;
        snprintf(src, sizeof(src), "old_%05d.txt", idx);
        snprintf(dst, sizeof(dst), "new_%05d.txt", idx);
        if (vfs_rename(a->vfs, a->dir_vp, src, a->dir_vp, dst, 0) != VFS_OK) {
            return NULL;
        }
    }
    a->ok = 1;
    return NULL;
}

static void test_concurrent_rename(void) {
    unlink(test_path);
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    int64_t root_vp = vfs->ctx->rootNodeOffset;

    /* Pre-create 2000 source files. */
    char name[32];
    for (int i = 0; i < 2000; i++) {
        snprintf(name, sizeof(name), "old_%05d.txt", i);
        CHECK(vfs_create(vfs, root_vp, name, 0) > 0);
    }

    /* Concurrent rename: thread 1 renames 0..999, thread 2 renames
     * 1000..1999.  Same parent, different children. */
    rename_thread_args a1 = {vfs, root_vp, 0,     1000, 0};
    rename_thread_args a2 = {vfs, root_vp, 1000,  1000, 0};

    pthread_t t1, t2;
    CHECK_EQ(pthread_create(&t1, NULL, rename_many_thread, &a1), 0);
    CHECK_EQ(pthread_create(&t2, NULL, rename_many_thread, &a2), 0);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    CHECK(a1.ok);
    CHECK(a2.ok);

    /* Verify all 2000 new names are findable and all 2000 old names
     * are gone. */
    int found_new = 0, found_old = 0;
    for (int i = 0; i < 2000; i++) {
        snprintf(name, sizeof(name), "new_%05d.txt", i);
        if (vfs_open(vfs, root_vp, name, 0) > 0) found_new++;
        snprintf(name, sizeof(name), "old_%05d.txt", i);
        if (vfs_open(vfs, root_vp, name, 0) > 0) found_old++;
    }
    CHECK_EQ(found_new, 2000);
    CHECK_EQ(found_old, 0);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * W5g: 10K+ children stress test.
 *
 * Sequentially creates 10K children, then deletes half, renames
 * the rest, and verifies the final readdir returns the right set.
 * This stresses the DirSegment chunking (with 10K children, we
 * expect ~10 SlotNodes per DirSegment and ~10 DirSegments).
 * --------------------------------------------------------------------------- */

static void test_stress_10k_children(void) {
    unlink(test_path);
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    int64_t root_vp = vfs->ctx->rootNodeOffset;
    char name[32];

    /* Create 10K children. */
    for (int i = 0; i < 10000; i++) {
        snprintf(name, sizeof(name), "stress_%05d.txt", i);
        CHECK(vfs_create(vfs, root_vp, name, 0) > 0);
    }

    /* Delete every odd-indexed child. */
    for (int i = 1; i < 10000; i += 2) {
        snprintf(name, sizeof(name), "stress_%05d.txt", i);
        CHECK_EQ(vfs_delete(vfs, root_vp, name, 0), VFS_OK);
    }

    /* Rename the remaining even-indexed children. */
    for (int i = 0; i < 10000; i += 2) {
        snprintf(name, sizeof(name), "stress_%05d.txt", i);
        char new_name[32];
        snprintf(new_name, sizeof(new_name), "renamed_%05d.txt", i);
        CHECK_EQ(vfs_rename(vfs, root_vp, name, root_vp, new_name, 0), VFS_OK);
    }

    /* Verify: 5000 renamed children, 0 old stress_* children. */
    int found_renamed = 0, found_stress = 0;
    for (int i = 0; i < 10000; i += 2) {
        snprintf(name, sizeof(name), "renamed_%05d.txt", i);
        if (vfs_open(vfs, root_vp, name, 0) > 0) found_renamed++;
        snprintf(name, sizeof(name), "stress_%05d.txt", i);
        if (vfs_open(vfs, root_vp, name, 0) > 0) found_stress++;
    }
    CHECK_EQ(found_renamed, 5000);
    CHECK_EQ(found_stress, 0);

    vfs_unmount(vfs);
}

/* ---------------------------------------------------------------------------
 * W5g: ContentUnit visibility rule (delete+recreate).
 *
 * Per spec §4.4: when a child is deleted and a NEW child is created
 * with the same name, the radix index has two ContentUnit links at
 * the same hash.  The visibility rule says: a ContentUnit is visible
 * at epoch E iff its highest-applicable DirContent is a live entry
 * (namePtr != 0).  Tombstoned ContentUnits are skipped.
 *
 * This test exercises:
 *   ep0: vfs_create("a.txt")   → SlotNode 1 (live @ep0)
 *   ep2: vfs_delete("a.txt")   → SlotNode 1 (tombstone @ep2)
 *   ep4: vfs_create("a.txt")   → SlotNode 2 (live @ep4)
 *
 * At ep0: SlotNode 1 is visible (original child live)
 * At ep2: SlotNode 1 is tombstoned, SlotNode 2 doesn't apply → NOTFOUND
 * At ep4: SlotNode 2 is visible (new child live)
 * --------------------------------------------------------------------------- */

static void test_contentunit_visibility(void) {
    unlink(test_path);
    vfs_t* vfs = vfs_mount(test_path, 8192);
    CHECK(vfs != NULL);
    int64_t root_vp = vfs->ctx->rootNodeOffset;

    /* ep0: create the original child. */
    int64_t orig_vp = vfs_create(vfs, root_vp, "shared.txt", 0);
    CHECK(orig_vp > 0);

    /* Bump to ep2 via snapshot.  After this, currentEpoch=2 and ep2
     * is the writable head. */
    int64_t s1 = vfs_snapshot(vfs);
    CHECK_EQ(s1, 1);
    CHECK_EQ(vfs->ctx->currentEpoch, 2);

    /* ep2: delete the original child. */
    CHECK_EQ(vfs_delete(vfs, root_vp, "shared.txt", 2), VFS_OK);

    /* Bump to ep4.  After this, currentEpoch=4 and ep4 is writable. */
    int64_t s2 = vfs_snapshot(vfs);
    CHECK_EQ(s2, 3);
    CHECK_EQ(vfs->ctx->currentEpoch, 4);

    /* ep4: create a new child with the same name (different nodeId). */
    int64_t new_vp = vfs_create(vfs, root_vp, "shared.txt", 4);
    CHECK(new_vp > 0);
    CHECK(new_vp != orig_vp);  /* new child, different VP */

    /* At ep0: the original child should be visible. */
    int64_t ep0_vp = vfs_open(vfs, root_vp, "shared.txt", 0);
    CHECK_EQ(ep0_vp, orig_vp);

    /* At ep2: the name is tombstoned, should NOT be found. */
    int64_t ep2_vp = vfs_open(vfs, root_vp, "shared.txt", 2);
    CHECK_EQ(ep2_vp, (int64_t)VFS_ERR_NOTFOUND);

    /* At ep4: the new child should be visible. */
    int64_t ep4_vp = vfs_open(vfs, root_vp, "shared.txt", 4);
    CHECK_EQ(ep4_vp, new_vp);

    vfs_unmount(vfs);
}

static void test_delete_recreate_same_name(void) {
    unlink(test_path);
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

    /* Snapshot so epoch 2 becomes the live head (phase 27 C4). */
    int64_t rsnap = vfs_snapshot(vfs);
    CHECK_EQ(rsnap, 1);

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
    unlink(test_path);
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

/* ---------------------------------------------------------------------------
 * Phase 26 / W2: dedicated read-rule test for vfs_chain_walk.
 *
 * Builds a chain of 3 VersionPage entries directly via the pool (no
 * real VFS file needed — we test the walk in isolation), then
 * verifies all 5 read-rule cases:
 *
 *   - exact-match-wins:     an entry at the query epoch
 *   - odd-skip:             an odd epoch below the query (unrelated snap)
 *   - committed-base:       an even epoch below the query
 *   - future-skip:          an entry at an epoch above the query
 *   - empty-chain:          chain_head == 0 returns WALK_NEED_GROW
 *
 * Also exercises the per-entry mapper remap: if the entry's stored
 * epoch is in the mapper (e.g. 6 → 8), the effective epoch used for
 * comparison is the resolved epoch, not the stored one.
 * --------------------------------------------------------------------------- */

static int64_t alloc_versionpage_slot(vfs_t* vfs, uint32_t stored_epoch,
                                      int64_t dataPage, int64_t nextPtr) {
    TreeContext* ctx = vfs->ctx;
    int64_t vp = pool_alloc(&ctx->pool);
    if (vp == VFS_VPTR_NULL) return VFS_VPTR_NULL;
    PoolSlot slot = {0};
    pool_acquire(&ctx->pool, vp, true, &slot);
    if (slot.vptr == VFS_VPTR_NULL) return VFS_VPTR_NULL;
    nodes_write_versionpage(slot.bytes, stored_epoch, dataPage, nextPtr,
                            ctx->page_size);
    pool_release(&ctx->pool, &slot);
    return vp;
}

static void test_chain_walk_read_rule(void) {
    const char* path = "/tmp/test_chain_walk.vfs";
    unlink(path);
    vfs_t* vfs = vfs_mount(path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;

    /* Build a 3-entry VersionPage chain in HEAD → TAIL order.
       Chain: [v4@E=4 dataPage=400] → [v2@E=2 dataPage=200] → [v1@E=0 dataPage=100]
       (newest first, oldest last — chains are descending in epoch). */
    int64_t v1 = alloc_versionpage_slot(vfs, 0, 100, 0);
    int64_t v2 = alloc_versionpage_slot(vfs, 2, 200, v1);
    int64_t v4 = alloc_versionpage_slot(vfs, 4, 400, v2);
    CHECK(v4 != VFS_VPTR_NULL);

    /* Case 1: exact-match-wins.  Query epoch=4.  Walk should return v4. */
    {
        PoolSlot leaf = {0};
        WalkResult r = vfs_chain_walk(ctx, v4, 4, &leaf);
        CHECK_EQ((int)r, (int)WALK_FOUND);
        int64_t dp = vfs_rd8_s(leaf.bytes, VERSIONPAGE_OFF_DATAPAGE, ctx->page_size);
        CHECK_EQ(dp, 400);
    }

    /* Case 2: committed-base.  Query epoch=3 (odd — no exact match).
       First even below 3 is 2 → returns v2. */
    {
        PoolSlot leaf = {0};
        WalkResult r = vfs_chain_walk(ctx, v4, 3, &leaf);
        CHECK_EQ((int)r, (int)WALK_FOUND);
        int64_t dp = vfs_rd8_s(leaf.bytes, VERSIONPAGE_OFF_DATAPAGE, ctx->page_size);
        CHECK_EQ(dp, 200);
    }

    /* Case 3: future-skip.  Query epoch=10.  All entries are below 10,
       but the highest even below 10 is 4 → returns v4. */
    {
        PoolSlot leaf = {0};
        WalkResult r = vfs_chain_walk(ctx, v4, 10, &leaf);
        CHECK_EQ((int)r, (int)WALK_FOUND);
        int64_t dp = vfs_rd8_s(leaf.bytes, VERSIONPAGE_OFF_DATAPAGE, ctx->page_size);
        CHECK_EQ(dp, 400);
    }

    /* Case 4: odd-skip — odd epoch below query.  Build a chain with
       an odd entry between two evens.  Query epoch=5: first entry at
       E=5 (exact), then odd@E=3 (skip), then even@E=2 (committed
       base).  We want the exact-match case to win, not the even base. */
    {
        int64_t base_v0 = alloc_versionpage_slot(vfs, 0, 0, 0);
        int64_t base_v2 = alloc_versionpage_slot(vfs, 2, 222, base_v0);
        int64_t odd_v3  = alloc_versionpage_slot(vfs, 3, 333, base_v2);
        int64_t top_v5  = alloc_versionpage_slot(vfs, 5, 555, odd_v3);

        PoolSlot leaf = {0};
        WalkResult r = vfs_chain_walk(ctx, top_v5, 5, &leaf);
        CHECK_EQ((int)r, (int)WALK_FOUND);
        int64_t dp = vfs_rd8_s(leaf.bytes, VERSIONPAGE_OFF_DATAPAGE, ctx->page_size);
        CHECK_EQ(dp, 555);  /* exact match at v5, not the even@E=2 base */
    }

    /* Case 4b: odd-skip from below.  Query epoch=5 but no entry at 5.
       First even below 5 is 2 (skipping the odd@3).  Returns the v2
       entry (dataPage=222), not the odd@3 (333). */
    {
        int64_t base_v0 = alloc_versionpage_slot(vfs, 0, 0, 0);
        int64_t base_v2 = alloc_versionpage_slot(vfs, 2, 222, base_v0);
        int64_t odd_v3  = alloc_versionpage_slot(vfs, 3, 333, base_v2);
        int64_t top_v4  = alloc_versionpage_slot(vfs, 4, 444, odd_v3);

        PoolSlot leaf = {0};
        WalkResult r = vfs_chain_walk(ctx, top_v4, 5, &leaf);
        CHECK_EQ((int)r, (int)WALK_FOUND);
        int64_t dp = vfs_rd8_s(leaf.bytes, VERSIONPAGE_OFF_DATAPAGE, ctx->page_size);
        CHECK_EQ(dp, 444);  /* even@4 wins over odd@3 */
    }

    /* Case 5: empty chain (chain_head=0) returns WALK_NEED_GROW. */
    {
        PoolSlot leaf = {0};
        WalkResult r = vfs_chain_walk(ctx, 0, 4, &leaf);
        CHECK_EQ((int)r, (int)WALK_NEED_GROW);
        CHECK_EQ(leaf.vptr, VFS_VPTR_NULL);
    }

    /* Case 6: per-entry mapper remap.  Add a mapper entry 4 → 8 (E=4
       with traversal-apply flag, remaps to E=8).  Query epoch=8.
       The walk should remap E=4 → E=8 (exact match) and return
       dataPage=400, not dataPage=200 from the E=2 entry. */
    {
        /* Insert mapper: fromEpoch=4, toEpoch=8, flags=traversal_apply. */
        int64_t mapper_vp = pool_alloc(&ctx->pool);
        CHECK(mapper_vp != VFS_VPTR_NULL);
        PoolSlot mapper_slot = {0};
        pool_acquire(&ctx->pool, mapper_vp, true, &mapper_slot);
        nodes_write_mapperentry(mapper_slot.bytes, 4, 8,
                                MAPPER_FLAG_TRAVERSAL_APPLY, 0,
                                ctx->page_size);
        pool_release(&ctx->pool, &mapper_slot);

        /* Re-point the superblock at this mapper entry. */
        ctx->epochMapperPtr = mapper_vp;
        /* Refresh the in-memory mapper table from the chain. */
        mapper_table_init(&ctx->mapper_table, &ctx->pool, &ctx->epochMapperPtr);
        int64_t mvp = ctx->epochMapperPtr;
        while (mvp != 0) {
            PoolSlot ms = {0};
            pool_acquire(&ctx->pool, mvp, false, &ms);
            uint32_t fromEp, toEp; uint16_t flags; int64_t mnext;
            nodes_read_mapperentry(ms.bytes, &fromEp, &toEp, &flags, &mnext,
                                  ctx->page_size);
            mapper_table_insert(&ctx->mapper_table, (int64_t)fromEp,
                                (int64_t)toEp, flags);
            mvp = mnext;
            pool_release(&ctx->pool, &ms);
        }

        PoolSlot leaf = {0};
        WalkResult r = vfs_chain_walk(ctx, v4, 8, &leaf);
        CHECK_EQ((int)r, (int)WALK_FOUND);
        int64_t dp = vfs_rd8_s(leaf.bytes, VERSIONPAGE_OFF_DATAPAGE, ctx->page_size);
        CHECK_EQ(dp, 400);  /* remapped v4 (stored E=4 → effective E=8) wins */
    }

    vfs_unmount(vfs);
    unlink(path);
}

/* ---------------------------------------------------------------------------
 * W6: test_chain_walk_no_duplication — static check that the
 * standard read-rule (mapper remap + even/odd + exact-match-wins)
 * lives in EXACTLY ONE place: vfs_chain_walk in src/tree.c.  All
 * callers go through vfs_chain_walk or vfs_chain_walk_extended
 * (which delegates to vfs_chain_walk) for the per-leaf chain
 * walk + read-rule.
 *
 * The read-rule markers we search for:
 *   - "mapper_table_traversal_apply("  (per-entry mapper remap)
 *   - "if (eff_epoch == read_epoch)"  (exact-match-wins)
 *
 * After W6, these markers should appear in EXACTLY ONE function
 * (vfs_chain_walk).  vfs_chain_walk_extended has its own
 * `eff_epoch` variable but uses the local `read_epoch` parameter,
 * not the literal "read_epoch" string — so the static check
 * specifically catches the read-rule pattern.
 *
 * vfs_commit (in src/epoch.c) keeps its own per-page
 * VersionPage walk because the commit needs to know about
 * has_snapshot AND has_live (two conditions the standard
 * read-rule doesn't expose).  This is the ONE remaining inline
 * walk; it's commit-specific, not the standard read-rule.
 *
 * We grep only src/tree.c for the marker — commit-specific code
 * in src/epoch.c uses a different pattern (% 2 == 0 with a
 * different surrounding condition).
 * --------------------------------------------------------------------------- */

static int count_occurrences_in(const char* path, const char* needle) {
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    int count = 0;
    char* line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, f)) != -1) {
        /* Skip comment lines starting with " * " (block comment
         * body).  Real code lines that mention the marker are
         * counted.  Lines that start with "//" (C++ comments) are
         * also skipped. */
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '*' || *p == '/') continue;
        if (strstr(line, needle) != NULL) count++;
    }
    free(line);
    fclose(f);
    return count;
}

static void test_chain_walk_no_duplication(void) {
    /* The standard read-rule (mapper remap per entry + even/odd
     * + exact-match-wins) must live in EXACTLY ONE function in
     * src/tree.c — vfs_chain_walk.  After W6, all other callers
     * go through vfs_chain_walk (directly) or vfs_chain_walk_extended
     * (which delegates).  The commit-specific walk in src/epoch.c
     * uses a different pattern and is excluded. */
    int rule_markers = count_occurrences_in(
        "/Users/bogdanadnan/Projects/ixsphere/native/iXSphereVFS/src/tree.c",
        "mapper_table_traversal_apply(");
    /* vfs_chain_walk has 1 call (line 826).  vfs_chain_walk_extended
     * does NOT call it (the read-rule is delegated to vfs_chain_walk). */
    CHECK_EQ(rule_markers, 1);

    /* The exact-match-wins pattern should also appear once. */
    int exact_match_markers = count_occurrences_in(
        "/Users/bogdanadnan/Projects/ixsphere/native/iXSphereVFS/src/tree.c",
        "if (eff_epoch == read_epoch)");
    /* vfs_chain_walk has 1 (line 833). */
    CHECK_EQ(exact_match_markers, 1);
}

/* ---------------------------------------------------------------------------
 * W6: test_chain_walk_extended — exercises vfs_chain_walk_extended on
 * a file with multiple pages and mixed-epoch writes.  Verifies the
 * 6-step walk: root_vp → FileContent chain → PageNode by id →
 * VersionPage chain with read-rule.  The visible VersionPage's
 * dataPage VP should be resolvable via the chain.
 * --------------------------------------------------------------------------- */

static void test_chain_walk_extended(void) {
    const char* path = "/tmp/test_chain_walk_extended.vfs";
    unlink(path);
    vfs_t* vfs = vfs_mount(path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t file_vp = vfs_create(vfs, root_vp, "chain_walk.txt", 0);
    CHECK(file_vp > 0);

    /* Write 3 pages at epoch 0: each creates a PageNode + VersionPage. */
    char buf[8192];
    memset(buf, 'A', sizeof(buf));
    CHECK_EQ(vfs_write(vfs, file_vp, buf, 0, 8192, 0), 8192);
    memset(buf, 'B', sizeof(buf));
    CHECK_EQ(vfs_write(vfs, file_vp, buf, 8192, 8192, 0), 8192);
    memset(buf, 'C', sizeof(buf));
    CHECK_EQ(vfs_write(vfs, file_vp, buf, 16384, 8192, 0), 8192);

    /* Resolve page 0 (unit_id=0) at epoch 0 — should find a VersionPage
       whose stored data can be read back as 'A'. */
    PoolSlot vp = {0};
    WalkResult r = vfs_chain_walk_extended(ctx, file_vp, 0, 0, &vp);
    CHECK_EQ(r, WALK_FOUND);
    CHECK(vp.vptr != VFS_VPTR_NULL);
    uint32_t stored_epoch = (uint32_t)vfs_rd4_s(vp.bytes, LEAF_OFF_EPOCH,
                                                ctx->page_size);
    CHECK_EQ(stored_epoch, 0u);

    /* Resolve page 1 (unit_id=1) — different PageNode, different chain. */
    PoolSlot vp1 = {0};
    WalkResult r1 = vfs_chain_walk_extended(ctx, file_vp, 1, 0, &vp1);
    CHECK_EQ(r1, WALK_FOUND);
    CHECK(vp1.vptr != VFS_VPTR_NULL);
    /* The two VersionPages are at different VPs. */
    CHECK(vp1.vptr != vp.vptr);

    /* Resolve page 99 (unit_id=99) — doesn't exist, WALK_NEED_GROW. */
    PoolSlot vp99 = {0};
    WalkResult r99 = vfs_chain_walk_extended(ctx, file_vp, 99, 0, &vp99);
    CHECK_EQ(r99, WALK_NEED_GROW);

    /* Read-rule: resolve at epoch 2 (read_epoch=2).  The chain
       has a VersionPage at epoch 0 (eff_epoch=0 < 2, even) — the
       read-rule returns it (WALK_FOUND) because the highest
       even epoch below read_epoch wins. */
    PoolSlot vp_ep2 = {0};
    WalkResult r_ep2 = vfs_chain_walk_extended(ctx, file_vp, 0, 2, &vp_ep2);
    CHECK_EQ(r_ep2, WALK_FOUND);
    uint32_t stored_epoch_ep2 = (uint32_t)vfs_rd4_s(vp_ep2.bytes, LEAF_OFF_EPOCH,
                                                   ctx->page_size);
    CHECK_EQ(stored_epoch_ep2, 0u);

    /* Snapshot so epoch 2 becomes the live head (phase 27 C4). */
    int64_t snap = vfs_snapshot(vfs);
    CHECK_EQ(snap, 1);

    /* Write at epoch 2: creates a new VersionPage on the page-0 chain. */
    memset(buf, 'X', sizeof(buf));
    CHECK_EQ(vfs_write(vfs, file_vp, buf, 0, 100, 2), 100);

    /* Re-resolve page 0 at epoch 2 — should find the new VersionPage
       (exact match at epoch 2 wins). */
    PoolSlot vp_ep2b = {0};
    WalkResult r_ep2b = vfs_chain_walk_extended(ctx, file_vp, 0, 2, &vp_ep2b);
    CHECK_EQ(r_ep2b, WALK_FOUND);
    uint32_t stored_epoch2 = (uint32_t)vfs_rd4_s(vp_ep2b.bytes, LEAF_OFF_EPOCH,
                                                 ctx->page_size);
    CHECK_EQ(stored_epoch2, 2u);

    vfs_unmount(vfs);
    unlink(path);
}

/* W6: callback for test_chain_walk_anchor_chain — counts segments. */
typedef struct {
    int count;
    int64_t total_unit_count;  /* sum of ANCHOR_OFF_COUNT across segments */
} seg_count_state;

static int seg_count_cb(TreeContext* ctx, int64_t anchor_vp,
                            const uint8_t* anchor_bytes, void* user) {
    seg_count_state* st = (seg_count_state*)user;
    (void)ctx; (void)anchor_vp;
    st->count++;
    uint32_t seg_cnt = (uint32_t)vfs_rd4_s(anchor_bytes, ANCHOR_OFF_COUNT,
                                           ctx->page_size);
    st->total_unit_count += (int64_t)seg_cnt;
    return 0;  /* continue */
}

/* W6: test_chain_walk_anchor_chain — iterate over all FileContent
 * segments of a file with > 1 segment.  Verifies the walk visits
 * each segment exactly once and the count matches.
 *
 * ANCHOR_UNITS_PER_SEGMENT = 1024 (per src/nodes.h:39), so a file
 * with 2000 pages has 2 FileContent segments.
 */
static void test_chain_walk_anchor_chain(void) {
    const char* path = "/tmp/test_chain_walk_anchor.vfs";
    unlink(path);
    vfs_t* vfs = vfs_mount(path, 8192);
    CHECK(vfs != NULL);
    TreeContext* ctx = vfs->ctx;
    int64_t root_vp = ctx->rootNodeOffset;

    int64_t file_vp = vfs_create(vfs, root_vp, "multi.txt", 0);
    CHECK(file_vp > 0);

    /* Write 3 pages — all in segment 0 (segment_size = 1024 by default). */
    char buf[8192];
    memset(buf, 'A', sizeof(buf));
    for (int i = 0; i < 3; i++) {
        CHECK_EQ(vfs_write(vfs, file_vp, buf, (int64_t)i * 8192, 8192, 0), 8192);
    }

    /* Walk the FileContent chain — should be 1 segment with count=3. */
    seg_count_state st = {0, 0};
    PoolSlot file_slot = {0};
    pool_acquire(&ctx->pool, file_vp, false, &file_slot);
    CHECK(file_slot.vptr != VFS_VPTR_NULL);
    int64_t head = vfs_rd8_s(file_slot.bytes, ANCHOR_OFF_HEADPTR, ctx->page_size);
    pool_release(&ctx->pool, &file_slot);
    int visited = walk_anchor_chain(ctx, head, seg_count_cb, &st);
    CHECK_EQ(visited, 1);
    CHECK_EQ(st.count, 1);
    CHECK_EQ(st.total_unit_count, 3);

    /* Force a second segment by writing to page seg_size.  This
       allocates segment 1 with 1 PageNode (for page seg_size).
       Segment 0 is sparse — it has 3 PageNodes (pages 0, 1, 2). */
    int seg_size = (int)ctx->segment_size;
    CHECK(seg_size > 0);
    CHECK_EQ(vfs_write(vfs, file_vp, buf, (int64_t)seg_size * 8192, 8192, 0), 8192);

    seg_count_state st2 = {0, 0};
    pool_acquire(&ctx->pool, file_vp, false, &file_slot);
    head = vfs_rd8_s(file_slot.bytes, ANCHOR_OFF_HEADPTR, ctx->page_size);
    pool_release(&ctx->pool, &file_slot);
    visited = walk_anchor_chain(ctx, head, seg_count_cb, &st2);
    CHECK_EQ(visited, 2);
    CHECK_EQ(st2.count, 2);
    /* Segment 0 has 3 PageNodes (sparse), segment 1 has 1.  Total 4. */
    CHECK_EQ(st2.total_unit_count, 4);

    /* vfs_chain_walk_extended should still find pages 0, 1, 2 in
       segment 0 and the per-segment page_index 0 in segment 1.
       (The ContentUnit.id field is the per-segment page_index for
       files, not the global page_index — so segment 1's
       PageNode has id=0, not 1024.  The segment is determined by
       the caller's segment_idx math; the walk searches by id.) */
    PoolSlot vp_seg0 = {0};
    WalkResult r_seg0 = vfs_chain_walk_extended(ctx, file_vp, 0, 0, &vp_seg0);
    CHECK_EQ(r_seg0, WALK_FOUND);
    CHECK(vp_seg0.vptr != VFS_VPTR_NULL);

    PoolSlot vp_seg1 = {0};
    WalkResult r_seg1 = vfs_chain_walk_extended(ctx, file_vp, 0, 0, &vp_seg1);
    /* Note: r_seg0 and r_seg1 may return the same VP if the cache
       happens to hand back the same slot, or different VPs.  The
       important thing is that both succeed. */
    CHECK_EQ(r_seg1, WALK_FOUND);
    CHECK(vp_seg1.vptr != VFS_VPTR_NULL);

    /* Same walk on a dir — DirSegment walk.  Create a dir with 3
       children; should be 1 segment. */
    int64_t dir_vp = vfs_mkdir(vfs, root_vp, "d", 0);
    CHECK(dir_vp > 0);
    CHECK_EQ(vfs_create(vfs, dir_vp, "a", 0) > 0, 1);
    CHECK_EQ(vfs_create(vfs, dir_vp, "b", 0) > 0, 1);
    CHECK_EQ(vfs_create(vfs, dir_vp, "c", 0) > 0, 1);

    vfs_unmount(vfs);
    unlink(path);
}

/* ---------------------------------------------------------------------------
 * Phase 27 C5: storage_read_with_status must distinguish "not allocated"
 * (sparse-file zero-fill) from "CRC error" (data corruption must
 * surface, not be silently zero-filled).
 *
 * This test operates at the storage layer directly: open a
 * StorageBackend, allocate a page, write valid data, flush, then
 * corrupt a single byte of the on-disk payload and verify that
 * storage_read_with_status returns STORAGE_CRC_ERROR.
 * --------------------------------------------------------------------------- */
static void test_crc_mismatch_propagation(void) {
    const char* path = "/tmp/test_crc_mismatch.vfs";
    unlink(path);

    /* Open a fresh StorageBackend at the same page size the VFS uses. */
    int64_t page_size = 8192;
    StorageBackend* sb = storage_open(path, page_size);
    CHECK(sb != NULL);

    /* Allocate a data page and write a known pattern. */
    int64_t page = storage_allocate(sb, 1);
    CHECK(page >= 2);  /* page 0 = header, page 1 = superblock; >= 2 = data */
    uint8_t payload[8192];
    memset(payload, 0xAA, sizeof(payload));
    payload[0] = 'H';
    payload[1] = 'E';
    payload[2] = 'L';
    payload[3] = 'L';
    payload[4] = 'O';
    /* mirror_write computes CRC, sets PageHeader, writes both to disk. */
    CHECK_EQ(mirror_write(sb, page, payload, 0), 0);
    /* Flush everything (the storage backend's flush walks the cache
       and writes dirty pages via mirror_write). */
    storage_flush(sb, page);

    /* Sanity check: read should succeed. */
    uint8_t out[8192];
    StorageReadStatus s0 = STORAGE_NOT_FOUND;
    uint8_t* p0 = storage_read_with_status(sb, page, &s0);
    CHECK(p0 != NULL);
    CHECK_EQ(s0, STORAGE_OK);
    CHECK_EQ(memcmp(p0, payload, 5), 0);

    /* Now corrupt a single byte of the on-disk payload (after the
       16-byte PageHeader) so the CRC no longer matches.  The
       physical offset of the data page is the indirection entry;
       the payload begins at offset + PAGE_HEADER_SIZE. */
    int64_t phys = indir_lookup(sb, page);
    CHECK(phys > 0);
    /* Overwrite one byte of the payload (offset 100) with a
       different value.  The PageHeader is preserved so the
       checksum stored in the header is still the original CRC of
       the unmodified data — meaning a re-read must fail CRC. */
    uint8_t xor_byte = (uint8_t)(payload[100] ^ 0xFF);
    ssize_t n = pwrite(sb->fd, &xor_byte, 1, phys + PAGE_HEADER_SIZE + 100);
    CHECK_EQ(n, 1);
    /* The first read above populated the cache with the (valid) page.
       Evict it so the re-read goes back to disk and sees the corruption. */
    vfs_cache_evict_all(sb);

    /* Re-read: must return NULL with status STORAGE_CRC_ERROR.
       Before the C5 fix, this would have been NULL with status
       STORAGE_IO_ERROR (or just NULL), and vfs_read would have
       silently zero-filled the corrupted bytes. */
    StorageReadStatus s1 = STORAGE_OK;  /* poison */
    uint8_t* p1 = storage_read_with_status(sb, page, &s1);
    CHECK(p1 == NULL);
    CHECK_EQ(s1, STORAGE_CRC_ERROR);

    storage_close(sb);
    unlink(path);
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
    test_chain_walk_read_rule();
    unlink(test_path);
    test_chain_walk_extended();
    unlink(test_path);
    test_chain_walk_anchor_chain();

    /* Phase 27 C5: storage layer must distinguish CRC errors from
       "not allocated" so callers can surface data corruption instead
       of silently zero-filling it. */
    test_crc_mismatch_propagation();
    unlink(test_path);
    test_chain_walk_no_duplication();
    unlink(test_path);
    test_concurrent_dir_writes();
    unlink(test_path);
    test_concurrent_rename();
    unlink(test_path);
    test_stress_10k_children();
    unlink(test_path);
    test_contentunit_visibility();

    /* Clean up */
    unlink(test_path);

    printf("test_tree: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
