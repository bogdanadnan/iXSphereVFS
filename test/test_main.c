#include "ixsphere/vfs.h"
#include "platform.h"
#include "page_buf.h"
#include <stdio.h>
#include <string.h>

#if VFS_OS_WINDOWS
    #include <windows.h>
    #include <process.h>
#else
    #include <pthread.h>
#endif

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

/* ========================================================================== */

void test_platform_detection(void) {
    printf("2. Platform detection...\n");
    CHECK_EQ(VFS_PAGE_SIZE, 8192);
    CHECK_TRUE(VFS_CACHELINE >= 64);
    CHECK_TRUE((VFS_CACHELINE & (VFS_CACHELINE - 1)) == 0);
    int os_count = (VFS_OS_LINUX ? 1 : 0) + (VFS_OS_MACOS ? 1 : 0) + (VFS_OS_WINDOWS ? 1 : 0);
    CHECK_EQ(os_count, 1);
    int arch_count = (VFS_ARCH_X86_64 ? 1 : 0) + (VFS_ARCH_AARCH64 ? 1 : 0);
    CHECK_EQ(arch_count, 1);
}

/* ========================================================================== */

void test_crc32c(void) {
    printf("3. CRC32C...\n");

    CHECK_EQ(vfs_crc32c(NULL, 0), 0x00000000u);
    CHECK_EQ(vfs_crc32c((const uint8_t*)"123456789", 9), 0xE3069283u);
    CHECK_EQ(vfs_crc32c((const uint8_t*)"hello world", 11),
             vfs_crc32c((const uint8_t*)"hello world", 11));

    uint8_t zero_page[8192];
    memset(zero_page, 0, 8192);
    uint32_t crc1 = vfs_crc32c(zero_page, 8192);
    CHECK_EQ(crc1, vfs_crc32c(zero_page, 8192));

    uint8_t ones_page[8192];
    memset(ones_page, 1, 8192);
    CHECK(vfs_crc32c(ones_page, 8192) != crc1);

    uint8_t large_buf[65536];
    for (int i = 0; i < 65536; i++) large_buf[i] = (uint8_t)i;
    CHECK_EQ(vfs_crc32c(large_buf, 65536), vfs_crc32c(large_buf, 65536));

    /* Unaligned input works */
    uint8_t aligned_buf[16];
    memset(aligned_buf, 0, 16);
    aligned_buf[8] = 0xFF;
    CHECK(vfs_crc32c(aligned_buf + 1, 8) != 0);
}

/* ========================================================================== */

static int64_t s_mt_counter = 0;

#if VFS_OS_WINDOWS
static unsigned __stdcall mt_thread_func(void* arg) {
    (void)arg;
    for (int i = 0; i < 100000; i++) vfs_atomic_add_i64(&s_mt_counter, 1);
    return 0;
}
#else
static void* mt_thread_func(void* arg) {
    (void)arg;
    for (int i = 0; i < 100000; i++) vfs_atomic_add_i64(&s_mt_counter, 1);
    return NULL;
}
#endif

void test_atomics(void) {
    printf("4. Atomics...\n");

    int64_t x = 0;

    vfs_atomic_store_i64(&x, 42);
    CHECK_EQ(vfs_atomic_load_i64(&x), 42);

    int64_t old = vfs_cas_i64(&x, 42, 99);
    CHECK_EQ(old, 42);
    CHECK_EQ(vfs_atomic_load_i64(&x), 99);

    old = vfs_cas_i64(&x, 77, 100);
    CHECK_EQ(old, 99);
    CHECK_EQ(vfs_atomic_load_i64(&x), 99);

    int64_t cur = vfs_atomic_load_i64(&x);
    while (vfs_cas_i64(&x, cur, cur + 1) != cur) {
        cur = vfs_atomic_load_i64(&x);
    }
    CHECK_EQ(vfs_atomic_load_i64(&x), 100);

    int64_t new_val = vfs_atomic_add_i64(&x, 1);
    CHECK_EQ(new_val, 101);
    CHECK_EQ(vfs_atomic_load_i64(&x), 101);

    /* 4-thread counter test */
    s_mt_counter = 0;
#if VFS_OS_WINDOWS
    HANDLE th[4];
    for (int i = 0; i < 4; i++)
        th[i] = (HANDLE)_beginthreadex(NULL, 0, mt_thread_func, NULL, 0, NULL);
    for (int i = 0; i < 4; i++) WaitForSingleObject(th[i], INFINITE);
    for (int i = 0; i < 4; i++) CloseHandle(th[i]);
#else
    pthread_t th[4];
    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, mt_thread_func, NULL);
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
#endif
    CHECK_EQ(s_mt_counter, 400000);
}

/* ========================================================================== */

void test_page_buf(void) {
    printf("5. Page buffer helpers...\n");

    uint8_t page[8192];
    memset(page, 0xFF, 8192);

    vfs_wr8(page, 0, 0x1122334455667788LL);
    CHECK_EQ(vfs_rd8(page, 0), 0x1122334455667788LL);

    vfs_wr8(page, 8184, 0xDEADBEEFLL);
    CHECK_EQ(vfs_rd8(page, 8184), 0xDEADBEEFLL);

    vfs_wr4(page, 100, 0x12345678);
    CHECK_EQ(vfs_rd4(page, 100), 0x12345678);

    vfs_wr2(page, 200, (int16_t)12345);
    CHECK_EQ(vfs_rd2(page, 200), (int16_t)12345);

    vfs_zero_page(page, VFS_PAGE_SIZE);
    int all_zero = 1;
    for (int i = 0; i < 8192; i++) { if (page[i] != 0) { all_zero = 0; break; } }
    CHECK(all_zero);

    memset(page, 0xAA, 8192);
    uint8_t dst[8192];
    vfs_copy_page(dst, page, VFS_PAGE_SIZE);
    CHECK_EQ(memcmp(dst, page, 8192), 0);

    /* vfs_zero_page_fast produces identical result to memset */
    memset(page, 0xFF, 8192);
    vfs_zero_page_fast(page, VFS_PAGE_SIZE);
    all_zero = 1;
    for (int i = 0; i < 8192; i++) { if (page[i] != 0) { all_zero = 0; break; } }
    CHECK(all_zero);
}

/* ========================================================================== */

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
