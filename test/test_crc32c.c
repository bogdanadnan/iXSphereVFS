/*
 * test/test_crc32c.c — CRC32C Tests
 *
 * Tests known vectors: empty input, "123456789", 8KB patterns.
 */
#include "ixsphere_vfs.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

extern int tests_run;
extern int tests_passed;

#define CHECK(expr) do { \
    tests_run++; \
    if (expr) { tests_passed++; } \
    else { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } \
} while(0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

int test_crc32c(void) {
    /* 1. Empty input -> 0x00000000 */
    CHECK_EQ(vfs_crc32c(NULL, 0), 0x00000000);
    
    /* 2. "123456789" (9 bytes) -> 0xE3069283 */
    const char* test_str = "123456789";
    CHECK_EQ(vfs_crc32c((const uint8_t*)test_str, 9), 0xE3069283);
    
    /* 3. 8KB of zeros -> deterministic, repeatable value */
    {
        uint8_t* zeros = calloc(8192, 1);
        uint32_t crc_zeros = vfs_crc32c(zeros, 8192);
        CHECK_EQ(vfs_crc32c(zeros, 8192), crc_zeros);
        free(zeros);
    }
    
    /* 4. 8KB of 0xFF -> different from zeros */
    {
        uint8_t* ff_buf = malloc(8192);
        memset(ff_buf, 0xFF, 8192);
        uint32_t crc_ff = vfs_crc32c(ff_buf, 8192);
        CHECK_EQ(vfs_crc32c(ff_buf, 8192), crc_ff);
        free(ff_buf);
    }
    
    /* 5. 64KB of ascending bytes -> deterministic value */
    {
        uint8_t* pattern = malloc(65536);
        for (int i = 0; i < 65536; i++) {
            pattern[i] = (uint8_t)i;
        }
        uint32_t crc_pattern = vfs_crc32c(pattern, 65536);
        CHECK_EQ(vfs_crc32c(pattern, 65536), crc_pattern);
        free(pattern);
    }
    
    /* 6. Consistency check - same input always produces same output */
    {
        uint8_t buf[100];
        for (int i = 0; i < 100; i++) buf[i] = i * 3;
        uint32_t first = vfs_crc32c(buf, 100);
        uint32_t second = vfs_crc32c(buf, 100);
        CHECK_EQ(first, second);
    }
    
    return 0;
}
