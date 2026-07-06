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

    /* Write DirNode with nodeId=5, headPtr points to page=10, slot=3 */
    nodes_write_dirnode(slot, 5, VFS_VPTR_MAKE(10, 3), VFS_PAGE_SIZE);

    /* Verify type field at byte 0 */
    CHECK_EQ(vfs_rd2(slot, DIRNODE_OFF_TYPE), (int16_t)NODE_TYPE_DIR);

    /* Read back all fields */
    uint32_t nodeId;
    int64_t headPtr;
    nodes_read_dirnode(slot, &nodeId, &headPtr, VFS_PAGE_SIZE);

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

    /* Verify reserved bytes 16-31 are zero */
    int all_zero = 1;
    for (int i = 16; i < 32; i++) {
        if (slot[i] != 0) { all_zero = 0; break; }
    }
    CHECK(all_zero);
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

    /* Verify reserved bytes 20-31 are zero */
    int all_zero = 1;
    for (int i = 20; i < 32; i++) {
        if (slot[i] != 0) { all_zero = 0; break; }
    }
    CHECK(all_zero);
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
 * TouchedFile tests (Workload 4.9)
 * --------------------------------------------------------------------------- */

static void test_touchedfile(void) {
    uint8_t slot[32];
    memset(slot, 0, sizeof(slot));

    /* Single TouchedFile {epoch=3, nodeId=7} */
    nodes_write_touchedfile(slot, 3, 7, 0, VFS_PAGE_SIZE);

    uint32_t epoch, nodeId;
    int64_t nextPtr;
    nodes_read_touchedfile(slot, &epoch, &nodeId, &nextPtr, VFS_PAGE_SIZE);

    CHECK_EQ(epoch, 3u);
    CHECK_EQ(nodeId, 7u);
    CHECK_EQ(nextPtr, 0);

    /* Verify reserved bytes 16-31 are zero */
    int all_zero = 1;
    for (int i = 16; i < 32; i++) {
        if (slot[i] != 0) { all_zero = 0; break; }
    }
    CHECK(all_zero);
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

    /* TouchedFile: epoch=0, nodeId=0, nextPtr=0 */
    CHECK_EQ(vfs_rd4(slot, TOUCHEDFILE_OFF_EPOCH), 0);
    CHECK_EQ(vfs_rd4(slot, TOUCHEDFILE_OFF_NODEID), 0);
    CHECK_EQ(vfs_rd8(slot, TOUCHEDFILE_OFF_NEXTPTR), 0);

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

int main(void) {
    test_dirnode_write_read();
    test_dirnode_zero_slot();
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
    test_versionpage();
    test_versionpage_chain();

    test_filesize();
    test_filesize_chain();

    test_touchedfile();
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

    test_zero_slot_safety();

    name_cleanup();

    printf("test_nodes: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
