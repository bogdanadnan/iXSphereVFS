#include "ixsphere_vfs.h"
#include "platform.h"
#include "page_buf.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

static int tests_run    = 0;
static int tests_passed = 0;

#define CHECK(expr) do { \
    tests_run++; \
    if (expr) { tests_passed++; } \
    else { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } \
} while(0)

#define CHECK_EQ(a, b)  CHECK((a) == (b))
#define CHECK_STREQ(a, b) CHECK(strcmp((a), (b)) == 0)
#define CHECK_TRUE(a)   CHECK((a))
#define CHECK_FALSE(a)  CHECK(!(a))

void test_platform_detection(void) {
    printf("2. Platform detection...\n");
    
    /* VFS_PAGE_SIZE must be 8192 at compile time */
    CHECK_EQ(VFS_PAGE_SIZE, 8192);
    
    /* VFS_CACHELINE must be a power of 2 and >= 64 */
    CHECK_TRUE(VFS_CACHELINE >= 64);
    CHECK_TRUE((VFS_CACHELINE & (VFS_CACHELINE - 1)) == 0);
    
    /* At least one OS must be defined */
    int os_count = (VFS_OS_LINUX ? 1 : 0) + (VFS_OS_MACOS ? 1 : 0) + (VFS_OS_WINDOWS ? 1 : 0);
    CHECK_EQ(os_count, 1);
    
    /* At least one arch must be defined */
    int arch_count = (VFS_ARCH_X86_64 ? 1 : 0) + (VFS_ARCH_AARCH64 ? 1 : 0);
    CHECK_EQ(arch_count, 1);
}

void test_crc32c(void) {
    printf("3. CRC32C...\n");
    
    /* Empty input: NULL/0 returns 0 */
    CHECK_EQ(vfs_crc32c(NULL, 0), 0x00000000);
    
    /* Known vector: "123456789" -> 0xE3069283 */
    CHECK_EQ(vfs_crc32c((const uint8_t*)"123456789", 9), 0xE3069283);
    
    /* Deterministic: same input twice gives same result */
    CHECK_EQ(vfs_crc32c((const uint8_t*)"hello world", 11), 
             vfs_crc32c((const uint8_t*)"hello world", 11));
    
    /* Large buffer is deterministic (all zeros) */
    uint8_t zero_page[8192];
    memset(zero_page, 0, 8192);
    uint32_t crc1 = vfs_crc32c(zero_page, 8192);
    CHECK_EQ(crc1, vfs_crc32c(zero_page, 8192));
    
    /* Different inputs give different outputs */
    uint8_t ones_page[8192];
    memset(ones_page, 1, 8192);
    CHECK(vfs_crc32c(ones_page, 8192) != crc1);
    
    /* Large buffer is deterministic (ascending bytes) */
    uint8_t large_buf[65536];
    for (int i = 0; i < 65536; i++) {
        large_buf[i] = (uint8_t)i;
    }
    CHECK_EQ(vfs_crc32c(large_buf, 65536), vfs_crc32c(large_buf, 65536));
}

static int64_t s_mt_counter = 0;

static void* atomic_thread_func(void* arg) {
    (void)arg;
    for (int i = 0; i < 10000; i++) {
        vfs_atomic_add_i64(&s_mt_counter, 1);
    }
    return NULL;
}

void test_atomics(void) {
    printf("4. Atomics...\n");
    
    int64_t x = 0;
    
    /* Basic store/load */
    vfs_atomic_store_i64(&x, 42);
    CHECK_EQ(vfs_atomic_load_i64(&x), 42);
    
    /* CAS with matching expected: succeeds, returns old value */
    int64_t old = vfs_cas_i64(&x, 42, 99);
    CHECK_EQ(old, 42);
    CHECK_EQ(vfs_atomic_load_i64(&x), 99);
    
    /* CAS with non-matching expected: fails, returns current value */
    old = vfs_cas_i64(&x, 77, 100);
    CHECK_EQ(old, 99);
    CHECK_EQ(vfs_atomic_load_i64(&x), 99);  /* unchanged */
    
    /* CAS retry loop increments */
    int64_t cur = vfs_atomic_load_i64(&x);
    while (vfs_cas_i64(&x, cur, cur + 1) != cur) {
        cur = vfs_atomic_load_i64(&x);
    }
    CHECK_EQ(vfs_atomic_load_i64(&x), 100);
    
    /* Add operation */
    int64_t new_val = vfs_atomic_add_i64(&x, 1);
    CHECK_EQ(new_val, 101);
    CHECK_EQ(vfs_atomic_load_i64(&x), 101);
    
    /* Multi-thread test */
    s_mt_counter = 0;
    pthread_t th[4];
    for (int i = 0; i < 4; i++) {
        pthread_create(&th[i], NULL, atomic_thread_func, NULL);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(th[i], NULL);
    }
    CHECK_EQ(s_mt_counter, 40000);
}

void test_page_buf(void) {
    printf("5. Page buffer helpers...\n");
    
    uint8_t page[8192];
    memset(page, 0xFF, 8192);
    
    /* Write int64 at offset 0, read back */
    vfs_wr8(page, 0, 0x1122334455667788LL);
    CHECK_EQ(vfs_rd8(page, 0), 0x1122334455667788LL);
    
    /* Write at offset 8184 (last 8 bytes of page) */
    vfs_wr8(page, 8184, 0xDEADBEEF);
    CHECK_EQ(vfs_rd8(page, 8184), 0xDEADBEEF);
    
    /* Write int32 at offset 100 */
    vfs_wr4(page, 100, 0x12345678);
    CHECK_EQ(vfs_rd4(page, 100), 0x12345678);
    
    /* Write int16 at offset 200 */
    vfs_wr2(page, 200, (int16_t)12345);
    CHECK_EQ(vfs_rd2(page, 200), (int16_t)12345);
    
    /* vfs_zero_page */
    vfs_zero_page(page);
    for (int i = 0; i < 8192; i++) {
        if (page[i] != 0) {
            CHECK(0 && "page not zero");
            break;
        }
    }
    
    /* vfs_copy_page */
    memset(page, 0xAA, 8192);
    uint8_t dst[8192];
    vfs_copy_page(dst, page);
    CHECK_EQ(memcmp(dst, page, 8192), 0);
}

int main(void) {
    printf("=== iXSphereVFS Tests ===\n\n");

    printf("1. Error strings...\n");
    CHECK_STREQ(vfs_error_string(VFS_OK),           "OK");
    CHECK_STREQ(vfs_error_string(VFS_ERR_NOTFOUND), "Not found");
    CHECK_STREQ(vfs_error_string(VFS_ERR_IO),       "I/O error");
    CHECK_STREQ(vfs_error_string(-999),             "Unknown error");

    test_platform_detection();
    test_crc32c();
    test_atomics();
    test_page_buf();
    
    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
