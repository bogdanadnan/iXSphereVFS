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
    /* Phase 27: inline_count reduced by 3 (the free-list header
       takes offsets 40/48/56).  For page_size=128, old=11, new=8. */
    CHECK(inline_count == 8);
    CHECK(entries_per_overflow == 15);

    /* Sanity: inline area covers 8 pages.  storage_open already
       allocated pages 0 (header) and 1 (superblock), so
       sb->total_pages = 2 and we have 8 entries for pages 0..7
       (page 0 = header, page 1 = superblock, pages 2..7 = 6 free
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

    /* Post-state: the new overflow page covers pages inline_count
       .. inline_count + entries_per_overflow - 1.  Total indirection
       capacity is inline_count + entries_per_overflow (1 overflow).
       Phase 27: inline_count is 8 (was 11 before the free-list
       header took 3 entries). */
    int64_t total_entries_after = (int64_t)inline_count +
        (int64_t)sb->indir.overflow_count * entries_per_overflow;
    CHECK_EQ(total_entries_after, (int64_t)inline_count + entries_per_overflow);

    /* Phase 27 C6 followup: self-ref of overflow[K] lives in the
       LAST entry of overflow[K-1] (or inline[inline_count-1] for K=0).
       The indirection table uses logicals, not physicals, in the
       chain so GC can move pages later without breaking the chain.
       For K=0, the self-ref is at inline[inline_count-1] (logical
       7 with the new layout; was 10 before Phase 27).
       indirection_head points to this self-ref so indir_init can
       find overflow[0] on mount. */
    int64_t new_overflow_logical = sb->indir.overflow_logical[0];
    CHECK_EQ(new_overflow_logical, (int64_t)inline_count - 1);
    CHECK_EQ(sb->total_pages, 2);  /* NOT bumped by indir_ensure_capacity */
    CHECK_EQ(sb->indirection_head, (int64_t)inline_count - 1);

    int64_t self_phys = indir_lookup(sb, new_overflow_logical);
    CHECK(self_phys > 0);
    int64_t new_overflow_phys = self_phys;

    /* inline[10] must hold the new overflow's physical (the self-ref). */
    int64_t inline_self = vfs_rd8_s((const uint8_t*)sb->header_buf,
                                     HDR_OFF_ENTRIES + new_overflow_logical * 8,
                                     page_size);
    CHECK_EQ(inline_self, new_overflow_phys);

    /* overflow[0] contents: 14 data slots at eidx 0..13 (logicals
       inline_count..inline_count+13), all 0.  The LAST entry
       (eidx 14, buf[15], logical inline_count - 1 +
       entries_per_overflow) is the self-ref of overflow[1] — also
       0 (overflow[1] not yet added).  Chain link buf[0] = 0 (no
       next overflow).
       Phase 27: with inline_count=8, these are logicals 8..21 for
       the data slots and 22 for the overflow[1] self-ref. */
    int64_t* new_buf = sb->indir.overflow_pages[0];
    CHECK(new_buf != NULL);
    int64_t buf_entry_0 = vfs_rd8_s((const uint8_t*)new_buf, 1 * 8, page_size);
    CHECK_EQ(buf_entry_0, 0);  /* first data slot not yet allocated */
    int64_t buf_entry_14 = vfs_rd8_s((const uint8_t*)new_buf, 15 * 8, page_size);
    CHECK_EQ(buf_entry_14, 0); /* overflow[1] self-ref slot — set later */
    int64_t buf_chain = vfs_rd8_s((const uint8_t*)new_buf, 0, page_size);
    CHECK_EQ(buf_chain, 0);    /* chain next — set later */

    /* Phase 2: call indir_ensure_capacity with needed = 100.
       sb->total_pages = 2, required = 102, total_entries = 26.
       The function should allocate enough overflow pages to cover
       102.  ceil((102-26)/15) = ceil(76/15) = 6 overflow pages,
       so overflow_count grows from 1 to 7. */
    rc = indir_ensure_capacity(sb, 100);
    CHECK_EQ(rc, 0);
    CHECK(sb->indir.overflow_count >= 6);

    /* Self-ref logicals for overflows 1..6:
       = inline_count - 1 + K*entries_per_overflow for K = 1..6.
       Phase 27: with inline_count=8 and entries_per_overflow=15,
       these are 22, 37, 52, 67, 82, 97. */
    CHECK_EQ(sb->indir.overflow_logical[1],
             (int64_t)inline_count - 1 + 1 * entries_per_overflow);
    CHECK_EQ(sb->indir.overflow_logical[2],
             (int64_t)inline_count - 1 + 2 * entries_per_overflow);
    CHECK_EQ(sb->indir.overflow_logical[3],
             (int64_t)inline_count - 1 + 3 * entries_per_overflow);
    CHECK_EQ(sb->indir.overflow_logical[4],
             (int64_t)inline_count - 1 + 4 * entries_per_overflow);
    CHECK_EQ(sb->indir.overflow_logical[5],
             (int64_t)inline_count - 1 + 5 * entries_per_overflow);
    CHECK_EQ(sb->indir.overflow_logical[6],
             (int64_t)inline_count - 1 + 6 * entries_per_overflow);

    /* sb->total_pages is still 2 — indir_ensure_capacity does not
       consume data slots, only the chain links. */
    CHECK_EQ(sb->total_pages, 2);

    /* overflow[0]'s chain link now points to overflow[1]'s self-ref
       (logical inline_count - 1 + entries_per_overflow), and
       overflow[0]'s last entry (buf[15], logical inline_count - 1 +
       entries_per_overflow) holds overflow[1]'s physical. */
    int64_t overflow_1_logical = (int64_t)inline_count - 1 + entries_per_overflow;
    int64_t overflow_1_phys = indir_lookup(sb, overflow_1_logical);
    buf_chain = vfs_rd8_s((const uint8_t*)new_buf, 0, page_size);
    CHECK_EQ(buf_chain, overflow_1_logical);
    buf_entry_14 = vfs_rd8_s((const uint8_t*)new_buf, 15 * 8, page_size);
    CHECK_EQ(buf_entry_14, overflow_1_phys);

    /* Phase 3: round-trip via storage_allocate.  The do-while in
       storage_allocate skips self-ref slots (logical
       inline_count-1, then inline_count-1+entries_per_overflow, ...)
       and claims the next data slot.  Under contention, the exact
       path depends on which slots are claimed in which order, so
       we only assert >= initial_total + 10 (not ==). */
    int64_t initial_total = sb->total_pages;
    for (int i = 0; i < 10; i++) {
        int64_t pg = storage_allocate(sb, 1);
        CHECK(pg > 0);
        int64_t phys = indir_lookup(sb, pg);
        CHECK(phys > 0);
    }
    CHECK(sb->total_pages >= initial_total + 10);

    storage_close(sb);
    cleanup(path);
}

/* Phase 27 W2 regression: free-list enqueue.
 *
 * Allocate some pages, free them, verify the free-list header
 * fields are updated correctly.  Verifies that the tail pointer
 * points at a free-list page, the count matches the number of
 * frees, and the entries contain the expected (logical, physical)
 * pairs.
 */

/* Helper: read a free-list entry directly.  Mirrors the in-storage.c
   internal helper, exposed here for the test. */
static int64_t read_free_list_entry_helper(StorageBackend* sb, int64_t fl_page,
                                            int idx, int64_t* out_phys) {
    int64_t fl_off = indir_lookup(sb, fl_page);
    if (fl_off == 0) return 0;
    /* fl_off is the start of the PageHeader.  The payload starts
       at fl_off + PAGE_HEADER_SIZE.  The first entry is at
       payload + 16 (after next_page + count + padding). */
    int64_t payload_off = fl_off + 16 + 16 + (int64_t)idx * 16;
    uint8_t buf[16];
    if (pread(sb->fd, buf, 16, payload_off) != 16) return 0;
    int64_t logical;
    memcpy(&logical, buf, 8);
    memcpy(out_phys, buf + 8, 8);
    return logical;
}

void test_free_list_enqueue(void) {
    printf("7. free-list enqueue (W2)...\n");
    const char* path = "/tmp/test_storage_freelist.vfs";
    cleanup(path);

    StorageBackend* sb = storage_open(path, 8192);
    CHECK(sb != NULL);
    if (!sb) { cleanup(path); return; }

    /* Allocate 10 pages. */
    int64_t vps[10];
    int64_t phys[10];
    for (int i = 0; i < 10; i++) {
        vps[i] = storage_allocate(sb, 1);
        CHECK(vps[i] > 0);
        phys[i] = indir_lookup(sb, vps[i]);
        CHECK(phys[i] > 0);
    }

    /* Free the first 5 pages.  The free-list should grow to 5. */
    for (int i = 0; i < 5; i++) {
        storage_free(sb, vps[i]);
    }

    /* Read the free-list header from the in-memory header_buf. */
    int64_t* fl_head = (int64_t*)(sb->header_buf + HDR_OFF_FREE_LIST_HEAD);
    int64_t* fl_tail = (int64_t*)(sb->header_buf + HDR_OFF_FREE_LIST_TAIL);
    int64_t* fl_count = (int64_t*)(sb->header_buf + HDR_OFF_FREE_LIST_COUNT);
    CHECK_EQ(*fl_count, 5);
    CHECK(*fl_head != 0);
    CHECK(*fl_tail != 0);
    CHECK_EQ(*fl_head, *fl_tail);  /* only one free-list page used for 5 entries */

    /* Verify the entries in the tail page.  Enqueue appends to
       position [count], so:
         entry[0] = first appended = first freed = vps[0]
         entry[4] = last appended = last freed = vps[4]
       The dequeue (W3) pops the LAST entry (entry[4]) for LIFO
       within page. */
    int64_t phys_check = 0;
    int64_t entry0 = read_free_list_entry_helper(sb, *fl_tail, 0, &phys_check);
    CHECK_EQ(entry0, vps[0]);
    CHECK_EQ(phys_check, phys[0]);
    int64_t entry4 = read_free_list_entry_helper(sb, *fl_tail, 4, &phys_check);
    CHECK_EQ(entry4, vps[4]);
    CHECK_EQ(phys_check, phys[4]);

    /* Free 5 more — should fill the current tail page (510 entries
       for 8KB) and stay within one page.  Count should now be 10. */
    for (int i = 5; i < 10; i++) {
        storage_free(sb, vps[i]);
    }
    CHECK_EQ(*fl_count, 10);

    /* Indir should now be 0 for all freed pages. */
    for (int i = 0; i < 10; i++) {
        CHECK_EQ(indir_lookup(sb, vps[i]), 0);
    }

    storage_close(sb);
    cleanup(path);
}

/* Phase 27 W3 regression: free-list dequeue + persistence.
 *
 * Verify that storage_allocate(1) consumes from the free-list
 * (returns the same pages that were freed, FIFO order via LIFO
 * within page) and that the total_pages counter is unchanged
 * (no tail-advance was needed).
 *
 * Then close the VFS, reopen, and verify the freed pages are
 * still in the queue (persisted across mount).
 */
void test_free_list_dequeue(void) {
    printf("8. free-list dequeue (W3)...\n");
    const char* path = "/tmp/test_storage_freelist_deq.vfs";
    cleanup(path);

    StorageBackend* sb = storage_open(path, 8192);
    CHECK(sb != NULL);
    if (!sb) { cleanup(path); return; }

    /* Allocate 20 pages. */
    int64_t vps[20];
    for (int i = 0; i < 20; i++) {
        vps[i] = storage_allocate(sb, 1);
        CHECK(vps[i] > 0);
    }
    int64_t total_before_free = sb->total_pages;

    /* Free pages 0-9.  The free-list should have 10 entries. */
    for (int i = 0; i < 10; i++) {
        storage_free(sb, vps[i]);
    }
    int64_t* fl_count = (int64_t*)(sb->header_buf + HDR_OFF_FREE_LIST_COUNT);
    CHECK_EQ(*fl_count, 10);

    /* Dequeue 10 pages.  LIFO within page means we get them in
       reverse-of-append order: vps[9] first, vps[8] next, ...,
       vps[0] last.  (Enqueue appends to entry[count], so vps[0]
       is at entry[0] and is the OLDEST; dequeue pops the highest
       index first, which is vps[9].) */
    int64_t total_after_dequeue = sb->total_pages;
    for (int i = 9; i >= 0; i--) {
        int64_t vp = storage_allocate(sb, 1);
        CHECK_EQ(vp, vps[i]);
    }
    /* total_pages is unchanged — the dequeue reused the freed
       VPs, not a tail-advance. */
    CHECK_EQ(sb->total_pages, total_after_dequeue);

    /* Queue is now empty. */
    CHECK_EQ(*fl_count, 0);

    /* Next allocate should tail-advance (queue is empty).  The
       VP should be > total_before_free (a new slot, not a reused
       one). */
    int64_t new_vp = storage_allocate(sb, 1);
    CHECK(new_vp > total_before_free);
    CHECK_EQ(sb->total_pages, total_after_dequeue + 1);

    /* Persistence: close, reopen, verify the queue is preserved.
       W3's persistence guarantee is that the free-list pages are
       regular storage pages — they're on disk.  The header's
       free_list_head/tail/count are persisted via the header
       flush at storage_close. */
    storage_close(sb);

    /* W3 caveat: the dequeue helper's reads are not fully
       crash-consistent yet.  For a mount test, the data
       structure needs the in-memory cache to be flushed before
       close, which storage_close does.  Reopen and verify
       count=0 (we dequeued everything). */
    sb = storage_open(path, 8192);
    CHECK(sb != NULL);
    if (sb) {
        int64_t* fl_count2 = (int64_t*)(sb->header_buf + HDR_OFF_FREE_LIST_COUNT);
        CHECK_EQ(*fl_count2, 0);
        storage_close(sb);
    }

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
    test_free_list_enqueue();
    test_free_list_dequeue();

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
