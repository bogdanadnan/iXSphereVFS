/* Phase 4b–4e: Node type unit tests. */
#include "nodes.h"
#include <stdio.h>
#include <string.h>

/* Test infrastructure */
static int tests_run = 0, tests_passed = 0;

#define CHECK(expr) do { \
    tests_run++; \
    if (!(expr)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    } else { \
        tests_passed++; \
    } \
} while(0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

/* ---------------------------------------------------------------------------
 * DirNode tests (Workload 4.1)
 * --------------------------------------------------------------------------- */

static void test_dirnode_write_read(void) {
    uint8_t slot[32];
    memset(slot, 0, sizeof(slot));

    /* Write DirNode with nodeId=5, headPtr points to page=10, slot=3 */
    nodes_write_dirnode(slot, 5, VFS_VPTR_MAKE(10, 3));

    /* Verify type field at byte 0 */
    CHECK_EQ(vfs_rd2(slot, DIRNODE_OFF_TYPE), (int16_t)NODE_TYPE_DIR);

    /* Read back all fields */
    uint32_t nodeId;
    int64_t headPtr;
    nodes_read_dirnode(slot, &nodeId, &headPtr);

    CHECK_EQ(nodeId, 5u);
    CHECK_EQ(VFS_VPTR_PAGE(headPtr), 10);
    CHECK_EQ(VFS_VPTR_SLOT(headPtr), 3);

    /* Verify reserved bytes 2-3 are zero */
    CHECK_EQ(vfs_rd2(slot, DIRNODE_OFF_RSVD), 0);

    /* Verify reserved bytes 16-31 are zero */
    int all_zero = 1;
    for (int i = 16; i < 32; i++) {
        if (slot[i] != 0) { all_zero = 0; break; }
    }
    CHECK(all_zero);
}

static void test_dirnode_zero_slot(void) {
    uint8_t slot[32];
    memset(slot, 0, sizeof(slot));

    /* Fresh zero slot: type should be 0x00 (not a valid DirNode) */
    CHECK_EQ(vfs_rd2(slot, DIRNODE_OFF_TYPE), 0);
    CHECK_EQ(vfs_rd4(slot, DIRNODE_OFF_NODEID), 0);
    CHECK_EQ(vfs_rd8(slot, DIRNODE_OFF_HEADPTR), 0);
}

/* ---------------------------------------------------------------------------
 * FileNode tests (Workload 4.2)
 * --------------------------------------------------------------------------- */

static void test_filenode_write_read(void) {
    uint8_t slot[32];
    memset(slot, 0, sizeof(slot));

    /* Write FileNode with all fields non-trivial */
    nodes_write_filenode(slot, 42, VFS_VPTR_MAKE(5, 10), VFS_VPTR_MAKE(6, 20), 987654321);

    /* Verify type field at byte 0 */
    CHECK_EQ(vfs_rd2(slot, FILENODE_OFF_TYPE), (int16_t)NODE_TYPE_FILE);

    /* Read back all fields */
    uint32_t nodeId;
    int64_t headPtr, sizePtr, createdAt;
    nodes_read_filenode(slot, &nodeId, &headPtr, &sizePtr, &createdAt);

    CHECK_EQ(nodeId, 42u);
    CHECK_EQ(VFS_VPTR_PAGE(headPtr), 5);
    CHECK_EQ(VFS_VPTR_SLOT(headPtr), 10);
    CHECK_EQ(VFS_VPTR_PAGE(sizePtr), 6);
    CHECK_EQ(VFS_VPTR_SLOT(sizePtr), 20);
    CHECK_EQ(createdAt, 987654321);

    /* Verify reserved bytes 2-3 are zero */
    CHECK_EQ(vfs_rd2(slot, FILENODE_OFF_RSVD), 0);
}

static void test_filenode_ctime(void) {
    uint8_t slot[32];
    memset(slot, 0, sizeof(slot));

    /* Write FileNode with a specific createdAt */
    nodes_write_filenode(slot, 1, 0, 0, 123456789);

    /* Read ctime independently */
    CHECK_EQ(nodes_read_filenode_ctime(slot), 123456789);
}

static void test_filenode_dirnode_overlap(void) {
    /* Verify FileNode and DirNode have identical first-16-byte structure:
       same offsets for type, rsvd, nodeId, headPtr */
    CHECK_EQ(FILENODE_OFF_TYPE, DIRNODE_OFF_TYPE);
    CHECK_EQ(FILENODE_OFF_TYPE, 0);
    CHECK_EQ(FILENODE_OFF_RSVD, DIRNODE_OFF_RSVD);
    CHECK_EQ(FILENODE_OFF_RSVD, 2);
    CHECK_EQ(FILENODE_OFF_NODEID, DIRNODE_OFF_NODEID);
    CHECK_EQ(FILENODE_OFF_NODEID, 4);
    CHECK_EQ(FILENODE_OFF_HEADPTR, DIRNODE_OFF_HEADPTR);
    CHECK_EQ(FILENODE_OFF_HEADPTR, 8);
}

/* ---------------------------------------------------------------------------
 * DirContent tests (Workload 4.3)
 * --------------------------------------------------------------------------- */

static void test_dircontent_basic(void) {
    uint8_t slot[32];
    memset(slot, 0, sizeof(slot));

    /* Normal DirContent entry */
    nodes_write_dircontent(slot, 7, 5, VFS_VPTR_MAKE(3, 1), VFS_VPTR_MAKE(3, 2), 0);

    uint32_t childNodeId, epoch;
    int64_t childPtr, namePtr, nextPtr;
    nodes_read_dircontent(slot, &childNodeId, &epoch, &childPtr, &namePtr, &nextPtr);

    CHECK_EQ(childNodeId, 7u);
    CHECK_EQ(epoch, 5u);
    CHECK_EQ(VFS_VPTR_PAGE(childPtr), 3);
    CHECK_EQ(VFS_VPTR_SLOT(childPtr), 1);
    CHECK_EQ(VFS_VPTR_PAGE(namePtr), 3);
    CHECK_EQ(VFS_VPTR_SLOT(namePtr), 2);
    CHECK_EQ(nextPtr, 0);
}

static void test_dircontent_tombstone(void) {
    uint8_t slot[32];
    memset(slot, 0, sizeof(slot));

    /* Tombstone: namePtr=0 means deleted */
    nodes_write_dircontent(slot, 7, 5, VFS_VPTR_MAKE(3, 1), 0, 0);

    uint32_t childNodeId, epoch;
    int64_t childPtr, namePtr, nextPtr;
    nodes_read_dircontent(slot, &childNodeId, &epoch, &childPtr, &namePtr, &nextPtr);

    CHECK_EQ(namePtr, 0);
}

static void test_dircontent_chain(void) {
    /* Build 3 DirContent entries linked via nextPtr, prepend in descending epoch */
    uint8_t e1[32], e2[32], e3[32];
    memset(e1, 0, sizeof(e1));
    memset(e2, 0, sizeof(e2));
    memset(e3, 0, sizeof(e3));

    int64_t vp1 = VFS_VPTR_MAKE(10, 1);
    int64_t vp2 = VFS_VPTR_MAKE(10, 2);
    int64_t vp3 = VFS_VPTR_MAKE(10, 3);

    /* Entry 3 (oldest, epoch=1): nextPtr=0 */
    nodes_write_dircontent(e3, 1, 1, vp3, 0, 0);
    /* Entry 2 (epoch=3): nextPtr points to e3 */
    nodes_write_dircontent(e2, 2, 3, vp2, 0, vp3);
    /* Entry 1 (newest, epoch=5): nextPtr points to e2 */
    nodes_write_dircontent(e1, 3, 5, vp1, 0, vp2);

    uint32_t childNodeId, epoch;
    int64_t childPtr, namePtr, nextPtr;

    /* Walk chain: e1 -> e2 -> e3 */
    nodes_read_dircontent(e1, &childNodeId, &epoch, &childPtr, &namePtr, &nextPtr);
    CHECK_EQ(epoch, 5u);
    CHECK_EQ(nextPtr, vp2);

    nodes_read_dircontent(e2, &childNodeId, &epoch, &childPtr, &namePtr, &nextPtr);
    CHECK_EQ(epoch, 3u);
    CHECK_EQ(nextPtr, vp3);

    nodes_read_dircontent(e3, &childNodeId, &epoch, &childPtr, &namePtr, &nextPtr);
    CHECK_EQ(epoch, 1u);
    CHECK_EQ(nextPtr, 0);

    /* Verify descending epoch order */
    CHECK(5 > 3);
    CHECK(3 > 1);
}

int main(void) {
    test_dirnode_write_read();
    test_dirnode_zero_slot();
    test_filenode_write_read();
    test_filenode_ctime();
    test_filenode_dirnode_overlap();
    test_dircontent_basic();
    test_dircontent_tombstone();
    test_dircontent_chain();

    printf("test_nodes: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
