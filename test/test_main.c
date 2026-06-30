#include "ixsphere_vfs.h"
#include <stdio.h>
#include <string.h>

static int tests_run    = 0;
static int tests_passed = 0;

#define CHECK(expr) do { \
    tests_run++; \
    if (expr) { tests_passed++; } \
    else { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } \
} while(0)

#define CHECK_EQ(a, b)  CHECK((a) == (b))
#define CHECK_STREQ(a, b) CHECK(strcmp((a), (b)) == 0)

int main(void) {
    printf("=== iXSphereVFS Tests ===\n\n");

    printf("1. Error strings...\n");
    CHECK_STREQ(vfs_error_string(VFS_OK),           "OK");
    CHECK_STREQ(vfs_error_string(VFS_ERR_NOTFOUND), "Not found");
    CHECK_STREQ(vfs_error_string(VFS_ERR_IO),       "I/O error");
    CHECK_STREQ(vfs_error_string(-999),             "Unknown error");

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
