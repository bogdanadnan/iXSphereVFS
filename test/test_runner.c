/*
 * test/test_runner.c — iXSphereVFS Test Harness
 *
 * A main() that calls individual test functions and reports
 * total passed vs failed. The CHECK macro increments counters
 * and prints failures with file and line.
 */
#include "ixsphere_vfs.h"
#include <stdio.h>
#include <string.h>

/* External test functions from separate test files */
extern int test_platform_detection(void);
extern int test_crc32c(void);
extern int test_atomics(void);
extern int test_page_buf(void);
extern int test_storage_backend(void);
extern int test_bitmap_allocator(void);
extern int test_bitmap_concurrent(void);

/* Global counters - shared across all test files */
int tests_run    = 0;
int tests_passed = 0;

#define CHECK(expr) do { \
    tests_run++; \
    if (expr) { tests_passed++; } \
    else { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } \
} while(0)

#define CHECK_EQ(a, b) CHECK((a) == (b))
#define CHECK_STREQ(a, b) CHECK(strcmp((a), (b)) == 0)

int main(void) {
    printf("=== iXSphereVFS Tests ===\n\n");

    /* 1. Platform detection */
    printf("1. Platform detection...\n");
    test_platform_detection();

    /* 2. CRC32C */
    printf("2. CRC32C...\n");
    test_crc32c();

    /* 3. Atomics */
    printf("3. Atomics...\n");
    test_atomics();

    /* 4. Page buffer helpers */
    printf("4. Page buffer...\n");
    test_page_buf();

    /* 5. StorageBackend */
    printf("5. StorageBackend...\n");
    test_storage_backend();

    /* 6. Bitmap allocator */
    printf("6. Bitmap allocator...\n");
    test_bitmap_allocator();
    
    /* 7. Bitmap concurrent allocation */
    printf("7. Bitmap concurrent...\n");
    test_bitmap_concurrent();

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
