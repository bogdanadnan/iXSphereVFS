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

int main(void) {
    test_dirnode_write_read();
    test_dirnode_zero_slot();
    test_filenode_write_read();
    test_filenode_ctime();
    test_filenode_dirnode_overlap();

    printf("test_nodes: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
