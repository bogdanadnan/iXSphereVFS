#include "storage.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

static int tests_run    = 0;
static int tests_passed = 0;

#define CHECK(expr) do { \
    tests_run++; \
    if (expr) { tests_passed++; } \
    else { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } \
} while(0)

#define CHECK_EQ(a, b)  CHECK((a) == (b))

/* Cleanup helper */
static void cleanup(const char* path) { unlink(path); }

/* ========================================================================== */

void test_create_open(void) {
    printf("1. File layout & open/create...\n");
    const char* path = "/tmp/test_storage_vfs.dat";
    cleanup(path);

    /* Create new file */
    StorageBackend* sb = storage_open(path, 8192);
    CHECK(sb != NULL);
    if (!sb) return;

    CHECK_EQ(sb->total_pages, 2);
    CHECK_EQ(sb->page_size, 8192);
    CHECK_EQ(sb->segment_size, 1024);
    storage_close(sb);

    /* Reopen existing file */
    sb = storage_open(path, 0);
    CHECK(sb != NULL);
    if (sb) {
        CHECK_EQ(sb->total_pages, 2);
        CHECK_EQ(sb->page_size, 8192);
        CHECK_EQ(sb->segment_size, 1024);
        storage_close(sb);
    }

    /* Open non-VFS file */
    const char* bad_path = "/tmp/test_bad_vfs.dat";
    FILE* f = fopen(bad_path, "wb");
    if (f) { fwrite("garbage", 1, 7, f); fclose(f); }
    sb = storage_open(bad_path, 8192);
    CHECK(sb == NULL);
    cleanup(bad_path);

    cleanup(path);
}

void test_allocate(void) {
    printf("2. Allocate/Acquire/Free...\n");
    const char* path = "/tmp/test_alloc_vfs.dat";
    cleanup(path);

    StorageBackend* sb = storage_open(path, 8192);
    CHECK(sb != NULL);
    if (!sb) return;

    /* Allocate(1) returns page 2 (0 and 1 are reserved) */
    int64_t p = storage_allocate(sb, 1);
    CHECK_EQ(p, 2);

    /* Allocate(10) returns sequential pages */
    int64_t p2 = storage_allocate(sb, 10);
    CHECK_EQ(p2, 3);

    /* Acquire on free page */
    int ok = storage_acquire(sb, 20);
    CHECK_EQ(ok, 1);

    /* Second acquire on same page fails */
    ok = storage_acquire(sb, 20);
    CHECK_EQ(ok, 0);

    /* Free */
    storage_free(sb, 20);
    int64_t offset = indir_lookup(sb, 20);
    CHECK_EQ(offset, 0);

    storage_close(sb);
    cleanup(path);
}

void test_read_write(void) {
    printf("3. Read/Write/Flush...\n");
    const char* path = "/tmp/test_rw_vfs.dat";
    cleanup(path);

    StorageBackend* sb = storage_open(path, 8192);
    CHECK(sb != NULL);
    if (!sb) return;

    /* Allocate page 2 */
    int64_t pg = storage_allocate(sb, 1);
    CHECK_EQ(pg, 2);

    /* Write a known payload */
    uint8_t payload[8192];
    memset(payload, 0xAB, 8192);
    storage_write(sb, pg, payload, 0);

    /* Read back */
    uint8_t* result = storage_read(sb, pg);
    CHECK(result != NULL);
    if (result) {
        CHECK_EQ(memcmp(result, payload, 8192), 0);
    }

    /* Read never-written page returns NULL */
    uint8_t* null_result = storage_read(sb, 50);
    CHECK(null_result == NULL);

    /* Flush and verify persistence */
    storage_flush(sb, -1);
    storage_close(sb);

    /* Reopen and read */
    sb = storage_open(path, 8192);
    CHECK(sb != NULL);
    if (sb) {
        result = storage_read(sb, pg);
        CHECK(result != NULL);
        if (result) {
            CHECK_EQ(memcmp(result, payload, 8192), 0);
        }
        storage_close(sb);
    }

    cleanup(path);
}

void test_lazy_mirror(void) {
    printf("4. Lazy mirror...\n");
    const char* path = "/tmp/test_mirror_vfs.dat";
    cleanup(path);

    StorageBackend* sb = storage_open(path, 8192);
    CHECK(sb != NULL);
    if (!sb) return;

    int64_t pg = storage_allocate(sb, 1);

    /* First write */
    uint8_t buf1[8192];
    memset(buf1, 0x11, 8192);
    storage_write(sb, pg, buf1, 0);

    uint8_t* r = storage_read(sb, pg);
    CHECK(r != NULL);
    if (r) CHECK_EQ(memcmp(r, buf1, 8192), 0);

    /* Second write (triggers mirror allocation) */
    uint8_t buf2[8192];
    memset(buf2, 0x22, 8192);
    storage_write(sb, pg, buf2, 0);

    r = storage_read(sb, pg);
    CHECK(r != NULL);
    if (r) CHECK_EQ(memcmp(r, buf2, 8192), 0);

    /* Third write (alternates between mirrors) */
    uint8_t buf3[8192];
    memset(buf3, 0x33, 8192);
    storage_write(sb, pg, buf3, 0);

    r = storage_read(sb, pg);
    CHECK(r != NULL);
    if (r) CHECK_EQ(memcmp(r, buf3, 8192), 0);

    storage_close(sb);
    cleanup(path);
}

void test_flush_order(void) {
    printf("5. Flush priority order...\n");
    const char* path = "/tmp/test_flush_vfs.dat";
    cleanup(path);

    StorageBackend* sb = storage_open(path, 8192);
    CHECK(sb != NULL);
    if (!sb) return;

    int64_t pg = storage_allocate(sb, 1);
    uint8_t data[8192];
    memset(data, 0xCC, 8192);
    storage_write(sb, pg, data, 0);

    /* Flush(-1) should succeed and persist */
    storage_flush(sb, -1);
    storage_close(sb);

    sb = storage_open(path, 8192);
    CHECK(sb != NULL);
    if (sb) {
        uint8_t* r = storage_read(sb, pg);
        CHECK(r != NULL);
        if (r) CHECK_EQ(memcmp(r, data, 8192), 0);
        storage_close(sb);
    }

    cleanup(path);
}

/* ========================================================================== */

static StorageBackend* s_shared_sb = NULL;

static void* mt_alloc_thread(void* arg) {
    int tid = *(int*)arg;
    for (int i = 0; i < 100; i++) {
        int64_t pg = storage_allocate(s_shared_sb, 1);
        if (pg >= 0) {
            uint8_t data[8192];
            memset(data, (uint8_t)(tid & 0xFF), 8192);
            storage_write(s_shared_sb, pg, data, 0);
        }
    }
    return NULL;
}

void test_concurrent(void) {
    printf("6. Concurrent allocate+write (4 threads)...\n");
    const char* path = "/tmp/test_mt_vfs.dat";
    cleanup(path);

    s_shared_sb = storage_open(path, 8192);
    CHECK(s_shared_sb != NULL);
    if (!s_shared_sb) return;

    pthread_t th[4];
    int tids[4] = {0, 1, 2, 3};
    for (int i = 0; i < 4; i++) {
        pthread_create(&th[i], NULL, mt_alloc_thread, &tids[i]);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(th[i], NULL);
    }

    /* total_pages should be >= 2 + 400 (4 threads × 100 pages each) */
    CHECK(s_shared_sb->total_pages >= 402);

    /* Verify we can read some pages */
    int readable = 0;
    for (int64_t pg = 2; pg < s_shared_sb->total_pages; pg++) {
        uint8_t* r = storage_read(s_shared_sb, pg);
        if (r) readable++;
    }
    CHECK(readable >= 400);

    storage_close(s_shared_sb);
    s_shared_sb = NULL;
    cleanup(path);
}

/* ========================================================================== */

int main(void) {
    printf("=== StorageBackend Tests ===\n\n");

    test_create_open();
    test_allocate();
    test_read_write();
    test_lazy_mirror();
    test_flush_order();
    test_concurrent();

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
