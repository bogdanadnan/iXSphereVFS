/*
 * test/test_page_buf.c — Page Buffer Tests
 *
 * Tests read/write at arbitrary offsets, zero-fill, and copy.
 */
#include "src/page_buf.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern int tests_run;
extern int tests_passed;

#define CHECK(expr) do { \
    tests_run++; \
    if (expr) { tests_passed++; } \
    else { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } \
} while(0)

#define CHECK_EQ_I64(a, b) CHECK((a) == (int64_t)(b))
#define CHECK_EQ_I32(a, b) CHECK((a) == (int32_t)(b))
#define CHECK_EQ_I16(a, b) CHECK((a) == (int16_t)(b))

int test_page_buf(void) {
    uint8_t* page = malloc(8192);
    memset(page, 0, 8192);
    
    /* 1. Write/read int64 at offset 0 */
    vfs_wr8(page, 0, (int64_t)0x123456789ABCDEF0LL);
    CHECK_EQ_I64(vfs_rd8(page, 0), 0x123456789ABCDEF0LL);
    
    /* 2. Write/read int64 at offset 8184 (last 8 bytes of page) */
    vfs_wr8(page, 8184, (int64_t)0xFEDCBA9876543210LL);
    CHECK_EQ_I64(vfs_rd8(page, 8184), 0xFEDCBA9876543210LL);
    
    /* 3. Write/read int32 at various offsets */
    vfs_wr4(page, 100, (int32_t)0xDEADBEEF);
    CHECK_EQ_I32(vfs_rd4(page, 100), 0xDEADBEEF);
    
    vfs_wr4(page, 8188, (int32_t)0xCAFEBABE);
    CHECK_EQ_I32(vfs_rd4(page, 8188), 0xCAFEBABE);
    
    /* 4. Write/read int16 at various offsets */
    vfs_wr2(page, 500, (int16_t)0x1234);
    CHECK_EQ_I16(vfs_rd2(page, 500), 0x1234);
    
    vfs_wr2(page, 8190, (int16_t)0x5678);
    CHECK_EQ_I16(vfs_rd2(page, 8190), 0x5678);
    
    /* 5. vfs_zero_page fills with zeros */
    memset(page, 0xFF, 8192);
    vfs_zero_page(page);
    int all_zero = 1;
    for (int i = 0; i < 8192; i++) {
        if (page[i] != 0) {
            all_zero = 0;
            break;
        }
    }
    CHECK(all_zero);
    
    /* 6. vfs_copy_page produces identical copy */
    memset(page, 0, 8192);
    for (int i = 0; i < 8192; i++) {
        page[i] = (uint8_t)(i & 0xFF);
    }
    uint8_t* copy = malloc(8192);
    vfs_copy_page(copy, page);
    int identical = (memcmp(page, copy, 8192) == 0);
    CHECK(identical);
    
    /* 7. vfs_zero_page_fast matches memset on all 0xFF buffer */
    memset(page, 0xFF, 8192);
    vfs_zero_page_fast(page);
    all_zero = 1;
    for (int i = 0; i < 8192; i++) {
        if (page[i] != 0) {
            all_zero = 0;
            break;
        }
    }
    CHECK(all_zero);
    
    free(copy);
    free(page);
    
    return 0;
}
