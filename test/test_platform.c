/*
 * test/test_platform.c — Platform Detection Tests
 */
#include "vfs_internal.h"
#include <stdio.h>

/* External counters from test_runner.c */
extern int tests_run;
extern int tests_passed;

#define CHECK(expr) do { \
    tests_run++; \
    if (expr) { tests_passed++; } \
    else { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } \
} while(0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

int test_platform_detection(void) {
    /* VFS_PAGE_SIZE must be 8192 */
    CHECK_EQ(VFS_PAGE_SIZE, 8192);
    
    /* VFS_CACHELINE must be a power of two >= 64 */
    int cacheline_ok = (VFS_CACHELINE >= 64) && 
                       ((VFS_CACHELINE & (VFS_CACHELINE - 1)) == 0);
    CHECK(cacheline_ok);
    
    /* Platform macros should be defined */
#ifdef VFS_ARCH_X86_64
    CHECK_EQ(VFS_ARCH_X86_64, 1);
#endif
#ifdef VFS_ARCH_AARCH64
    CHECK_EQ(VFS_ARCH_AARCH64, 1);
#endif
    
    return 0;
}
