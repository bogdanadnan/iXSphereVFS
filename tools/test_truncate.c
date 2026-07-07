/* test_truncate.c — unit test for vfs_truncate API + vfs_read EOF behaviour.
 *
 * Scenarios:
 *   1. Write data, truncate to smaller, read returns short read at EOF.
 *   2. Write data, truncate to larger, new region is zero-filled.
 *   3. Read past EOF returns 0 bytes.
 *   4. Truncate to 0, file becomes empty.
 */

#include "ixsphere/vfs.h"
#include <stdio.h>
#include <string.h>

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
        return 1; \
    } \
} while (0)

#define ASSERT_EQ(actual, expected, msg) do { \
    long long _a = (long long)(actual); \
    long long _e = (long long)(expected); \
    if (_a != _e) { \
        fprintf(stderr, "FAIL: %s: got %lld expected %lld (line %d)\n", \
                msg, _a, _e, __LINE__); \
        return 1; \
    } \
} while (0)

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <file.vfs>\n", argv[0]);
        return 1;
    }
    const char* path = argv[1];

    /* Create a fresh VFS with a file containing 100 bytes */
    vfs_t* vfs = vfs_mount(path, 8192);
    ASSERT(vfs != NULL, "mount");
    int64_t root = vfs_root(vfs);
    int64_t f = vfs_create(vfs, root, "data.txt", 0);
    ASSERT(f > 0, "create");
    ASSERT_EQ(vfs_file_size(vfs, f, 0), 0, "initial size");

    char data[100];
    for (int i = 0; i < 100; i++) data[i] = (char)('A' + (i % 26));
    ASSERT_EQ(vfs_write(vfs, f, data, 0, 100, 0), 100, "write 100");
    ASSERT_EQ(vfs_file_size(vfs, f, 0), 100, "size after write");

    vfs_flush(vfs);
    vfs_unmount(vfs);

    /* === Scenario 1: truncate to 50, then read past EOF === */
    fprintf(stderr, "=== Scenario 1: shrink + read past EOF ===\n");
    vfs = vfs_mount(path, 8192);
    ASSERT(vfs != NULL, "mount");
    int64_t root2 = vfs_root(vfs);
    vfs_dirent_t ents[16];
    int n = vfs_readdir(vfs, root2, ents, 16, 0);
    ASSERT(n == 1, "readdir count");
    int64_t data_vp = ents[0].vp;

    /* Truncate to 50 */
    int r = vfs_truncate(vfs, data_vp, 50, 0);
    ASSERT_EQ(r, VFS_OK, "truncate to 50");
    ASSERT_EQ(vfs_file_size(vfs, data_vp, 0), 50, "size after shrink");

    /* Read 100 bytes from offset 0 — should return only 50 */
    char buf[100];
    int rd = vfs_read(vfs, data_vp, buf, 0, 100, 0);
    ASSERT_EQ(rd, 50, "read after shrink");

    /* Read past EOF */
    rd = vfs_read(vfs, data_vp, buf, 60, 100, 0);
    ASSERT_EQ(rd, 0, "read entirely past EOF");

    /* Read straddling EOF: offset 40, count 20 — should return 10 */
    rd = vfs_read(vfs, data_vp, buf, 40, 20, 0);
    ASSERT_EQ(rd, 10, "read straddling EOF");

    /* Verify first 50 bytes match original */
    int reread = vfs_read(vfs, data_vp, buf, 0, 50, 0);
    ASSERT_EQ(reread, 50, "read after shrink #2");
    ASSERT(memcmp(buf, data, 50) == 0, "content preserved after shrink");

    vfs_flush(vfs);
    vfs_unmount(vfs);

    /* === Scenario 2: truncate to larger, new region is zero-filled === */
    fprintf(stderr, "=== Scenario 2: grow + zero-fill ===\n");
    vfs = vfs_mount(path, 8192);
    ASSERT(vfs != NULL, "mount");
    root2 = vfs_root(vfs);
    n = vfs_readdir(vfs, root2, ents, 16, 0);
    ASSERT(n == 1, "readdir count");
    data_vp = ents[0].vp;

    /* Truncate from 50 to 200 */
    r = vfs_truncate(vfs, data_vp, 200, 0);
    ASSERT_EQ(r, VFS_OK, "truncate to 200");
    ASSERT_EQ(vfs_file_size(vfs, data_vp, 0), 200, "size after grow");

    /* Read 200 bytes — should be: first 50 from original, next 150 zeros */
    memset(buf, 0xFF, sizeof(buf));
    rd = vfs_read(vfs, data_vp, buf, 0, 200, 0);
    ASSERT_EQ(rd, 200, "read full after grow");
    ASSERT(memcmp(buf, data, 50) == 0, "first 50 preserved after grow");
    int all_zero = 1;
    for (int i = 50; i < 200; i++) {
        if (buf[i] != 0) { all_zero = 0; break; }
    }
    ASSERT(all_zero, "new region zero-filled");

    vfs_flush(vfs);
    vfs_unmount(vfs);

    /* === Scenario 3: truncate to 0 === */
    fprintf(stderr, "=== Scenario 3: truncate to 0 ===\n");
    vfs = vfs_mount(path, 8192);
    ASSERT(vfs != NULL, "mount");
    root2 = vfs_root(vfs);
    n = vfs_readdir(vfs, root2, ents, 16, 0);
    ASSERT(n == 1, "readdir count");
    data_vp = ents[0].vp;

    r = vfs_truncate(vfs, data_vp, 0, 0);
    ASSERT_EQ(r, VFS_OK, "truncate to 0");
    ASSERT_EQ(vfs_file_size(vfs, data_vp, 0), 0, "size after truncate to 0");

    rd = vfs_read(vfs, data_vp, buf, 0, 100, 0);
    ASSERT_EQ(rd, 0, "read after truncate to 0");

    vfs_flush(vfs);
    vfs_unmount(vfs);

    fprintf(stderr, "\n=== ALL TRUNCATE TESTS PASSED ===\n");
    return 0;
}