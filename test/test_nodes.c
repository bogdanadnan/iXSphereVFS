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

int main(void) {
    test_dirnode_write_read();
    test_dirnode_zero_slot();

    printf("test_nodes: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
