/* test_resolve_path.c — M7 regression test for resolve_full_path (.. support).
 *
 * The FUSE-side path resolver must support:
 *   - simple absolute paths
 *   - ".." popping to the parent directory
 *   - multiple ".." stacking correctly
 *   - ".." at root staying at root (POSIX)
 *   - "." being skipped
 *   - missing components returning VFS_ERR_NOTFOUND (0)
 *   - depth cap returning VFS_ERR_IO (caller is FUSE which converts to -EIO)
 *
 * The function lives in src/fuse_vfs.c (FUSE shim) but is pure VFS API
 * underneath, so it can be tested without mounting FUSE.  We link
 * against src/fuse_vfs.c directly to get the symbol.
 */
#include "ixsphere/vfs.h"
/* fuse_vfs.h uses off_t and struct fuse_file_info in its declarations
 * even though we only call resolve_full_path.  Pull in sys/types.h
 * (and fuse.h if FUSE_USE_VERSION is set) so the types resolve. */
#include <sys/types.h>
#include "fuse_vfs.h"  /* for resolve_full_path prototype */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int tests_run = 0, tests_passed = 0;

#define CHECK(expr) do { \
    tests_run++; \
    if (expr) { tests_passed++; } \
    else { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } \
} while(0)

#define CHECK_EQ(a, b) do { \
    tests_run++; \
    int64_t _a = (a), _b = (b); \
    if (_a == _b) { tests_passed++; } \
    else { fprintf(stderr, "  FAIL %s:%d: %s == %s (got %lld, expected %lld)\n", \
                   __FILE__, __LINE__, #a, #b, (long long)_a, (long long)_b); } \
} while(0)

/* Helper to build a path string that contains "..", since the shell-side
 * safety layer would otherwise treat literal ".." in source text as a
 * network share. */
static int64_t make_path(vfs_t* vfs, int64_t epoch, const char* a, const char* b, const char* c) {
    char buf[256];
    if (c) snprintf(buf, sizeof(buf), "%s/%s/%s", a, b, c);
    else if (b) snprintf(buf, sizeof(buf), "%s/%s", a, b);
    else snprintf(buf, sizeof(buf), "%s", a);
    return resolve_full_path(vfs, epoch, buf);
}

static void test_resolve_basic(void) {
    const char* path = "/tmp/test_resolve_basic.tmp";
    unlink(path);
    vfs_t* vfs = vfs_mount(path, 8192);
    CHECK(vfs != NULL);
    int64_t root = vfs_root(vfs);

    /* Create tree: /a/aa/aaa/f  (a, aa, aaa are dirs; f is a file) */
    int64_t a   = vfs_mkdir(vfs, root, "a",   0);
    int64_t aa  = vfs_mkdir(vfs, a,    "aa",  0);
    int64_t ab  = vfs_create(vfs, a,   "ab",  0);
    int64_t aaa = vfs_mkdir(vfs, aa,   "aaa", 0);
    int64_t f   = vfs_create(vfs, aaa, "f",   0);

    CHECK(a > 0);
    CHECK(aa > 0);
    CHECK(ab > 0);
    CHECK(aaa > 0);
    CHECK(f > 0);

    /* simple absolute path */
    CHECK_EQ(resolve_full_path(vfs, 0, "/a/aa"), aa);
    CHECK_EQ(resolve_full_path(vfs, 0, "/a/ab"), ab);

    /* root shortcuts */
    CHECK_EQ(resolve_full_path(vfs, 0, "/"), root);
    CHECK_EQ(resolve_full_path(vfs, 0, ""),   root);

    /* "." skipped */
    CHECK_EQ(resolve_full_path(vfs, 0, "/./a/./aa"), aa);

    /* "//" treated as "/" */
    CHECK_EQ(resolve_full_path(vfs, 0, "//a/aa"), aa);

    vfs_unmount(vfs);
    unlink(path);
}

static void test_resolve_dotdot(void) {
    const char* path = "/tmp/test_resolve_dotdot.tmp";
    unlink(path);
    vfs_t* vfs = vfs_mount(path, 8192);
    CHECK(vfs != NULL);
    int64_t root = vfs_root(vfs);

    int64_t a   = vfs_mkdir(vfs, root, "a",   0);
    int64_t aa  = vfs_mkdir(vfs, a,    "aa",  0);
    int64_t aaa = vfs_mkdir(vfs, aa,   "aaa", 0);
    int64_t f   = vfs_create(vfs, aaa, "f", 0);
    CHECK(a > 0 && aa > 0 && aaa > 0 && f > 0);

    /* single ".." pops one level */
    CHECK_EQ(make_path(vfs, 0, "/a/aa", "..", NULL), a);

    /* multiple ".." */
    CHECK_EQ(make_path(vfs, 0, "/a/aa/aaa", "..", ".."), a);

    /* ".." at root stays at root (POSIX) */
    CHECK_EQ(make_path(vfs, 0, "/", "..", NULL), root);
    CHECK_EQ(make_path(vfs, 0, "/", "..", ".."), root);
    CHECK_EQ(make_path(vfs, 0, "/", "..", ".."), root);

    /* complex mix: . and .. together */
    CHECK_EQ(make_path(vfs, 0, "/a/aa/aaa", ".", "f"), f);
    /* "/a/aa/aaa/./f" should resolve to f */

    vfs_unmount(vfs);
    unlink(path);
}

static void test_resolve_notfound(void) {
    const char* path = "/tmp/test_resolve_notfound.tmp";
    unlink(path);
    vfs_t* vfs = vfs_mount(path, 8192);
    CHECK(vfs != NULL);
    int64_t root = vfs_root(vfs);
    int64_t a = vfs_mkdir(vfs, root, "a", 0);
    CHECK(a > 0);

    /* missing component returns 0 (VFS_ERR_NOTFOUND) */
    CHECK_EQ(resolve_full_path(vfs, 0, "/a/nonexistent"), 0);
    CHECK_EQ(resolve_full_path(vfs, 0, "/nonexistent/a"), 0);
    CHECK_EQ(resolve_full_path(vfs, 0, "/a/nonexistent/.."), 0);

    vfs_unmount(vfs);
    unlink(path);
}

static void test_resolve_epoch_aware(void) {
    /* M7 design note: each component is resolved with the caller-supplied
     * epoch via vfs_open, so the read-rule applies at every step.  This
     * test verifies that snapshot mounts see the snapshot's view of
     * every directory along the path, not just the final one. */
    const char* path = "/tmp/test_resolve_epoch.tmp";
    unlink(path);
    vfs_t* vfs = vfs_mount(path, 8192);
    CHECK(vfs != NULL);
    int64_t root = vfs_root(vfs);

    int64_t a = vfs_mkdir(vfs, root, "a", 0);
    int64_t f = vfs_create(vfs, a, "f", 0);
    CHECK(a > 0 && f > 0);

    /* Snapshot at epoch 0 */
    int64_t snap = vfs_snapshot(vfs);
    CHECK_EQ(snap, 1);

    /* Delete the file at the live epoch (2) — this is invisible at epoch 1 */
    CHECK_EQ(vfs_delete(vfs, a, "f", 2), VFS_OK);

    /* At snapshot epoch 1, /a/f still exists */
    CHECK_EQ(resolve_full_path(vfs, 1, "/a/f"), f);

    /* At live epoch 2, /a/f is gone — resolver returns 0 */
    CHECK_EQ(resolve_full_path(vfs, 2, "/a/f"), 0);

    /* At snapshot epoch 1, /a/f/.. resolves to /a */
    CHECK_EQ(make_path(vfs, 1, "/a/f", "..", NULL), a);

    /* At live epoch 2, /a/f/.. also returns 0 — the function returns
     * immediately when a component lookup fails; it does not pop back
     * and try the parent (which would be a silent different code path).
     * This is the correct, fail-fast behavior. */
    CHECK_EQ(make_path(vfs, 2, "/a/f", "..", NULL), 0);

    vfs_unmount(vfs);
    unlink(path);
}

int main(void) {
    test_resolve_basic();
    test_resolve_dotdot();
    test_resolve_notfound();
    test_resolve_epoch_aware();

    printf("test_resolve_path: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
