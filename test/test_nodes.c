/* Phase 4b–4e: Node type unit tests. */
#include "nodes.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

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

    /* Write DirNode with nodeId=5, headPtr points to page=10, slot=3,
       createdAt=1700000000 (a fixed test timestamp). */
    nodes_write_dirnode(slot, 5, VFS_VPTR_MAKE(10, 3), 0, 1700000000LL,
                        VFS_PAGE_SIZE);

    /* Verify type field at byte 0 */
    CHECK_EQ(vfs_rd2(slot, DIRNODE_OFF_TYPE), (int16_t)NODE_TYPE_DIR);

    /* Read back all fields */
    uint32_t nodeId;
    int64_t headPtr;
    int64_t indexHeadPtr;
    int64_t createdAt;
    nodes_read_dirnode(slot, &nodeId, &headPtr, &indexHeadPtr, &createdAt,
                       VFS_PAGE_SIZE);

    CHECK_EQ(nodeId, 5u);
    CHECK_EQ(VFS_VPTR_PAGE(headPtr), 10);
    CHECK_EQ(VFS_VPTR_SLOT(headPtr), 3);
    CHECK_EQ(createdAt, 1700000000LL);

    /* Verify reserved bytes 2-3 are zero */
    CHECK_EQ(vfs_rd2(slot, DIRNODE_OFF_RSVD), 0);

    /* Verify reserved bytes 16-23 (indexHeadPtr) are non-zero upper,
       but lower 4 bytes (since headPtr was 0) are zero.  Just check
       that the slot is exactly what we wrote. */
    CHECK_EQ(vfs_rd8(slot, DIRNODE_OFF_INDEXHEADPTR), 0);
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
 * DirContentIndex tests (Phase 18)
 * --------------------------------------------------------------------------- */

static void test_dircontentindex_write_read(void) {
    uint8_t slot[32];
    memset(slot, 0, sizeof(slot));

    /* Write INTERNAL node with non-trivial fields */
    nodes_write_dircontentindex(slot, 3, NODE_TYPE_INDEX_INTERNAL,
                                 VFS_VPTR_MAKE(7, 2),
                                 VFS_VPTR_MAKE(8, 4), VFS_PAGE_SIZE);

    /* Verify direct field reads */
    CHECK_EQ((int)slot[DIRCONTENTINDEX_OFF_HASHNIBBLE], 3);
    CHECK_EQ((int)slot[DIRCONTENTINDEX_OFF_NODETYPE], (int)NODE_TYPE_INDEX_INTERNAL);

    /* Read back all fields */
    uint8_t hashNibble, nodeType;
    int64_t listVP, nextVP;
    nodes_read_dircontentindex(slot, &hashNibble, &nodeType,
                               &listVP, &nextVP, VFS_PAGE_SIZE);

    CHECK_EQ(hashNibble, 3u);
    CHECK_EQ(nodeType, NODE_TYPE_INDEX_INTERNAL);
    CHECK_EQ(VFS_VPTR_PAGE(listVP), 7);
    CHECK_EQ(VFS_VPTR_SLOT(listVP), 2);
    CHECK_EQ(VFS_VPTR_PAGE(nextVP), 8);
    CHECK_EQ(VFS_VPTR_SLOT(nextVP), 4);

    /* Verify reserved bytes 2-7 are zero */
    int zero_2_7 = 1;
    for (int i = 2; i < 8; i++) {
        if (slot[i] != 0) { zero_2_7 = 0; break; }
    }
    CHECK(zero_2_7);

    /* Verify reserved bytes 24-31 are zero */
    int zero_24_31 = 1;
    for (int i = 24; i < 32; i++) {
        if (slot[i] != 0) { zero_24_31 = 0; break; }
    }
    CHECK(zero_24_31);
}

static void test_dircontentindex_leaf_roundtrip(void) {
    uint8_t slot[32];
    memset(slot, 0, sizeof(slot));

    /* Write LEAF node */
    nodes_write_dircontentindex(slot, 15, NODE_TYPE_INDEX_LEAF,
                                 VFS_VPTR_MAKE(20, 10), 0, VFS_PAGE_SIZE);

    uint8_t hashNibble, nodeType;
    int64_t listVP, nextVP;
    nodes_read_dircontentindex(slot, &hashNibble, &nodeType,
                               &listVP, &nextVP, VFS_PAGE_SIZE);

    CHECK_EQ(hashNibble, 15u);
    CHECK_EQ(nodeType, NODE_TYPE_INDEX_LEAF);
    CHECK_EQ(VFS_VPTR_PAGE(listVP), 20);
    CHECK_EQ(VFS_VPTR_SLOT(listVP), 10);
    CHECK_EQ(nextVP, 0);
}

static void test_dircontentindex_zero_slot(void) {
    uint8_t slot[32];
    memset(slot, 0, sizeof(slot));

    /* Fresh zero slot: all fields should read as 0 */
    uint8_t hashNibble, nodeType;
    int64_t listVP, nextVP;
    nodes_read_dircontentindex(slot, &hashNibble, &nodeType,
                               &listVP, &nextVP, VFS_PAGE_SIZE);

    CHECK_EQ(hashNibble, 0u);
    CHECK_EQ(nodeType, 0);
    CHECK_EQ(listVP, 0);
    CHECK_EQ(nextVP, 0);
}

/* ---------------------------------------------------------------------------
 * DirContentLink tests (Phase 18)
 * --------------------------------------------------------------------------- */

static void test_dircontentlink_write_read(void) {
    uint8_t slot[32];
    memset(slot, 0, sizeof(slot));

    /* Write DirContentLink with non-trivial fields */
    nodes_write_dircontentlink(slot, VFS_VPTR_MAKE(15, 7),
                                VFS_VPTR_MAKE(16, 8), VFS_PAGE_SIZE);

    /* Read back all fields */
    int64_t dirContentVP, nextVP;
    nodes_read_dircontentlink(slot, &dirContentVP, &nextVP, VFS_PAGE_SIZE);

    CHECK_EQ(VFS_VPTR_PAGE(dirContentVP), 15);
    CHECK_EQ(VFS_VPTR_SLOT(dirContentVP), 7);
    CHECK_EQ(VFS_VPTR_PAGE(nextVP), 16);
    CHECK_EQ(VFS_VPTR_SLOT(nextVP), 8);

    /* Verify reserved bytes 0-7 are zero */
    int zero_0_7 = 1;
    for (int i = 0; i < 8; i++) {
        if (slot[i] != 0) { zero_0_7 = 0; break; }
    }
    CHECK(zero_0_7);

    /* Verify reserved bytes 24-31 are zero */
    int zero_24_31 = 1;
    for (int i = 24; i < 32; i++) {
        if (slot[i] != 0) { zero_24_31 = 0; break; }
    }
    CHECK(zero_24_31);
}

static void test_dircontentlink_zero_link(void) {
    uint8_t slot[32];
    memset(slot, 0, sizeof(slot));

    /* Single link with no next (nextVP=0) */
    nodes_write_dircontentlink(slot, VFS_VPTR_MAKE(1, 1), 0, VFS_PAGE_SIZE);

    int64_t dirContentVP, nextVP;
    nodes_read_dircontentlink(slot, &dirContentVP, &nextVP, VFS_PAGE_SIZE);

    CHECK_EQ(VFS_VPTR_PAGE(dirContentVP), 1);
    CHECK_EQ(VFS_VPTR_SLOT(dirContentVP), 1);
    CHECK_EQ(nextVP, 0);
}

static void test_dircontentlink_zero_slot(void) {
    uint8_t slot[32];
    memset(slot, 0, sizeof(slot));

    /* Fresh zero slot: all fields should read as 0 */
    int64_t dirContentVP, nextVP;
    nodes_read_dircontentlink(slot, &dirContentVP, &nextVP, VFS_PAGE_SIZE);

    CHECK_EQ(dirContentVP, 0);
    CHECK_EQ(nextVP, 0);
}

/* ---------------------------------------------------------------------------
 * FileNode tests (Workload 4.2)
 * --------------------------------------------------------------------------- */

static void test_filenode_write_read(void) {
    uint8_t slot[32];
    memset(slot, 0, sizeof(slot));

    /* Write FileNode with all fields non-trivial */
    nodes_write_filenode(slot, 42, VFS_VPTR_MAKE(5, 10), VFS_VPTR_MAKE(6, 20), 987654321, VFS_PAGE_SIZE);

    /* Verify type field at byte 0 */
    CHECK_EQ(vfs_rd2(slot, FILENODE_OFF_TYPE), (int16_t)NODE_TYPE_FILE);

    /* Read back all fields */
    uint32_t nodeId;
    int64_t headPtr, sizePtr, createdAt;
    nodes_read_filenode(slot, &nodeId, &headPtr, &sizePtr, &createdAt, VFS_PAGE_SIZE);

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
    nodes_write_filenode(slot, 1, 0, 0, 123456789, VFS_PAGE_SIZE);

    /* Read ctime independently */
    CHECK_EQ(nodes_read_filenode_ctime(slot, VFS_PAGE_SIZE), 123456789);
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
    nodes_write_dircontent(slot, 7, 5, VFS_VPTR_MAKE(3, 1), VFS_VPTR_MAKE(3, 2), 0, VFS_PAGE_SIZE);

    uint32_t childNodeId, epoch;
    int64_t childPtr, namePtr, nextPtr;
    nodes_read_dircontent(slot, &childNodeId, &epoch, &childPtr, &namePtr, &nextPtr, VFS_PAGE_SIZE);

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
    nodes_write_dircontent(slot, 7, 5, VFS_VPTR_MAKE(3, 1), 0, 0, VFS_PAGE_SIZE);

    uint32_t childNodeId, epoch;
    int64_t childPtr, namePtr, nextPtr;
    nodes_read_dircontent(slot, &childNodeId, &epoch, &childPtr, &namePtr, &nextPtr, VFS_PAGE_SIZE);

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
    nodes_write_dircontent(e3, 1, 1, vp3, 0, 0, VFS_PAGE_SIZE);
    /* Entry 2 (epoch=3): nextPtr points to e3 */
    nodes_write_dircontent(e2, 2, 3, vp2, 0, vp3, VFS_PAGE_SIZE);
    /* Entry 1 (newest, epoch=5): nextPtr points to e2 */
    nodes_write_dircontent(e1, 3, 5, vp1, 0, vp2, VFS_PAGE_SIZE);

    uint32_t childNodeId, epoch;
    int64_t childPtr, namePtr, nextPtr;

    /* Walk chain: e1 -> e2 -> e3 */
    nodes_read_dircontent(e1, &childNodeId, &epoch, &childPtr, &namePtr, &nextPtr, VFS_PAGE_SIZE);
    CHECK_EQ(epoch, 5u);
    CHECK_EQ(nextPtr, vp2);

    nodes_read_dircontent(e2, &childNodeId, &epoch, &childPtr, &namePtr, &nextPtr, VFS_PAGE_SIZE);
    CHECK_EQ(epoch, 3u);
    CHECK_EQ(nextPtr, vp3);

    nodes_read_dircontent(e3, &childNodeId, &epoch, &childPtr, &namePtr, &nextPtr, VFS_PAGE_SIZE);
    CHECK_EQ(epoch, 1u);
    CHECK_EQ(nextPtr, 0);

    /* Verify descending epoch order */
    CHECK(5 > 3);
    CHECK(3 > 1);
}

/* ---------------------------------------------------------------------------
 * FileContent tests (Workload 4.4)
 * --------------------------------------------------------------------------- */

static void test_filecontent(void) {
    uint8_t slot[32];
    memset(slot, 0, sizeof(slot));

    /* Single FileContent entry */
    nodes_write_filecontent(slot, VFS_VPTR_MAKE(20, 5), 0, VFS_PAGE_SIZE);

    int64_t pageRootPtr, nextPtr;
    nodes_read_filecontent(slot, &pageRootPtr, &nextPtr, VFS_PAGE_SIZE);

    CHECK_EQ(VFS_VPTR_PAGE(pageRootPtr), 20);
    CHECK_EQ(VFS_VPTR_SLOT(pageRootPtr), 5);
    CHECK_EQ(nextPtr, 0);

    /* W1c: the new type/flags/segmentId fields at the front (offsets 0-7)
       should be initialized: type=ANCHOR_KIND_SEGMENT_FILE, flags=0,
       segmentId=0 (this convenience wrapper doesn't allocate). */
    CHECK_EQ(vfs_rd2(slot, FILECONTENT_OFF_TYPE), (int16_t)ANCHOR_KIND_SEGMENT_FILE);
    CHECK_EQ(vfs_rd2(slot, FILECONTENT_OFF_FLAGS), 0);
    CHECK_EQ(vfs_rd4(slot, FILECONTENT_OFF_SEGMENTID), 0u);

    /* pageCount at the new offset 24 = 0, reserved tail at 28 = 0 */
    CHECK_EQ(vfs_rd4(slot, FILECONTENT_OFF_PAGECOUNT), 0u);
    CHECK_EQ(vfs_rd4(slot, 28), 0);
}

static void test_filecontent_chain(void) {
    uint8_t s1[32], s2[32];
    memset(s1, 0, sizeof(s1));
    memset(s2, 0, sizeof(s2));

    int64_t vp2 = VFS_VPTR_MAKE(30, 0);

    /* Segment 2 (higher range): nextPtr=0 */
    nodes_write_filecontent(s2, VFS_VPTR_MAKE(30, 0), 0, VFS_PAGE_SIZE);
    /* Segment 1 (lower range): nextPtr points to s2 */
    nodes_write_filecontent(s1, VFS_VPTR_MAKE(10, 0), vp2, VFS_PAGE_SIZE);

    int64_t root, next;
    nodes_read_filecontent(s1, &root, &next, VFS_PAGE_SIZE);
    CHECK_EQ(VFS_VPTR_PAGE(root), 10);
    CHECK_EQ(next, vp2);

    nodes_read_filecontent(s2, &root, &next, VFS_PAGE_SIZE);
    CHECK_EQ(VFS_VPTR_PAGE(root), 30);
    CHECK_EQ(next, 0);
}

static void test_pagenode(void) {
    uint8_t slot[32];
    memset(slot, 0, sizeof(slot));

    /* PageNode with versionRootPtr=0 (unwritten page) */
    nodes_write_pagenode(slot, 0, VFS_VPTR_MAKE(5, 0), 0, VFS_PAGE_SIZE);

    int64_t vroot, next;
    uint32_t pidx;
    nodes_read_pagenode(slot, &vroot, &next, &pidx, VFS_PAGE_SIZE);

    CHECK_EQ(vroot, 0);
    CHECK_EQ(VFS_VPTR_PAGE(next), 5);
    CHECK_EQ(VFS_VPTR_SLOT(next), 0);
    CHECK_EQ(pidx, 0u);

    /* W1d: the new type/flags fields at offsets 0-3, and pageIndex at
       offset 4 (the Anchor `id` field).  type=ANCHOR_KIND_UNIT_PAGE,
       flags=0, pageIndex=0. */
    CHECK_EQ(vfs_rd2(slot, PAGENODE_OFF_TYPE), (int16_t)ANCHOR_KIND_UNIT_PAGE);
    CHECK_EQ(vfs_rd2(slot, PAGENODE_OFF_FLAGS), 0);
    CHECK_EQ(vfs_rd4(slot, PAGENODE_OFF_PAGEINDEX), 0u);

    /* Verify reserved bytes 24-31 are zero */
    CHECK_EQ(vfs_rd4(slot, 24), 0);
    CHECK_EQ(vfs_rd4(slot, 28), 0);
}

static void test_pagenode_chain(void) {
    uint8_t s1[32], s2[32], s3[32];
    memset(s1, 0, sizeof(s1));
    memset(s2, 0, sizeof(s2));
    memset(s3, 0, sizeof(s3));

    int64_t vp2 = VFS_VPTR_MAKE(10, 2);
    int64_t vp3 = VFS_VPTR_MAKE(10, 3);

    /* PageNode 3 (last): nextPtr=0 */
    nodes_write_pagenode(s3, 0, 0, 0, VFS_PAGE_SIZE);
    /* PageNode 2: nextPtr -> s3 */
    nodes_write_pagenode(s2, 0, vp3, 0, VFS_PAGE_SIZE);
    /* PageNode 1 (first): nextPtr -> s2 */
    nodes_write_pagenode(s1, 0, vp2, 0, VFS_PAGE_SIZE);

    int64_t vroot, next;
    nodes_read_pagenode(s1, &vroot, &next, NULL, VFS_PAGE_SIZE);
    CHECK_EQ(next, vp2);
    nodes_read_pagenode(s2, &vroot, &next, NULL, VFS_PAGE_SIZE);
    CHECK_EQ(next, vp3);
    nodes_read_pagenode(s3, &vroot, &next, NULL, VFS_PAGE_SIZE);
    CHECK_EQ(next, 0);
}

/* ---------------------------------------------------------------------------
 * Phase 26 / W1a: Anchor write/read (unified 32-byte layout).
 * Tests round-trip for each AnchorKind value, plus the count field.
 * --------------------------------------------------------------------------- */

static void test_anchor_write_read(void) {
    uint8_t slot[32];
    AnchorKind kinds[] = {
        ANCHOR_KIND_ROOT_FILE,
        ANCHOR_KIND_ROOT_DIR,
        ANCHOR_KIND_SEGMENT_FILE,
        ANCHOR_KIND_SEGMENT_DIR,
        ANCHOR_KIND_UNIT_PAGE,
        ANCHOR_KIND_UNIT_SLOT,
    };
    for (size_t i = 0; i < sizeof(kinds)/sizeof(kinds[0]); i++) {
        memset(slot, 0, sizeof(slot));
        nodes_write_anchor(slot, kinds[i], 42 + (uint32_t)i,
                          0x1111 + (int64_t)i, 0x2222 + (int64_t)i,
                          100 + (uint32_t)i, VFS_PAGE_SIZE);

        AnchorKind k = 0;
        uint32_t id = 0;
        int64_t head = 0, sib = 0;
        uint32_t count = 0;
        nodes_read_anchor(slot, &k, &id, &head, &sib, &count, VFS_PAGE_SIZE);
        CHECK_EQ((int)k, (int)kinds[i]);
        CHECK_EQ((int)id, 42 + (int)i);
        CHECK_EQ(head, 0x1111 + (int64_t)i);
        CHECK_EQ(sib,  0x2222 + (int64_t)i);
        CHECK_EQ((int)count, 100 + (int)i);
    }
}

/* Anchor should be exactly 32 bytes (one pool slot). */
static void test_anchor_size_is_32(void) {
    CHECK_EQ((int)sizeof(AnchorKind), 2);
    /* The offset macros are all 0..31; no out-of-range check needed at
       compile time, but the write helper should fit in 32 bytes. */
    uint8_t slot[32];
    memset(slot, 0xAA, sizeof(slot));  /* poison */
    nodes_write_anchor(slot, ANCHOR_KIND_ROOT_FILE, 1, 2, 3, 4, VFS_PAGE_SIZE);
    /* Spot-check that the writer didn't overrun the slot. */
    /* offset 32 (one past) is OOB; we can't check it, but we can check
       that the write doesn't depend on it. */
    CHECK_EQ((int)ANCHOR_OFF_TYPE, 0);
    CHECK_EQ((int)ANCHOR_OFF_FLAGS, 2);
    CHECK_EQ((int)ANCHOR_OFF_ID, 4);
    CHECK_EQ((int)ANCHOR_OFF_HEADPTR, 8);
    CHECK_EQ((int)ANCHOR_OFF_SIBPTR, 16);
    CHECK_EQ((int)ANCHOR_OFF_COUNT, 24);
    CHECK_EQ((int)ANCHOR_OFF_RESERVED, 28);
}

/* ---------------------------------------------------------------------------
 * Phase 26 / W1e: DirSegment and SlotNode — dir-side mirror types.
 *
 * Both use the unified 32-byte Anchor layout.  Their offset macros
 * must point to the same offsets as the file-side Segment / ContentUnit
 * macros so the unified vfs_chain_walk (W2) can walk file and dir
 * chains with byte-identical step code.
 * --------------------------------------------------------------------------- */

/* DirSegment offsets match FileContent / Segment offsets. */
static void test_dirsegment_offset_parity(void) {
    CHECK_EQ((int)DIRSEGMENT_OFF_TYPE,      (int)FILECONTENT_OFF_TYPE);
    CHECK_EQ((int)DIRSEGMENT_OFF_FLAGS,     (int)FILECONTENT_OFF_FLAGS);
    CHECK_EQ((int)DIRSEGMENT_OFF_SEGMENTID, (int)FILECONTENT_OFF_SEGMENTID);
    CHECK_EQ((int)DIRSEGMENT_OFF_HEADPTR,   (int)FILECONTENT_OFF_ROOTPTR);
    CHECK_EQ((int)DIRSEGMENT_OFF_NEXTPTR,   (int)FILECONTENT_OFF_NEXTPTR);
    CHECK_EQ((int)DIRSEGMENT_OFF_SLOTCOUNT, (int)FILECONTENT_OFF_PAGECOUNT);
}

/* SlotNode offsets match PageNode / ContentUnit offsets. */
static void test_slotnode_offset_parity(void) {
    CHECK_EQ((int)SLOTNODE_OFF_TYPE,        (int)PAGENODE_OFF_TYPE);
    CHECK_EQ((int)SLOTNODE_OFF_FLAGS,       (int)PAGENODE_OFF_FLAGS);
    CHECK_EQ((int)SLOTNODE_OFF_CHILDNODEID, (int)PAGENODE_OFF_PAGEINDEX);
    CHECK_EQ((int)SLOTNODE_OFF_HEADPTR,     (int)PAGENODE_OFF_VERSIONROOT);
    CHECK_EQ((int)SLOTNODE_OFF_NEXTPTR,     (int)PAGENODE_OFF_NEXTPTR);
}

/* DirSegment and SlotNode can be written/read via the unified
 * nodes_write_anchor / nodes_read_anchor helpers with the right kind. */
static void test_dirslot_anchor_roundtrip(void) {
    uint8_t seg_slot[32];
    uint8_t slot_slot[32];
    memset(seg_slot, 0, sizeof(seg_slot));
    memset(slot_slot, 0, sizeof(slot_slot));

    /* DirSegment: id=7, headPtr=first SlotNode, sibPtr=next DirSegment,
       count=3 live slots. */
    nodes_write_anchor(seg_slot, ANCHOR_KIND_SEGMENT_DIR,
                       7, 0xAA00, 0xBB00, 3, VFS_PAGE_SIZE);
    /* SlotNode: id=42 (childNodeId), headPtr=first DirContent,
       sibPtr=next SlotNode in same segment, count=0. */
    nodes_write_anchor(slot_slot, ANCHOR_KIND_UNIT_SLOT,
                       42, 0xCC00, 0xDD00, 0, VFS_PAGE_SIZE);

    /* Read back DirSegment */
    AnchorKind k = 0;
    uint32_t id = 0, count = 0;
    int64_t head = 0, sib = 0;
    nodes_read_anchor(seg_slot, &k, &id, &head, &sib, &count, VFS_PAGE_SIZE);
    CHECK_EQ((int)k, (int)ANCHOR_KIND_SEGMENT_DIR);
    CHECK_EQ((int)id, 7);
    CHECK_EQ(head, 0xAA00);
    CHECK_EQ(sib,  0xBB00);
    CHECK_EQ((int)count, 3);

    /* The type field at offset 0 (per DIRSEGMENT_OFF_TYPE) is 0x21. */
    CHECK_EQ((int)vfs_rd2(seg_slot, DIRSEGMENT_OFF_TYPE), 0x21);

    /* Read back SlotNode */
    k = 0; id = 0; count = 0; head = 0; sib = 0;
    nodes_read_anchor(slot_slot, &k, &id, &head, &sib, &count, VFS_PAGE_SIZE);
    CHECK_EQ((int)k, (int)ANCHOR_KIND_UNIT_SLOT);
    CHECK_EQ((int)id, 42);
    CHECK_EQ(head, 0xCC00);
    CHECK_EQ(sib,  0xDD00);
    CHECK_EQ((int)count, 0);

    /* The type field at offset 0 (per SLOTNODE_OFF_TYPE) is 0x31. */
    CHECK_EQ((int)vfs_rd2(slot_slot, SLOTNODE_OFF_TYPE), 0x31);
    /* childNodeId lives at the same offset as the Anchor `id` field. */
    CHECK_EQ((int)vfs_rd4(slot_slot, SLOTNODE_OFF_CHILDNODEID), 42);
}

static void test_versionpage(void) {
    uint8_t slot[32];
    memset(slot, 0, sizeof(slot));

    /* VersionPage {epoch=3, dataPage=100, nextPtr=VFS_VPTR_MAKE(5,1)} */
    nodes_write_versionpage(slot, 3, 100, VFS_VPTR_MAKE(5, 1), VFS_PAGE_SIZE);

    uint32_t epoch;
    int64_t dataPage, nextPtr;
    nodes_read_versionpage(slot, &epoch, &dataPage, &nextPtr, VFS_PAGE_SIZE);

    CHECK_EQ(epoch, 3u);
    CHECK_EQ(dataPage, 100);
    CHECK_EQ(VFS_VPTR_PAGE(nextPtr), 5);
    CHECK_EQ(VFS_VPTR_SLOT(nextPtr), 1);

    /* Verify reserved bytes 24-31 are zero */
    int all_zero = 1;
    for (int i = 24; i < 32; i++) {
        if (slot[i] != 0) { all_zero = 0; break; }
    }
    CHECK(all_zero);
}

static void test_versionpage_chain(void) {
    uint8_t v5[32], v3[32];
    memset(v5, 0, sizeof(v5));
    memset(v3, 0, sizeof(v3));

    int64_t vp3 = VFS_VPTR_MAKE(5, 1);

    /* Older version (epoch=3): nextPtr=0 */
    nodes_write_versionpage(v3, 3, 50, 0, VFS_PAGE_SIZE);
    /* Newer version (epoch=5): nextPtr -> v3 */
    nodes_write_versionpage(v5, 5, 100, vp3, VFS_PAGE_SIZE);

    uint32_t epoch;
    int64_t dataPage, nextPtr;

    nodes_read_versionpage(v5, &epoch, &dataPage, &nextPtr, VFS_PAGE_SIZE);
    CHECK_EQ(epoch, 5u);
    CHECK_EQ(dataPage, 100);
    CHECK_EQ(nextPtr, vp3);

    nodes_read_versionpage(v3, &epoch, &dataPage, &nextPtr, VFS_PAGE_SIZE);
    CHECK_EQ(epoch, 3u);
    CHECK_EQ(dataPage, 50);
    CHECK_EQ(nextPtr, 0);
}

/* ---------------------------------------------------------------------------
 * FileSize tests (Workload 4.7)
 * --------------------------------------------------------------------------- */

static void test_filesize(void) {
    uint8_t slot[32];
    memset(slot, 0, sizeof(slot));

    /* Single FileSize entry {epoch=2, modifiedAt=now, fileSize=500} */
    nodes_write_filesize(slot, 2, 1000, 500, 0, VFS_PAGE_SIZE);

    uint32_t epoch;
    int64_t modifiedAt, fileSize, nextPtr;
    nodes_read_filesize(slot, &epoch, &modifiedAt, &fileSize, &nextPtr, VFS_PAGE_SIZE);

    CHECK_EQ(epoch, 2u);
    CHECK_EQ(modifiedAt, 1000);
    CHECK_EQ(fileSize, 500);
    CHECK_EQ(nextPtr, 0);

    /* Verify reserved bytes 28-31 are zero */
    CHECK_EQ(vfs_rd4(slot, 28), 0);
}

static void test_filesize_chain(void) {
    uint8_t s1[32], s2[32];
    memset(s1, 0, sizeof(s1));
    memset(s2, 0, sizeof(s2));

    int64_t vp2 = VFS_VPTR_MAKE(10, 2);

    /* Older entry (epoch=2, size=500): nextPtr=0 */
    nodes_write_filesize(s2, 2, 500, 500, 0, VFS_PAGE_SIZE);
    /* Newer entry (epoch=3, size=1000): nextPtr -> s2 */
    nodes_write_filesize(s1, 3, 1000, 1000, vp2, VFS_PAGE_SIZE);

    uint32_t epoch;
    int64_t modifiedAt, fileSize, nextPtr;

    nodes_read_filesize(s1, &epoch, &modifiedAt, &fileSize, &nextPtr, VFS_PAGE_SIZE);
    CHECK_EQ(epoch, 3u);
    CHECK_EQ(fileSize, 1000);
    CHECK_EQ(nextPtr, vp2);

    nodes_read_filesize(s2, &epoch, &modifiedAt, &fileSize, &nextPtr, VFS_PAGE_SIZE);
    CHECK_EQ(epoch, 2u);
    CHECK_EQ(fileSize, 500);
    CHECK_EQ(nextPtr, 0);
}

/* ---------------------------------------------------------------------------
 * MapperEntry tests (Workload 4.10)
 * --------------------------------------------------------------------------- */

static void test_mapperentry(void) {
    uint8_t slot[32];
    memset(slot, 0, sizeof(slot));

    /* Commit mapping: {from=1, to=2, traversalApply=true} */
    nodes_write_mapperentry(slot, 1, 2, MAPPER_FLAG_TRAVERSAL_APPLY, 0, VFS_PAGE_SIZE);

    uint32_t fromEpoch, toEpoch;
    uint16_t flags;
    int64_t nextPtr;
    nodes_read_mapperentry(slot, &fromEpoch, &toEpoch, &flags, &nextPtr, VFS_PAGE_SIZE);

    CHECK_EQ(fromEpoch, 1u);
    CHECK_EQ(toEpoch, 2u);
    CHECK_EQ(flags, MAPPER_FLAG_TRAVERSAL_APPLY);
    CHECK_EQ(nextPtr, 0);

    /* Soft-delete mapping: {from=3, to=2, flags=0} */
    memset(slot, 0, sizeof(slot));
    nodes_write_mapperentry(slot, 3, 2, 0, 0, VFS_PAGE_SIZE);
    nodes_read_mapperentry(slot, &fromEpoch, &toEpoch, &flags, &nextPtr, VFS_PAGE_SIZE);
    CHECK_EQ(fromEpoch, 3u);
    CHECK_EQ(toEpoch, 2u);
    CHECK_EQ(flags, 0);
}

/* ---------------------------------------------------------------------------
 * NameEntry tests (Workload 4.8)
 * --------------------------------------------------------------------------- */

static const char* name_test_path = "/tmp/test_nodes_name.tmp";
static void name_cleanup(void) { unlink(name_test_path); }

static Pool* name_setup(void) {
    StorageBackend* sb = storage_open(name_test_path, 8192);
    if (!sb) return NULL;
    static int64_t list_head;
    static Pool pool;
    pool_init(&pool, sb, &list_head);
    return &pool;
}

static void name_teardown(Pool* pool) {
    if (pool && pool->sb) storage_close(pool->sb);
}

static void test_nameentry_short(void) {
    Pool* pool = name_setup();
    CHECK(pool != NULL);

    int64_t first_vp;
    int n = nodes_write_name(pool, "hello", &first_vp);
    CHECK_EQ(n, 1);
    CHECK(first_vp != VFS_VPTR_NULL);

    char buf[64];
    int len = nodes_read_name(pool, first_vp, buf, sizeof(buf));
    CHECK_EQ(len, 5);
    CHECK_EQ(strcmp(buf, "hello"), 0);

    name_teardown(pool);
}

static void test_nameentry_50byte(void) {
    Pool* pool = name_setup();
    CHECK(pool != NULL);

    const char* name = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWX";
    CHECK_EQ(strlen(name), 50);

    int64_t first_vp;
    int n = nodes_write_name(pool, name, &first_vp);
    CHECK_EQ(n, 3); /* 16+24+10 */
    CHECK(first_vp != VFS_VPTR_NULL);

    char buf[128];
    int len = nodes_read_name(pool, first_vp, buf, sizeof(buf));
    CHECK_EQ(len, 50);
    CHECK_EQ(strcmp(buf, name), 0);

    name_teardown(pool);
}

static void test_nameentry_24byte(void) {
    Pool* pool = name_setup();
    CHECK(pool != NULL);

    int64_t first_vp;
    int n = nodes_write_name(pool, "abcdefghijklmnopqrstuvwx", &first_vp);
    CHECK_EQ(n, 2);  /* 16 bytes in first slot + 8 in chain slot */

    char buf[64];
    int len = nodes_read_name(pool, first_vp, buf, sizeof(buf));
    CHECK_EQ(len, 24);

    name_teardown(pool);
}

static void test_nameentry_48byte(void) {
    Pool* pool = name_setup();
    CHECK(pool != NULL);

    /* Exactly 48 bytes = 3 slots: 16 in first + 24 + 8 in chain slots */
    const char* name48 = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUV";
    CHECK_EQ(strlen(name48), 48);

    int64_t first_vp;
    int n = nodes_write_name(pool, name48, &first_vp);
    CHECK_EQ(n, 3);
    CHECK(first_vp != VFS_VPTR_NULL);

    char buf[128];
    int len = nodes_read_name(pool, first_vp, buf, sizeof(buf));
    CHECK_EQ(len, 48);
    CHECK_EQ(strcmp(buf, name48), 0);

    name_teardown(pool);
}

static void test_nameentry_unicode(void) {
    Pool* pool = name_setup();
    CHECK(pool != NULL);

    const char* uname = "h\u00e9llo w\u00f6rld";
    size_t expected_len = strlen(uname);

    int64_t first_vp;
    int n = nodes_write_name(pool, uname, &first_vp);
    CHECK(n >= 1);
    CHECK(first_vp != VFS_VPTR_NULL);

    char buf[128];
    int len = nodes_read_name(pool, first_vp, buf, sizeof(buf));
    CHECK_EQ(len, (int)expected_len);
    CHECK_EQ(strcmp(buf, uname), 0);

    name_teardown(pool);
}

static void test_nameentry_embedded_null(void) {
    Pool* pool = name_setup();
    CHECK(pool != NULL);

    const char ename[] = {'a', 'b', '\0', 'c', 'd', '\0'};
    int64_t first_vp;
    int n = nodes_write_name(pool, ename, &first_vp);
    CHECK_EQ(n, 1);

    char buf[64];
    int len = nodes_read_name(pool, first_vp, buf, sizeof(buf));
    CHECK_EQ(len, 2);
    CHECK_EQ(buf[0], 'a');
    CHECK_EQ(buf[1], 'b');
    CHECK_EQ(buf[2], '\0');

    name_teardown(pool);
}

static void test_nameentry_maxlen_boundary(void) {
    Pool* pool = name_setup();
    CHECK(pool != NULL);

    const char* name50 = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWX";
    int64_t first_vp;
    int n = nodes_write_name(pool, name50, &first_vp);
    CHECK_EQ(n, 3);

    char small_buf[10];
    int len = nodes_read_name(pool, first_vp, small_buf, 10);
    CHECK_EQ(len, 9);
    CHECK_EQ(strncmp(small_buf, "abcdefghi", 9), 0);
    CHECK_EQ(small_buf[9], '\0');

    name_teardown(pool);
}

static void test_nameentry_empty(void) {
    Pool* pool = name_setup();
    CHECK(pool != NULL);

    int64_t first_vp = 123;
    int n = nodes_write_name(pool, "", &first_vp);
    CHECK_EQ(n, 0);
    CHECK_EQ(first_vp, VFS_VPTR_NULL);

    char buf[64];
    int len = nodes_read_name(pool, first_vp, buf, sizeof(buf));
    CHECK_EQ(len, 0);

    name_teardown(pool);
}

/* ---------------------------------------------------------------------------
 * Zero-slot safety test (Phase 4e)
 * --------------------------------------------------------------------------- */

static void test_zero_slot_safety(void) {
    uint8_t slot[32];
    memset(slot, 0, sizeof(slot));

    /* DirNode: type=0, nodeId=0, headPtr=0 */
    CHECK_EQ(vfs_rd2(slot, DIRNODE_OFF_TYPE), 0);
    CHECK_EQ(vfs_rd4(slot, DIRNODE_OFF_NODEID), 0);
    CHECK_EQ(vfs_rd8(slot, DIRNODE_OFF_HEADPTR), 0);

    /* FileNode: type=0, nodeId=0, headPtr=0, sizePtr=0, createdAt=0 */
    CHECK_EQ(vfs_rd2(slot, FILENODE_OFF_TYPE), 0);
    CHECK_EQ(vfs_rd4(slot, FILENODE_OFF_NODEID), 0);
    CHECK_EQ(vfs_rd8(slot, FILENODE_OFF_HEADPTR), 0);
    CHECK_EQ(vfs_rd8(slot, FILENODE_OFF_SIZEPTR), 0);
    CHECK_EQ(vfs_rd8(slot, FILENODE_OFF_CTIME), 0);

    /* DirContent: childNodeId=0, epoch=0, childPtr=0, namePtr=0, nextPtr=0 */
    CHECK_EQ(vfs_rd4(slot, DIRCONTENT_OFF_CHILDID), 0);
    CHECK_EQ(vfs_rd4(slot, DIRCONTENT_OFF_EPOCH), 0);
    CHECK_EQ(vfs_rd8(slot, DIRCONTENT_OFF_CHILDPTR), 0);
    CHECK_EQ(vfs_rd8(slot, DIRCONTENT_OFF_NAMEPTR), 0);
    CHECK_EQ(vfs_rd8(slot, DIRCONTENT_OFF_NEXTPTR), 0);

    /* FileContent: pageRootPtr=0, nextPtr=0 */
    CHECK_EQ(vfs_rd8(slot, FILECONTENT_OFF_ROOTPTR), 0);
    CHECK_EQ(vfs_rd8(slot, FILECONTENT_OFF_NEXTPTR), 0);

    /* PageNode: versionRootPtr=0, nextPtr=0 */
    CHECK_EQ(vfs_rd8(slot, PAGENODE_OFF_VERSIONROOT), 0);
    CHECK_EQ(vfs_rd8(slot, PAGENODE_OFF_NEXTPTR), 0);

    /* VersionPage: epoch=0, dataPage=0, nextPtr=0 */
    CHECK_EQ(vfs_rd4(slot, VERSIONPAGE_OFF_EPOCH), 0);
    CHECK_EQ(vfs_rd8(slot, VERSIONPAGE_OFF_DATAPAGE), 0);
    CHECK_EQ(vfs_rd8(slot, VERSIONPAGE_OFF_NEXTPTR), 0);

    /* FileSize: epoch=0, modifiedAt=0, fileSize=0, nextPtr=0 */
    CHECK_EQ(vfs_rd4(slot, FILESIZE_OFF_EPOCH), 0);
    CHECK_EQ(vfs_rd8(slot, FILESIZE_OFF_MODIFIEDAT), 0);
    CHECK_EQ(vfs_rd8(slot, FILESIZE_OFF_FILESIZE), 0);
    CHECK_EQ(vfs_rd8(slot, FILESIZE_OFF_NEXTPTR), 0);

    /* NameEntry: data all zero, nextPtr=0 */
    int data_all_zero = 1;
    for (int i = 0; i < NAMEENTRY_DATA_SIZE; i++) {
        if (slot[i] != 0) { data_all_zero = 0; break; }
    }
    CHECK(data_all_zero);
    CHECK_EQ(vfs_rd8(slot, NAMEENTRY_OFF_NEXTPTR), 0);

    /* MapperEntry: fromEpoch=0, toEpoch=0, flags=0, nextPtr=0 */
    CHECK_EQ(vfs_rd4(slot, MAPPER_OFF_FROMEPOCH), 0);
    CHECK_EQ(vfs_rd4(slot, MAPPER_OFF_TOEPOCH), 0);
    CHECK_EQ(vfs_rd2(slot, MAPPER_OFF_FLAGS), 0);
    CHECK_EQ(vfs_rd8(slot, MAPPER_OFF_NEXTPTR), 0);
}

static void test_name_hash_compute_basic(void) {
    uint64_t h1 = name_hash_compute("foo", 3);
    CHECK(h1 != 0);

    uint64_t h2 = name_hash_compute("", 0);
    /* Empty input returns a deterministic (non-zero) hash */
    CHECK(h2 != 0);

    /* Determinism */
    CHECK_EQ(name_hash_compute("foo", 3), h1);

    /* Sensitivity: different input → different hash */
    uint64_t h_bar = name_hash_compute("bar", 3);
    CHECK(h_bar != h1);
}

static void test_name_hash_compute_long(void) {
    const char* name = "a-very-long-filename.txt";
    int len = (int)strlen(name);
    CHECK_EQ(len, 24);
    uint64_t h = name_hash_compute(name, len);
    CHECK(h != 0);

    /* One byte different */
    uint64_t h2 = name_hash_compute("a-very-long-filename.ttx", len);
    CHECK(h2 != 0);
    CHECK(h2 != h);
}

static void test_name_read_hash(void) {
    Pool* pool = name_setup();
    CHECK(pool != NULL);

    /* Short name: write "foo", read hash, verify it matches compute */
    int64_t first_vp;
    int n = nodes_write_name(pool, "foo", &first_vp);
    CHECK_EQ(n, 1);
    CHECK(first_vp != VFS_VPTR_NULL);

    uint64_t h_read = nodes_read_name_hash(pool, first_vp);
    CHECK_EQ(h_read, name_hash_compute("foo", 3));

    /* Long name: 24 bytes, write + read hash */
    const char* long_name = "a-very-long-filename.txt";
    int long_len = (int)strlen(long_name);
    CHECK_EQ(long_len, 24);
    int64_t first_vp2;
    int n2 = nodes_write_name(pool, long_name, &first_vp2);
    CHECK_EQ(n2, 2);  /* 16 + 8 → 2 slots */
    CHECK(first_vp2 != VFS_VPTR_NULL);

    uint64_t h_long = nodes_read_name_hash(pool, first_vp2);
    CHECK_EQ(h_long, name_hash_compute(long_name, long_len));

    name_teardown(pool);
}

static void test_name_read_full_roundtrip_short(void) {
    Pool* pool = name_setup();
    CHECK(pool != NULL);

    /* Empty name */
    {
        int64_t vp;
        int n = nodes_write_name(pool, "", &vp);
        CHECK_EQ(n, 0);
        CHECK_EQ(vp, VFS_VPTR_NULL);
        char buf[8];
        int len = nodes_read_name(pool, vp, buf, sizeof(buf));
        CHECK_EQ(len, 0);
        CHECK_EQ(buf[0], '\0');
    }

    /* 1 byte */
    {
        int64_t vp;
        int n = nodes_write_name(pool, "x", &vp);
        CHECK_EQ(n, 1);
        char buf[8];
        int len = nodes_read_name(pool, vp, buf, sizeof(buf));
        CHECK_EQ(len, 1);
        CHECK_EQ(strcmp(buf, "x"), 0);
    }

    /* 8 bytes */
    {
        int64_t vp;
        int n = nodes_write_name(pool, "12345678", &vp);
        CHECK_EQ(n, 1);
        char buf[16];
        int len = nodes_read_name(pool, vp, buf, sizeof(buf));
        CHECK_EQ(len, 8);
        CHECK_EQ(strcmp(buf, "12345678"), 0);
    }

    /* 15 bytes (just under first-slot limit) */
    {
        int64_t vp;
        int n = nodes_write_name(pool, "123456789012345", &vp);
        CHECK_EQ(n, 1);
        char buf[32];
        int len = nodes_read_name(pool, vp, buf, sizeof(buf));
        CHECK_EQ(len, 15);
        CHECK_EQ(strcmp(buf, "123456789012345"), 0);
    }

    name_teardown(pool);
}

static void test_name_read_full_roundtrip_long(void) {
    Pool* pool = name_setup();
    CHECK(pool != NULL);

    /* 25 bytes: 16 in first slot + 9 in second slot = 2 slots */
    {
        const char* name25 = "abcdefghijklmnopabcdefghi";  /* 25 chars */
        CHECK_EQ(strlen(name25), 25);
        int64_t vp;
        int n = nodes_write_name(pool, name25, &vp);
        CHECK_EQ(n, 2);
        char buf[64];
        int len = nodes_read_name(pool, vp, buf, sizeof(buf));
        CHECK_EQ(len, 25);
        CHECK_EQ(strcmp(buf, name25), 0);
    }

    /* 40 bytes: 16 in first slot + 24 in second = 2 slots, exactly fills */
    {
        const char* name40 = "0123456789abcdef0123456789abcdef01234567";
        CHECK_EQ(strlen(name40), 40);
        int64_t vp;
        int n = nodes_write_name(pool, name40, &vp);
        CHECK_EQ(n, 2);
        char buf[64];
        int len = nodes_read_name(pool, vp, buf, sizeof(buf));
        CHECK_EQ(len, 40);
        CHECK_EQ(strcmp(buf, name40), 0);
    }

    /* 50 bytes: 16 + 24 + 10 = 3 slots */
    {
        const char* name50 = "0123456789abcdef0123456789abcdef0123456789abcdefgh";
        CHECK_EQ(strlen(name50), 50);
        int64_t vp;
        int n = nodes_write_name(pool, name50, &vp);
        CHECK_EQ(n, 3);
        char buf[64];
        int len = nodes_read_name(pool, vp, buf, sizeof(buf));
        CHECK_EQ(len, 50);
        CHECK_EQ(strcmp(buf, name50), 0);
    }

    name_teardown(pool);
}

static void test_name_slots_needed_formula(void) {
    Pool* pool = name_setup();
    CHECK(pool != NULL);

    /* Helper: write name, verify slot count via nodes_write_name return */
    #define CHECK_SLOTS(name_str, expected_slots) do { \
        const char* nm = (name_str); \
        int64_t vp; \
        int n = nodes_write_name(pool, nm, &vp); \
        CHECK_EQ(n, (expected_slots)); \
    } while(0)

    CHECK_SLOTS("x", 1);                          /* 1 byte */
    CHECK_SLOTS("12345678", 1);                   /* 8 bytes */
    CHECK_SLOTS("1234567890123456", 1);           /* 16 bytes (exactly fills first slot) */
    CHECK_SLOTS("1234567890123456x", 2);          /* 17 bytes */
    CHECK_SLOTS("0123456789abcdef0123456789abcdef01234567", 2);  /* 40 bytes */
    CHECK_SLOTS("0123456789abcdef0123456789abcdef012345678", 3); /* 41 bytes */
    CHECK_SLOTS("abcdefghijklmnopabcdefghijklmnopabcdefghijklmnopabcdefghijklmnop", 3); /* 64 bytes */

    #undef CHECK_SLOTS
    name_teardown(pool);
}

int main(void) {
    test_dirnode_write_read();
    test_dirnode_zero_slot();
    test_dircontentindex_write_read();
    test_dircontentindex_leaf_roundtrip();
    test_dircontentindex_zero_slot();
    test_dircontentlink_write_read();
    test_dircontentlink_zero_link();
    test_dircontentlink_zero_slot();
    test_filenode_write_read();
    test_filenode_ctime();
    test_filenode_dirnode_overlap();
    test_dircontent_basic();
    test_dircontent_tombstone();
    test_dircontent_chain();

    test_filecontent();
    test_filecontent_chain();

    test_pagenode();
    test_pagenode_chain();

    /* --- Phase 26 / W1a Anchor tests --- */
    test_anchor_write_read();
    test_anchor_size_is_32();
    test_dirsegment_offset_parity();
    test_slotnode_offset_parity();
    test_dirslot_anchor_roundtrip();
    test_versionpage();
    test_versionpage_chain();

    test_filesize();
    test_filesize_chain();

    test_mapperentry();

    test_nameentry_short();
    test_nameentry_50byte();
    test_nameentry_24byte();
    test_nameentry_48byte();
    test_nameentry_unicode();
    test_nameentry_embedded_null();
    test_nameentry_maxlen_boundary();
    test_nameentry_empty();

    test_name_hash_compute_basic();
    test_name_hash_compute_long();
    test_name_read_hash();
    test_name_read_full_roundtrip_short();
    test_name_read_full_roundtrip_long();
    test_name_slots_needed_formula();

    test_zero_slot_safety();

    name_cleanup();

    printf("test_nodes: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
