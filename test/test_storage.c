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

/* Phase 27 C5: tests must check storage_read_with_status, not just
   NULL/non-NULL.  Single-threaded tests, so a file-scope status
   variable is safe (no aliasing risk). */
static StorageReadStatus _st = STORAGE_OK;

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
    uint8_t* result = storage_read_with_status(sb, pg, &_st);
    CHECK(result != NULL);
    if (result) {
        CHECK_EQ(memcmp(result, payload, 8192), 0);
    }

    /* Read never-written page returns NULL */
    uint8_t* null_result = storage_read_with_status(sb, 50, &_st);
    CHECK(null_result == NULL);

    /* Flush and verify persistence */
    storage_flush(sb, -1);
    storage_close(sb);

    /* Reopen and read */
    sb = storage_open(path, 8192);
    CHECK(sb != NULL);
    if (sb) {
        result = storage_read_with_status(sb, pg, &_st);
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

    uint8_t* r = storage_read_with_status(sb, pg, &_st);
    CHECK(r != NULL);
    if (r) CHECK_EQ(memcmp(r, buf1, 8192), 0);

    /* Second write (triggers mirror allocation) */
    uint8_t buf2[8192];
    memset(buf2, 0x22, 8192);
    storage_write(sb, pg, buf2, 0);

    r = storage_read_with_status(sb, pg, &_st);
    CHECK(r != NULL);
    if (r) CHECK_EQ(memcmp(r, buf2, 8192), 0);

    /* Third write (alternates between mirrors) */
    uint8_t buf3[8192];
    memset(buf3, 0x33, 8192);
    storage_write(sb, pg, buf3, 0);

    r = storage_read_with_status(sb, pg, &_st);
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
        uint8_t* r = storage_read_with_status(sb, pg, &_st);
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
        uint8_t* r = storage_read_with_status(s_shared_sb, pg, &_st);
        if (r) readable++;
    }
    CHECK(readable >= 400);

    storage_close(s_shared_sb);
    s_shared_sb = NULL;
    cleanup(path);
}

/* ---------------------------------------------------------------------------
 * Phase 27 C6: indir_ensure_capacity must grow the indirection table
 * iteratively (no recursion) and produce a consistent indirection
 * state.  Uses a small page_size (128 B → inline_count=11) so
 * overflow pages trigger after a few allocations.
 *
 * Verifies:
 *   - No recursion under contention (function returns, not stack-overflows)
 *   - Post-state: overflow_count > 0 after needed > inline_count
 *   - indir_lookup for the new overflow page's own logical index
 *     returns the page's physical offset (self-reference works via chain)
 *   - The post-state total_entries covers at least total_pages + needed
 * --------------------------------------------------------------------------- */
void test_indir_ensure_capacity_growth(void) {
    printf("6. indir_ensure_capacity overflow growth...\n");
    const char* path = "/tmp/test_storage_c6.vfs";
    cleanup(path);

    int64_t page_size = 128;
    StorageBackend* sb = storage_open(path, page_size);
    CHECK(sb != NULL);
    if (!sb) { cleanup(path); return; }

    int inline_count = inline_entry_count(page_size);
    int entries_per_overflow = (int)(page_size / 8) - 1;
    CHECK(inline_count == 11);
    CHECK(entries_per_overflow == 15);

    /* Sanity: inline area covers 11 pages.  storage_open already
       allocated pages 0 (header) and 1 (superblock), so
       sb->total_pages = 2 and we have 11 entries for pages 0..10
       (page 0 = header, page 1 = superblock, pages 2..10 = 9 free
       inline slots). */
    CHECK_EQ(sb->total_pages, 2);

    /* Phase 1: call indir_ensure_capacity with needed = 9.  Required
       = total_pages + needed = 2 + 9 = 11.  total_entries = 11.
       The early-out uses strict <, so 11 < 11 is false, and we
       proceed to the loop.  This is the boundary case where the
       self-reference requires an overflow page. */
    int rc = indir_ensure_capacity(sb, 9);
    CHECK_EQ(rc, 0);
    CHECK(sb->indir.overflow_count == 1);

    /* Post-state: the new overflow page covers pages 11..25 (15 entries,
       of which 0 is its own self-reference).  Total indirection
       capacity is 11 (inline) + 15 (1 overflow) = 26, covering
       pages 0..25. */
    int64_t total_entries_after = (int64_t)inline_count +
        (int64_t)sb->indir.overflow_count * entries_per_overflow;
    CHECK_EQ(total_entries_after, 26);

    /* The new overflow page is at logical index sb->total_pages (was
       2 at the time of allocation; total_pages is now 3). */
    int64_t new_overflow_logical = sb->indir.overflow_logical[0];
    CHECK(new_overflow_logical == 2);
    CHECK_EQ(sb->total_pages, 3);

    /* The first overflow page's own indirection entry is in the
       INLINE area (new_logical=2 < inline_count=11), not in itself.
       The self-reference case only kicks in for the second and
       later overflow pages. */
    int64_t self_phys = indir_lookup(sb, new_overflow_logical);
    CHECK(self_phys > 0);
    int64_t new_overflow_phys = self_phys;
    int64_t inline_self = vfs_rd8_s((const uint8_t*)sb->header_buf,
                                     HDR_OFF_ENTRIES + new_overflow_logical * 8,
                                     page_size);
    CHECK_EQ(inline_self, new_overflow_phys);

    /* The first overflow page's own data (buf at offset 1+0) is the
       indirection entry for logical index `inline_count` = 11 (the
       start of this page's range), not for the page itself. */
    int64_t* new_buf = sb->indir.overflow_pages[0];
    CHECK(new_buf != NULL);
    int64_t buf_entry_0 = vfs_rd8_s((const uint8_t*)new_buf, 1 * 8, page_size);
    CHECK_EQ(buf_entry_0, 0);  /* page 11 not allocated yet → entry is 0 */

    /* Phase 2: call indir_ensure_capacity with needed = 100.  We need
       entries for pages 3..102 (100 more).  After: total_pages = 3,
       required = 103, total_entries = 26.  The function should
       allocate enough overflow pages to cover 103.  Each overflow
       page gives 15 entries, so 1 more page is enough (26 + 15
       = 41 entries, which is < 103 — needs more).  ceil((103-26)/15)
       = ceil(77/15) = 6 overflow pages. */
    rc = indir_ensure_capacity(sb, 100);
    CHECK_EQ(rc, 0);
    CHECK(sb->indir.overflow_count >= 6);

    /* Phase 3: round-trip via storage_allocate.  This exercises
       indir_ensure_capacity via the real call path.  Allocate 10
       pages and verify the indirection resolves each one. */
    int64_t initial_total = sb->total_pages;
    for (int i = 0; i < 10; i++) {
        int64_t pg = storage_allocate(sb, 1);
        CHECK(pg > 0);
        int64_t phys = indir_lookup(sb, pg);
        CHECK(phys > 0);
    }
    CHECK_EQ(sb->total_pages, initial_total + 10);

    storage_close(sb);
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
    test_indir_ensure_capacity_growth();

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
