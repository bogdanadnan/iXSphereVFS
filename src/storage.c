#include "storage.h"
#include "page_buf.h"
#include "gc.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* Pointer to the GC's deferred-free queue, set during GC to prevent
   re-allocation of pages still referenced by in-flight readers. */
static DeferredFreeQueue* _deferred_queue = NULL;

/* Cache performance counters (atomic for lock-free reads in benchmark code) */
static volatile int64_t _cache_total = 0;
static volatile int64_t _cache_hits       = 0;
static volatile int64_t _vfs_data_total = 0;
static volatile int64_t _vfs_data_hits  = 0;
static volatile int     _last_was_hit     = 0;

int64_t vfs_cache_get_max_entries(StorageBackend* sb) {
    return sb ? (int64_t)sb->cache.max_entries : (int64_t)CACHE_DEFAULT_MAX;
}

void vfs_cache_evict_all(StorageBackend* sb) {
    if (sb) cache_evict_all(sb);
}

void vfs_cache_reset(void) {
    _cache_total      = 0;
    _cache_hits       = 0;
    _vfs_data_total = 0;
    _vfs_data_hits  = 0;
}

int64_t vfs_cache_total(void)      { return _cache_total; }
int64_t vfs_cache_hits(void)       { return _cache_hits; }
int64_t vfs_data_total(void) { return _vfs_data_total; }
int64_t vfs_data_hits(void)  { return _vfs_data_hits; }
int     vfs_cache_was_last_hit(void) { return _last_was_hit; }

void vfs_data_inc_total(void) { _vfs_data_total++; }
void vfs_data_inc_hits(void)  { _vfs_data_hits++; }
void storage_set_deferred_queue(DeferredFreeQueue* queue) {
    _deferred_queue = queue;
}

/* ---------------------------------------------------------------------------
 * Physical offset helpers
 * --------------------------------------------------------------------------- */

int64_t phys_record_size(StorageBackend* sb) {
    return sb->page_size + PAGE_HEADER_SIZE;
}

/* ---------------------------------------------------------------------------
 * Header page layout (payload only — offset 0..page_size-1)
 *
 *  0   int64_t  total_pages
 *  8   int64_t  page_size
 * 16   uint32_t segment_size
 * 20   uint32  reserved
 * 24   int64_t  physical_tail
 * 32   int64_t  indirection_head
 * 40   int64_t  entries[inline_count]
 * --------------------------------------------------------------------------- */

#define HDR_OFF_TOTAL_PAGES       0
#define HDR_OFF_PAGE_SIZE         8
#define HDR_OFF_SEGMENT_SIZE      16
#define HDR_OFF_PHYS_TAIL         24
#define HDR_OFF_INDIR_HEAD        32
#define HDR_OFF_FREE_LIST_HEAD    40
#define HDR_OFF_FREE_LIST_TAIL    48
#define HDR_OFF_FREE_LIST_COUNT   56
#define HDR_OFF_ENTRIES           64

/* Number of inline indirection entries in the header page.
   Reduced by 3 in Phase 27 to make room for the free-list header
   at offsets 40/48/56. */
int inline_entry_count(int64_t page_size) {
    return (int)((page_size - HDR_OFF_ENTRIES) / 8);
}

/* ---------------------------------------------------------------------------
 * Bootstrap: create a new backing file
 * --------------------------------------------------------------------------- */

static int bootstrap_new(StorageBackend* sb) {
    int64_t ps = sb->page_size;

    /* Allocate header page buffer */
    sb->header_buf = calloc(1, (size_t)ps);
    if (!sb->header_buf) return -1;

    /* Write header fields */
    vfs_wr8_s(sb->header_buf, HDR_OFF_TOTAL_PAGES,  2, sb->page_size);     /* pages 0 and 1 reserved */
    vfs_wr8_s(sb->header_buf, HDR_OFF_PAGE_SIZE,     ps, sb->page_size);
    vfs_wr4_s(sb->header_buf, HDR_OFF_SEGMENT_SIZE,  1024, sb->page_size);
    vfs_wr8_s(sb->header_buf, HDR_OFF_PHYS_TAIL,     (int64_t)(2LL * (ps + PAGE_HEADER_SIZE)), sb->page_size);
    vfs_wr8_s(sb->header_buf, HDR_OFF_INDIR_HEAD,    0, sb->page_size);

    /* Phase 27: free-page queue header.  A fresh VFS has no free
       pages, so all three fields are 0.  The W2 (enqueue) and W3
       (dequeue) workloads will populate these as storage_free is
       called. */
    vfs_wr8_s(sb->header_buf, HDR_OFF_FREE_LIST_HEAD,  0, sb->page_size);
    vfs_wr8_s(sb->header_buf, HDR_OFF_FREE_LIST_TAIL,  0, sb->page_size);
    vfs_wr8_s(sb->header_buf, HDR_OFF_FREE_LIST_COUNT, 0, sb->page_size);

    /* Indirection entries: page 0 at physical 0, page 1 at physical (ps+16) */
    int ic = inline_entry_count(ps);
    /* Zero all entries first */
    for (int i = 0; i < ic; i++) {
        vfs_wr8_s(sb->header_buf, HDR_OFF_ENTRIES + i * 8, 0, sb->page_size);
    }
    vfs_wr8_s(sb->header_buf, HDR_OFF_ENTRIES + 0 * 8, 0, sb->page_size);                   /* page 0 → offset 0 */
    vfs_wr8_s(sb->header_buf, HDR_OFF_ENTRIES + 1 * 8, ps + PAGE_HEADER_SIZE, sb->page_size); /* page 1 → offset ps+16 */

    /* Write header page to disk with XVFS magic */
    int64_t header_offset = 0;
    uint32_t crc = vfs_crc32c(sb->header_buf, (size_t)ps);

    PageHeader ph;
    memset(&ph, 0, sizeof(ph));
    ph.flags       = XVFS_MAGIC;
    ph.checksum    = crc;
    ph.generation  = 1;
    ph.mirror_page = -1;

    /* Write PageHeader */
    ssize_t n = pwrite(sb->fd, &ph, PAGE_HEADER_SIZE, header_offset);
    if (n != PAGE_HEADER_SIZE) return -1;

    /* Write payload */
    n = pwrite(sb->fd, sb->header_buf, (size_t)ps, header_offset + PAGE_HEADER_SIZE);
    if (n != ps) return -1;

    /* Zero-fill page 1 (superblock placeholder) */
    uint8_t* zero_buf = calloc(1, (size_t)ps);
    if (!zero_buf) return -1;

    PageHeader ph1;
    memset(&ph1, 0, sizeof(ph1));
    ph1.generation  = 0;
    ph1.mirror_page = -1;
    ph1.checksum    = vfs_crc32c(zero_buf, (size_t)ps);

    int64_t p1_off = ps + PAGE_HEADER_SIZE;
    n = pwrite(sb->fd, &ph1, PAGE_HEADER_SIZE, p1_off);
    if (n == PAGE_HEADER_SIZE) {
        n = pwrite(sb->fd, zero_buf, (size_t)ps, p1_off + PAGE_HEADER_SIZE);
    }
    free(zero_buf);
    if (n != ps) return -1;

    fsync(sb->fd);

    /* Sync in-memory tracking — bootstrap wrote page 0 with generation=1 */
    sb->total_pages      = 2;
    sb->segment_size     = 1024;
    sb->physical_tail    = 2 * (ps + PAGE_HEADER_SIZE);
    sb->indirection_head = 0;

    return 0;
}

/* ---------------------------------------------------------------------------
 * Mount: open an existing backing file
 * --------------------------------------------------------------------------- */

static int mount_existing(StorageBackend* sb) {
    /* Read PageHeader at offset 0 */
    PageHeader ph;
    ssize_t n = pread(sb->fd, &ph, PAGE_HEADER_SIZE, 0);
    if (n != PAGE_HEADER_SIZE) return -1;

    /* Validate XVFS magic */
    if (ph.flags != XVFS_MAGIC) return -1;

    /* Step 1: Read only the config area (first 40 bytes) to discover page_size.
       The config fields (total_pages, page_size, etc.) are within the first
       40 bytes of the payload — this works regardless of actual page_size. */
    uint8_t tmp[40];
    n = pread(sb->fd, tmp, 40, PAGE_HEADER_SIZE);
    if (n != 40) return -1;

    int64_t ps;
    memcpy(&ps, tmp + HDR_OFF_PAGE_SIZE, 8);
    if (ps < 512 || ps > 65536) return -1;

    /* Step 2: Read the full page_size payload and validate CRC over ALL of it */
    uint8_t* hdr = calloc(1, (size_t)ps);
    if (!hdr) return -1;

    n = pread(sb->fd, hdr, (size_t)ps, PAGE_HEADER_SIZE);
    if (n != ps) { free(hdr); return -1; }

    uint32_t crc = vfs_crc32c(hdr, (size_t)ps);
    if (crc != ph.checksum) { free(hdr); return -1; }

    sb->page_size       = ps;
    sb->total_pages      = vfs_rd8_s(hdr, HDR_OFF_TOTAL_PAGES, sb->page_size);
    sb->segment_size     = (uint32_t)vfs_rd4_s(hdr, HDR_OFF_SEGMENT_SIZE, sb->page_size);
    sb->physical_tail    = vfs_rd8_s(hdr, HDR_OFF_PHYS_TAIL, sb->page_size);
    sb->indirection_head = vfs_rd8_s(hdr, HDR_OFF_INDIR_HEAD, sb->page_size);
    sb->header_buf       = hdr;

    /* Phase 27: load free-page queue header.  The 3 fields were
       zero-initialized by bootstrap_new, so a freshly-created VFS
       has free_list_count=0 (queue empty).  W1 doesn't populate
       the queue — that's W2.  W1 just preserves the on-disk
       layout so W2/W3 can read it back. */
    /* (W2/W3 will read these from the header via
       vfs_atomic_load_i64 on the in-memory header_buf.) */

    /* Sync in-memory lazy mirror tracking from on-disk PageHeader */

    return 0;
}

/* ---------------------------------------------------------------------------
 * Allocate mirror/generation tracking arrays
 * --------------------------------------------------------------------------- */


/* ---------------------------------------------------------------------------
 * storage_open / storage_close  (W2.6)
 * --------------------------------------------------------------------------- */

/* Phase 27 W5: validate the free-list count after mount.  Called
 * from storage_open AFTER indir_init and cache_init, so the
 * indirection table can resolve free-list page physicals.
 *
 * After a crash in the CAS-to-flush window (a free-list page's
 * `count` was CAS'd from N to N+1, the entry was written, but the
 * dirty mark hadn't been flushed to disk), the on-disk global
 * `free_list_count` may be ahead of the actual valid entries.
 * We walk the chain (using raw pread — NOT the cache, to avoid
 * putting free-list pages in the cache and triggering a flush
 * during the walk that would allocate a mirror sibling and
 * consume a free entry) and:
 *   - Sum the valid entries (non-zero phys within file size).
 *   - If the stored free_list_count differs, correct the
 *     in-memory header_buf, pwrite the new value to disk, and
 *     refresh the header's CRC.
 *
 * Note: per-page count validation is intentionally NOT done here.
 * The dequeue's try_claim_entry is a final guard against returning
 * a page with stale indir, and a separate per-page walk would
 * require updating the free-list page's CRC (and would also risk
 * the flush recursion).  Per-page count is checked lazily on
 * dequeue. */
static void validate_free_list_on_mount(StorageBackend* sb) {
    if (!sb || !sb->header_buf) return;
    int64_t* fl_head_ptr  = (int64_t*)(sb->header_buf + HDR_OFF_FREE_LIST_HEAD);
    int64_t* fl_count_ptr = (int64_t*)(sb->header_buf + HDR_OFF_FREE_LIST_COUNT);
    int64_t head  = vfs_atomic_load_i64(fl_head_ptr);
    int64_t file_size = lseek(sb->fd, 0, SEEK_END);
    if (file_size < 0) file_size = 0;
    int64_t total_valid = 0;
    int64_t cur = head;
    int page_walked = 0;
    /* Safety limit: a cycle or corrupted chain could loop forever.
       10000 pages = ~80 MB of free-list metadata, which is many
       times more than any realistic VFS. */
    while (cur != 0 && page_walked < 10000) {
        int64_t fl_off = indir_lookup(sb, cur);
        if (fl_off == 0) break;
        /* Read the count (payload offset 8) via raw pread. */
        int64_t payload_off = fl_off + PAGE_HEADER_SIZE;
        uint8_t buf[4];
        if (pread(sb->fd, buf, 4, payload_off + 8) != 4) break;
        int32_t stored_count;
        memcpy(&stored_count, buf, 4);
        int max_per_page = (int)((sb->page_size - 16) / 16);
        int scan = (stored_count >= 0 && stored_count <= max_per_page)
                   ? stored_count : max_per_page;
        int valid_count = 0;
        for (int i = 0; i < scan; i++) {
            int64_t eoff = payload_off + 16 + (int64_t)i * 16 + 8;
            uint8_t pbuf[8];
            if (pread(sb->fd, pbuf, 8, eoff) != 8) break;
            int64_t phys;
            memcpy(&phys, pbuf, 8);
            if (phys != 0 && (file_size == 0 || phys < file_size)) {
                valid_count++;
            }
        }
        total_valid += valid_count;
        /* Read next_page (payload offset 0) via raw pread. */
        uint8_t nbuf[8];
        if (pread(sb->fd, nbuf, 8, payload_off) != 8) break;
        memcpy(&cur, nbuf, 8);
        page_walked++;
    }
    int64_t stored_global = vfs_atomic_load_i64(fl_count_ptr);
    if (total_valid != stored_global) {
        vfs_atomic_store_i64(fl_count_ptr, total_valid);
        /* Update on-disk header: rewrite the count and refresh
           the CRC.  Read the entire header payload, modify the
           count, recompute CRC, write CRC back to the PageHeader. */
        uint8_t* payload = malloc((size_t)sb->page_size);
        if (payload) {
            if (pread(sb->fd, payload, (size_t)sb->page_size,
                      PAGE_HEADER_SIZE) == sb->page_size) {
                /* Rewrite the count in the payload. */
                vfs_wr8_s(payload, HDR_OFF_FREE_LIST_COUNT, total_valid, sb->page_size);
                /* Recompute and write the new CRC. */
                uint32_t new_crc = vfs_crc32c(payload, (size_t)sb->page_size);
                PageHeader ph;
                if (pread(sb->fd, &ph, PAGE_HEADER_SIZE, 0) == PAGE_HEADER_SIZE) {
                    ph.checksum = new_crc;
                    pwrite(sb->fd, &ph, PAGE_HEADER_SIZE, 0);
                }
            }
            free(payload);
        }
        fprintf(stderr,
            "free-list validation: global count %lld -> %lld\n",
            (long long)stored_global, (long long)total_valid);
    }
}

StorageBackend* storage_open(const char* path, int64_t page_size) {
    StorageBackend* sb = calloc(1, sizeof(StorageBackend));
    if (!sb) return NULL;

    sb->page_size  = page_size > 0 ? page_size : 8192;
    sb->fd         = -1;

    /* Try to open existing file */
    sb->fd = open(path, O_RDWR);
    if (sb->fd >= 0) {
        /* Existing file — mount */
        if (mount_existing(sb) != 0) {
            close(sb->fd);
            free(sb);
            return NULL;
        }
    } else {
        /* Create new file */
        sb->fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (sb->fd < 0) {
            free(sb);
            return NULL;
        }
        if (bootstrap_new(sb) != 0) {
            close(sb->fd);
            unlink(path);
            free(sb->header_buf);
            free(sb);
            return NULL;
        }
    }

    /* Initialize indirection table */
    indir_init(sb);

    /* Initialize mirror tracking arrays */

    /* Initialize page cache */
    cache_init(&sb->cache, sb, sb->page_size);

    /* Phase 27 W5: validate the free-list count after mount.  Must
       run after indir_init (need indir_lookup for free-list pages). */
    validate_free_list_on_mount(sb);

    return sb;
}

void storage_close(StorageBackend* sb) {
    if (!sb) return;

    /* Flush all dirty pages */
    storage_flush(sb, -1);

    /* Destroy cache */
    cache_destroy(&sb->cache);

    /* Free indirection overflow pages */
    for (int i = 0; i < sb->indir.overflow_count; i++) {
        free(sb->indir.overflow_pages[i]);
    }
    free(sb->indir.overflow_pages);
    free(sb->indir.overflow_logical);

    /* Free mirror arrays */

    /* Free header buffer */
    free(sb->header_buf);

    /* Close fd */
    if (sb->fd >= 0) close(sb->fd);

    free(sb);
}

/* ---------------------------------------------------------------------------
 * Allocate / Acquire / Free  (public, thread-safe)
 * --------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * Allocate / Acquire / Free  (public, thread-safe, lock-free)
 * --------------------------------------------------------------------------- */

/* Attempt to CAS-set an indirection entry from 0 to physical_offset.
   Returns 1 on success, 0 if the entry was already claimed. */
static int try_claim_entry(StorageBackend* sb, int64_t logical_page, int64_t physical_offset) {
    int64_t existing = indir_lookup(sb, logical_page);
    if (existing != 0) {
        return 0;  /* fast path: already taken (self-ref, prior claim, etc.) */
    }

    /* The entry is in the header buffer (inline) or overflow page buffer.
       Both are plain int64_t arrays.  CAS on the entry directly. */
    IndirectionTable* it = &sb->indir;
    int64_t* entry_ptr;

    if (logical_page < it->inline_count) {
        entry_ptr = &it->inline_entries[logical_page];
    } else {
        int64_t remaining = logical_page - it->inline_count;
        int64_t overflow_idx = remaining / it->entries_per_overflow;
        int64_t entry_idx    = remaining % it->entries_per_overflow;
        if (overflow_idx >= it->overflow_count) return 0;
        entry_ptr = &it->overflow_pages[overflow_idx][1 + entry_idx];
    }

    int64_t expected = 0;
    return (vfs_cas_i64(entry_ptr, expected, physical_offset) == expected);
}

/* Forward declaration for the count>1 fallback below. */
static int64_t storage_allocate_count_scan(StorageBackend* sb, int count);

/* Phase 27: storage_allocate_tail_advance — extracted from the
   original storage_allocate(1) tail-advance code.  Used by:
     - storage_allocate(1)  (the public API — the hot path)
     - enqueue_free_page()  (W2 — to allocate free-list metadata
       pages when the tail is full; bypasses the free-list check
       to avoid the producer-consumer cycle described in the spec's
       R6 fix)
   The function returns the new logical page index, or -1 on error.
   The caller is responsible for claiming the slot via try_claim_entry
   if it wants to use the page for data. */
static int64_t storage_allocate_tail_advance(StorageBackend* sb, int count);

/* Phase 27 W6: dequeue_from_free_list — try to pop a (logical,
   physical) entry from the free-page queue.  Returns the logical
   page on success, or 0 if the queue is empty / a race lost.

   Thread-safety model (W6):
     - Multiple FUSE worker threads can call dequeue concurrently.
     - The enqueue side (GC) is single-threaded (per the W5+ GC
       redesign: a single background thread does enqueues).
     - Cross-thread races between enqueue and dequeue are benign
       (the enqueue's "append + increment count" can race with
       the dequeue's "pop + decrement count"; the dequeue's CAS
       catches the count change and retries).

   Concurrency primitives (per the spec's R2 design):
     - Per-page CAS on the head's count field (decrement).
       Catches concurrent dequeues from the same head page —
       the loser of the CAS race re-reads count and retries.
     - CAS on the global head pointer (advance past a drained
       head page).  Catches concurrent "head is empty, advance
       to next" races — the loser retries with the new head.
     - try_claim_entry (CAS on indir) is the final ABA guard.
       If the popped page was already claimed by another thread
       (the indir CAS fails), the dequeue returns 0 and the
       caller tail-advances past the already-claimed VP.

   The dequeue is wrapped in a retry loop.  Any CAS failure
   (count or head) restarts the dequeue from the top.  The
   retry count is bounded (1000) to prevent livelock in the
   pathological case of constant contention. */

/* Forward declarations for the free-list helpers (defined below
   alongside enqueue_free_page). */
static int     read_free_list_count(StorageBackend* sb, int64_t fl_page);
static int64_t read_free_list_next(StorageBackend* sb, int64_t fl_page);
static void    write_free_list_count(StorageBackend* sb, int64_t fl_page, int count);
static int64_t read_free_list_entry(StorageBackend* sb, int64_t fl_page, int idx,
                                     int64_t* out_phys);

/* Forward declaration for the deferred-free check (defined in gc.c).
   Used by the dequeue to honor GC's deferred-free queue. */
struct DeferredFreeQueue;
bool deferred_free_is_queued(struct DeferredFreeQueue* queue, int64_t logical_page);
extern struct DeferredFreeQueue* _deferred_queue;

static int64_t dequeue_from_free_list(StorageBackend* sb) {
    int64_t* fl_head_ptr = (int64_t*)(sb->header_buf + HDR_OFF_FREE_LIST_HEAD);
    int64_t* fl_count_ptr = (int64_t*)(sb->header_buf + HDR_OFF_FREE_LIST_COUNT);

    /* Phase 27 W6: retry loop for thread-safety.  Any CAS failure
       (count or head) restarts the dequeue from the top.  The
       retry count is bounded to prevent livelock. */
    for (int retry = 0; retry < 1000; retry++) {
        int64_t count = vfs_atomic_load_i64(fl_count_ptr);
        if (count <= 0) return 0;

        int64_t head = vfs_atomic_load_i64(fl_head_ptr);
        if (head == 0) return 0;

        /* Read the head's per-page count.  If the page is in the
           cache, the count is the latest cached value (the cache's
           bucket lock synchronizes the read with concurrent writes
           via storage_read_with_status). */
        int head_count = read_free_list_count(sb, head);
        if (head_count < 0) return 0;  /* I/O error */
        if (head_count == 0) {
            /* Head page is empty.  Advance to head->next and free
               the drained page (R2: indir_set(head, 0)).  Use CAS
               on the head pointer so concurrent advances don't
               clobber each other.  If we lose the CAS, another
               thread already advanced — retry from the new head. */
            int64_t next = read_free_list_next(sb, head);
            if (vfs_cas_i64(fl_head_ptr, head, next) == head) {
                indir_set(sb, head, 0);
            }
            /* else: lost the CAS.  Another thread advanced the
               head and freed the old head.  Retry to read the
               new head's count. */
            continue;
        }

        /* Pop entry[count-1] (LIFO within page). */
        int pop_idx = head_count - 1;
        int64_t phys = 0;
        int64_t page = read_free_list_entry(sb, head, pop_idx, &phys);
        if (page == 0) return 0;

        /* CAS-decrement the per-page count.  If the CAS fails,
           another thread popped first — retry from the top
           (the new count might mean a different entry, or the
           head might have been drained entirely). */
        StorageReadStatus st;
        uint8_t* payload = storage_read_with_status(sb, head, &st);
        if (!payload || st != STORAGE_OK) {
            /* I/O error.  Invalidate any cached entry for the
               popped page (we may have just read a stale entry)
               and bail. */
            return 0;
        }
        int32_t* count_ptr = (int32_t*)(payload + 8);  /* count at payload+8 */
        int32_t expected = (int32_t)head_count;
        if (vfs_cas_i32(count_ptr, expected, expected - 1) != expected) {
            /* Lost the count CAS.  Another thread popped or
               advanced the head.  Retry from the top. */
            continue;
        }

        /* We won the count CAS.  The entry is ours. */
        /* If head_count was 1, the head is now empty.  Advance
           to next and free the old head.  Use CAS on the head
           pointer — if we lose, another thread already advanced
           and freed the old head.  Either way, the old head is
           freed exactly once. */
        if (head_count == 1) {
            int64_t next = read_free_list_next(sb, head);
            if (vfs_cas_i64(fl_head_ptr, head, next) == head) {
                indir_set(sb, head, 0);
            }
            /* else: another thread won.  They freed the old head. */
        }

        /* Invalidate the cache entry for the popped page.  The
           page is now "allocated" (we'll try_claim_entry below)
           so any cached payload is stale.  The next read will
           reload from disk (which has the post-enqueue content)
           — wait, that's wrong.  The page was freed (indir=0)
           and enqueued (entry added).  The on-disk content is
           whatever was last written (the data before free).
           We don't want to return that as "fresh" data.  But
           the caller will overwrite it (storage_allocate returns
           to pool_alloc, which returns to vfs_create, which
           writes the new file's metadata).  So the stale data
           is fine — it'll be overwritten before anyone reads
           it.  The cache_invalidate just makes sure no one
           reads the stale data from the cache. */
        cache_invalidate(&sb->cache, page);

        /* Decrement global count.  Atomic add — multiple deques
           can decrement concurrently.  The per-page count is the
           authoritative count (via CAS above); the global count
           is a fast-path optimization (the dequeue's first
           `count <= 0` check).  They stay in sync because every
           successful dequeue decrements both. */
        vfs_atomic_add_i64(fl_count_ptr, -1);

        /* try_claim_entry: ABA detection.  If another thread
           already claimed this page (indir != 0), the CAS fails
           and we return 0 (the caller will tail-advance past
           the already-claimed VP).  Note: this is a separate
           guard from the count CAS — it catches the case where
           the page was enqueued, then re-allocated by another
           path (e.g., a tail-advance raced with the free-list
           and the page was given out twice). */
        if (!try_claim_entry(sb, page, phys)) {
            return 0;  /* ABA: caller tail-advances */
        }

        /* H3 fix: honor the deferred-free queue.  If GC is running
           and the dequeued page is in GC's deferred queue, the
           page is still "in flight" (an in-flight reader might
           have a pointer to it).  Don't return it.  The caller
           will tail-advance past this VP. */
        if (_deferred_queue && deferred_free_is_queued(_deferred_queue, page)) {
            return 0;  /* in deferred queue; caller tail-advances */
        }

        return page;
    }
    /* Safety: gave up after 1000 retries (extreme contention).
       Treat as queue-empty; caller tail-advances. */
    return 0;
}

int64_t storage_allocate(StorageBackend* sb, int count) {
    if (count <= 0) return -1;
    if (count > 1) {
        /* Multi-page allocations aren't currently exercised by the hot
           path (only count=1 is, from pool_alloc / mirror_write).  Fall
           back to the original scan-and-CAS logic via indir_lookup +
           free-page search, which is rare.  TODO: replace this fallback
           with the same tail-advance pattern once count>1 is needed. */
        return storage_allocate_count_scan(sb, count);
    }
    /* Phase 27 W3: try the free-list first.  If the queue is empty
       (the common case — no GC = no frees), the load is in L1 and
       the fall-through to tail-advance is immediate. */
    int64_t page = dequeue_from_free_list(sb);
    if (page > 0) return page;
    return storage_allocate_tail_advance(sb, count);
}

/* storage_allocate_tail_advance — see comment above.
   The body is the original count==1 tail-advance fast path. */
static int64_t storage_allocate_tail_advance(StorageBackend* sb, int count) {
    /* Tail-advance fast path.  Assumption: every newly allocated page
       gets the next available index at the tail of the indirection
       table, so the first free slot is exactly sb->total_pages.  This
       holds as long as storage_free is never called during normal
       operation — and currently it isn't (only GC calls it).

       TODO: when GC reclaims mid-table slots, switch to a free-list
       allocation policy.  Until then, this works because there are
       no holes in [2, sb->total_pages) during the file-create/write
       hot path. */

    /* Ensure the indirection table has room for the new page. */
    if (indir_ensure_capacity(sb, count) != 0) return -1;

    /* Allocate the next physical slot from physical_tail (CAS). */
    int64_t old_tail = sb->physical_tail;
    int64_t new_tail = old_tail + phys_record_size(sb);
    while (vfs_cas_i64(&sb->physical_tail, old_tail, new_tail) != old_tail) {
        old_tail = sb->physical_tail;
        new_tail = old_tail + phys_record_size(sb);
    }

    /* Atomically claim the slot at sb->total_pages.  The do-while
       handles three cases:
         1. Normal: CAS-bump sb->total_pages, claim the slot.
         2. Self-ref (entry is non-zero = the overflow's physical).
            Just loop back — the next bump lands on a data slot.
         3. OOB (sb->total_pages is beyond the current table).
            The pre-check catches the common case (grow before
            CAS-bump, so the OOB slot is claimable on the next
            iteration).  But the pre-check is racy: between the
            read of overflow_count and the CAS, another thread
            can add an overflow, making the pre-check see a
            stale (too-large) overflow_count and miss the OOB.
            In that case the CAS bumps past the OOB slot.  The
            post-check below catches this: if try_claim_entry
            fails AND the entry is OOB, we grow the table and
            retry the claim with the SAME logical (the OOB
            data slot that became valid after the grow).  sb->
            total_pages is now logical + 1, but we claim logical
            itself.  If another thread already claimed it, we
            loop back and the next iteration's pre-check sees
            the updated state. */
    int64_t logical;
    int     oob_retries = 0;
    do {
        /* Pre-check: determine if we need to grow the table
           BEFORE the CAS-bump.  Three needs_grow cases:
             a) sb->total_pages is OOB (beyond the current table)
             b) sb->total_pages is the inline self-ref (inline_count-1)
                and its entry is 0 (overflow[0] not allocated)
             c) sb->total_pages is a self-ref in a previous overflow's
                last entry, and that entry is 0 (the next overflow
                is not allocated)
           Case (c) is the dangerous one: without the pre-check,
           try_claim_entry would see existing=0, CAS the entry to
           a data page's physical, and corrupt the indirection
           table.  The post-check below would catch it, but only
           after the corruption already happened.

           The pre-check reads overflow_count and
           overflow_pages[overflow_idx][1+eidx] non-atomically.
           Under contention, A can read overflow_count stale
           (e.g., K when B has incremented to K+1) but see B's
           self-ref write (overflow_pages[K-1][1+14] = non-zero).
           A then thinks overflow[K] is added but it's not, and
           try_claim_entry reads uninitialized memory.

           To close this race, we hold overflow_lock for just the
           pre-check (a few memory reads).  The lock is released
           before the inner CAS, so the CAS itself is lock-free.
           The lock is held for nanoseconds — contention is
           minimal.  indir_ensure_capacity (which also takes the
           lock, for longer) is rare, so the pre-check rarely
           waits on it. */
        {
            IndirectionTable* it = &sb->indir;
            int needs_grow = 0;
            /* Acquire the lock for the pre-check only.  Released
               after the needs_grow decision below. */
            while (__sync_lock_test_and_set(&it->overflow_lock, 1)) { /* spin */ }
            int64_t tp = sb->total_pages;
            if (tp < 2) tp = 2;
            if (tp < it->inline_count) {
                if (tp == it->inline_count - 1 &&
                    it->inline_entries[tp] == 0) {
                    needs_grow = 1;
                }
            } else {
                int64_t remaining   = tp - it->inline_count;
                int64_t overflow_idx = remaining / it->entries_per_overflow;
                int64_t eidx        = remaining % it->entries_per_overflow;
                if (overflow_idx >= it->overflow_count) {
                    needs_grow = 1;
                } else if (eidx == it->entries_per_overflow - 1 &&
                           it->overflow_pages[overflow_idx][1 + eidx] == 0) {
                    needs_grow = 1;
                }
            }
            __sync_lock_release(&it->overflow_lock);
            if (needs_grow) {
                if (indir_ensure_capacity(sb, 1) != 0) return -1;
                continue;  /* re-check from the top */
            }
        }

        int64_t old_total;
        do {
            old_total = sb->total_pages;
            if (old_total < 2) old_total = 2;  /* page 0 = header, 1 = superblock */
            logical = old_total;
        } while (vfs_cas_i64(&sb->total_pages, old_total, old_total + 1) != old_total);

        if (try_claim_entry(sb, logical, old_tail)) break;

        /* Claim failed.  Three sub-cases:
           1. Self-ref in previous overflow whose next overflow
              is already allocated (entry is non-zero = the
              overflow's physical).  Just loop back — the next
              bump lands on a data slot.
           2. Self-ref in previous overflow whose next overflow
              is NOT yet allocated (entry is 0).  This is the
              dangerous case: try_claim_entry would CAS the
              entry to a data page's physical, corrupting the
              indirection table.  Grow the table and retry the
              claim with the same logical.
           3. OOB (logical is beyond the current table).  Same
              fix: grow the table and retry with the same logical.

           The pre-check catches the common case for (3) but
           can miss it under contention (stale overflow_count
           read).  The post-check below is the safety net for
           both (2) and (3). */
        {
            IndirectionTable* it = &sb->indir;
            int needs_grow = 0;
            if (logical < it->inline_count) {
                /* Inline self-ref (only inline[inline_count-1]). */
                if (logical == it->inline_count - 1 &&
                    it->inline_entries[logical] == 0) {
                    needs_grow = 1;
                }
            } else {
                int64_t remaining = logical - it->inline_count;
                int64_t overflow_idx = remaining / it->entries_per_overflow;
                int64_t eidx = remaining % it->entries_per_overflow;
                if (overflow_idx >= it->overflow_count) {
                    /* OOB: logical is beyond the current table. */
                    needs_grow = 1;
                } else if (eidx == it->entries_per_overflow - 1) {
                    /* Last entry of overflow[overflow_idx] =
                       self-ref of overflow[overflow_idx+1].
                       If entry is 0, the next overflow is not
                       yet allocated. */
                    if (it->overflow_pages[overflow_idx][1 + eidx] == 0) {
                        needs_grow = 1;
                    }
                }
                /* Otherwise: normal data slot, entry is non-zero
                   (already claimed by another thread) — CAS lost
                   the race, just loop back. */
            }
            if (needs_grow) {
                if (++oob_retries > 8) return -1;
                if (indir_ensure_capacity(sb, 1) != 0) return -1;
                /* Retry the claim with the same logical.  After
                   the grow, the OOB/self-ref slot is now a
                   valid entry.  If another thread claimed it,
                   try_claim_entry returns 0 and we loop back. */
                if (try_claim_entry(sb, logical, old_tail)) break;
            }
        }
        /* Normal self-ref, CAS-lost, or post-grow-retry-lost:
           loop back. */
    } while (1);

    return logical;
}  /* end of storage_allocate_tail_advance */

/* ---------------------------------------------------------------------------
 * storage_allocate_count_scan — slow fallback for count > 1.
 *
 * Original scan-and-CAS allocation: walk from i=2 to total_entries, find
 * `count` consecutive free entries, CAS-claim them.  Kept as the rare
 * path; the hot path goes through storage_allocate's tail-advance.
 * --------------------------------------------------------------------------- */

static int64_t storage_allocate_count_scan(StorageBackend* sb, int count) {
    int64_t total_entries = (int64_t)sb->indir.inline_count +
                            (int64_t)sb->indir.overflow_count * sb->indir.entries_per_overflow;

    for (int attempt = 0; attempt < 1000; attempt++) {
        int64_t run_start = -1;
        int     run_len   = 0;

        for (int64_t i = 2; i < total_entries && run_len < count; i++) {
            if (indir_lookup(sb, i) == 0) {
                /* Skip pages queued for deferred free — they may still be
                   referenced by in-flight readers. */
                if (_deferred_queue && deferred_free_is_queued(_deferred_queue, i)) {
                    run_start = -1;
                    run_len   = 0;
                    continue;
                }
                if (run_len == 0) run_start = i;
                run_len++;
            } else {
                run_start = -1;
                run_len   = 0;
            }
        }

        if (run_len < count) return -1;  /* not enough free entries */

        /* Try to CAS-claim each entry in the run */
        int ok = 1;
        for (int j = 0; j < count; j++) {
            int64_t logical = run_start + j;

            /* CAS advance physical_tail */
            int64_t old_tail = sb->physical_tail;
            int64_t new_tail = old_tail + phys_record_size(sb);
            while (vfs_cas_i64(&sb->physical_tail, old_tail, new_tail) != old_tail) {
                old_tail = sb->physical_tail;
                new_tail = old_tail + phys_record_size(sb);
            }

            /* Try to CAS-set the indirection entry from 0 → old_tail */
            if (!try_claim_entry(sb, logical, old_tail)) {
                /* Another thread claimed it.  The physical slot we reserved is
                   a zombie — wasted but harmless (GC reclaims it).  Restart. */
                ok = 0;
                break;
            }

        }

        if (ok) {
            /* Update total_pages if needed */
            int64_t new_total = run_start + count;
            int64_t old_total;
            do {
                old_total = sb->total_pages;
                if (new_total <= old_total) break;
            } while (vfs_cas_i64(&sb->total_pages, old_total, new_total) != old_total);

            return run_start;
        }
        /* Collision — retry scan */
    }

    return -1;  /* too many retries */
}

int storage_acquire(StorageBackend* sb, int64_t logical_page) {
    if (logical_page < 0) return 0;

    /* Ensure mirror arrays are big enough */

    /* Ensure indirection has room for this page */
    int64_t total_entries = (int64_t)sb->indir.inline_count +
                            (int64_t)sb->indir.overflow_count * sb->indir.entries_per_overflow;
    if (logical_page >= total_entries) {
        int needed = (int)(logical_page - total_entries + 1);
        if (indir_ensure_capacity(sb, needed) != 0) return 0;
    }

    /* Fast check: if already allocated, return false */
    if (indir_lookup(sb, logical_page) != 0) return 0;

    /* CAS advance physical_tail */
    int64_t old_tail = sb->physical_tail;
    int64_t new_tail = old_tail + phys_record_size(sb);
    while (vfs_cas_i64(&sb->physical_tail, old_tail, new_tail) != old_tail) {
        old_tail = sb->physical_tail;
        new_tail = old_tail + phys_record_size(sb);
    }

    /* CAS-set the entry from 0 to old_tail.
       try_claim_entry does atomic CAS, not plain store.
       If CAS fails (another thread claimed it), the physical slot is a zombie. */
    if (!try_claim_entry(sb, logical_page, old_tail)) {
        return 0;  /* another thread claimed it */
    }

    /* Update total_pages */
    int64_t new_total = logical_page + 1;
    int64_t old_total;
    do {
        old_total = sb->total_pages;
        if (new_total <= old_total) break;
    } while (vfs_cas_i64(&sb->total_pages, old_total, new_total) != old_total);

    return 1;
}

/* ---------------------------------------------------------------------------
 * Phase 27 free-page queue: enqueue / dequeue helpers
 * --------------------------------------------------------------------------- */

/* Free-list page format (per-page_size bytes, when interpreted as
   a free-list metadata page):
     offset 0:  8 bytes  next_page  (logical VP of next free-list page; 0 = end)
     offset 8:  4 bytes  count      (number of valid entries; 0..MAX_PER_PAGE)
     offset 12: 4 bytes  padding    (8-byte alignment for the entry array)
     offset 16: 16*N bytes  entries[N]  (each: 8-byte logical + 8-byte physical)

   MAX_PER_PAGE = (page_size - 16) / 16.
*/

/* The MAX_PER_PAGE constant depends on page_size; this helper gives
   us the right value at runtime. */
static int free_list_max_per_page(StorageBackend* sb) {
    return (int)((sb->page_size - 16) / 16);
}

/* Read the count field of a free-list page (via the cache). */
static int read_free_list_count(StorageBackend* sb, int64_t fl_page) {
    StorageReadStatus st;
    uint8_t* payload = storage_read_with_status(sb, fl_page, &st);
    if (!payload || st != STORAGE_OK) return -1;
    int32_t count = (int32_t)vfs_rd4_s(payload, 8, sb->page_size);
    return (int)count;
}

/* Read the next_page field of a free-list page (via the cache). */
static int64_t read_free_list_next(StorageBackend* sb, int64_t fl_page) {
    StorageReadStatus st;
    uint8_t* payload = storage_read_with_status(sb, fl_page, &st);
    if (!payload || st != STORAGE_OK) return 0;
    return vfs_rd8_s(payload, 0, sb->page_size);
}

/* Write the next_page field of a free-list page (via the cache). */
static void write_free_list_next(StorageBackend* sb, int64_t fl_page, int64_t next) {
    StorageReadStatus st;
    uint8_t* payload = storage_read_with_status(sb, fl_page, &st);
    if (!payload || st != STORAGE_OK) return;
    vfs_wr8_s(payload, 0, next, sb->page_size);  /* next_page at offset 0 */
    cache_mark_dirty(&sb->cache, fl_page, FLUSH_PRIO_POOL);
}

/* Write the count field of a free-list page (via the cache). */
static void write_free_list_count(StorageBackend* sb, int64_t fl_page, int count) {
    StorageReadStatus st;
    uint8_t* payload = storage_read_with_status(sb, fl_page, &st);
    if (!payload || st != STORAGE_OK) return;
    vfs_wr4_s(payload, 8, (int32_t)count, sb->page_size);
    cache_mark_dirty(&sb->cache, fl_page, FLUSH_PRIO_POOL);
}

/* Read an entry at index `idx` in the free-list page.  Returns
   logical VP and physical offset.  Returns 0 on failure. */
static int64_t read_free_list_entry(StorageBackend* sb, int64_t fl_page, int idx,
                                     int64_t* out_phys) {
    StorageReadStatus st;
    uint8_t* payload = storage_read_with_status(sb, fl_page, &st);
    if (!payload || st != STORAGE_OK) return 0;
    int64_t logical = vfs_rd8_s(payload, 16 + (int64_t)idx * 16,     sb->page_size);
    int64_t phys     = vfs_rd8_s(payload, 16 + (int64_t)idx * 16 + 8, sb->page_size);
    *out_phys = phys;
    return logical;
}

/* Write an entry at index `idx` in the free-list page. */
static void write_free_list_entry(StorageBackend* sb, int64_t fl_page, int idx,
                                    int64_t logical, int64_t phys) {
    StorageReadStatus st;
    uint8_t* payload = storage_read_with_status(sb, fl_page, &st);
    if (!payload || st != STORAGE_OK) return;
    vfs_wr8_s(payload, 16 + (int64_t)idx * 16,     logical, sb->page_size);
    vfs_wr8_s(payload, 16 + (int64_t)idx * 16 + 8, phys,    sb->page_size);
    cache_mark_dirty(&sb->cache, fl_page, FLUSH_PRIO_POOL);
}

/* Allocate a new free-list page via the tail-advance path.  The
   new page's count is initialized to 1 and entry[0] = (logical, phys).
   Returns the new page's logical VP, or 0 on error.

   The new free-list page is brand-new — its on-disk content is
   uninitialized, so a `storage_read_with_status` call would fail
   the CRC check and return NULL.  We pre-populate the cache with
   a fresh zero-initialized buffer (dirty) so the subsequent
   `write_free_list_*` calls can find the page in the cache and
   write directly to the in-memory payload.  The buffer is
   flushed to disk on the next `cache_flush_all`.  This is safe
   because the cache_flush_all refactor (W5) makes the flush
   re-entrant — mirror_write can recurse into the cache without
   deadlocking. */
static int64_t alloc_free_list_page(StorageBackend* sb, int64_t logical, int64_t phys) {
    int64_t new_vp = storage_allocate_tail_advance(sb, 1);
    if (new_vp < 0) return 0;

    /* Pre-populate the cache with a fresh zero buffer.  The cache
       owns its own copy (dirty=1 path: malloc + memcpy, caller frees). */
    uint8_t* fresh = calloc(1, (size_t)sb->page_size);
    if (!fresh) return 0;
    cache_insert(&sb->cache, new_vp, fresh, FLUSH_PRIO_POOL, 1);
    free(fresh);

    /* Now the writes will find the cached entry and write to it. */
    write_free_list_next(sb, new_vp, 0);  /* no next */
    write_free_list_count(sb, new_vp, 1);
    write_free_list_entry(sb, new_vp, 0, logical, phys);
    return new_vp;
}

/* Append (logical, phys) to the free-page queue.  Allocates a new
   free-list page if the tail is null or full.  This is W2's enqueue.
   Note: not thread-safe yet (single-threaded assumption for the W2
   initial impl; the spec acknowledges storage_free is currently only
   called by GC, which is single-threaded). */
static void enqueue_free_page(StorageBackend* sb, int64_t logical, int64_t phys) {
    int max_per_page = free_list_max_per_page(sb);

    /* Read current tail. */
    int64_t* fl_head_ptr = (int64_t*)(sb->header_buf + HDR_OFF_FREE_LIST_HEAD);
    int64_t* fl_tail_ptr = (int64_t*)(sb->header_buf + HDR_OFF_FREE_LIST_TAIL);
    int64_t* fl_count_ptr = (int64_t*)(sb->header_buf + HDR_OFF_FREE_LIST_COUNT);

    int64_t tail = vfs_atomic_load_i64(fl_tail_ptr);
    int tail_count = (tail != 0) ? read_free_list_count(sb, tail) : 0;

    int64_t new_tail;
    if (tail == 0 || tail_count < 0 || tail_count >= max_per_page) {
        /* Empty queue or tail is full.  Allocate a new free-list page. */
        new_tail = alloc_free_list_page(sb, logical, phys);
        if (new_tail == 0) return;  /* OOM; the logical free still happened
                                       (indir=0 was set by storage_free) */

        if (tail != 0) {
            /* Queue was non-empty; link the new page as the next after tail. */
            write_free_list_next(sb, tail, new_tail);
        }
    } else {
        /* Append to existing tail. */
        write_free_list_entry(sb, tail, tail_count, logical, phys);
        write_free_list_count(sb, tail, tail_count + 1);
        new_tail = tail;
    }

    /* Update tail and head pointers + global count. */
    vfs_atomic_store_i64(fl_tail_ptr, new_tail);
    if (vfs_atomic_load_i64(fl_head_ptr) == 0) {
        vfs_atomic_store_i64(fl_head_ptr, new_tail);
    }
    vfs_atomic_add_i64(fl_count_ptr, 1);
}

void storage_free(StorageBackend* sb, int64_t logical_page) {
    if (logical_page < 2) return;  /* don't free header or superblock */
    int64_t phys = indir_lookup(sb, logical_page);
    if (phys == 0) return;  /* already free */
    indir_set(sb, logical_page, 0);
    /* Invalidate the cache entry — the data is now stale (the page
       is free; the new indir=0 means reads should return NOT_FOUND). */
    cache_invalidate(&sb->cache, logical_page);
    /* Phase 27: enqueue (logical, phys) into the free-page queue. */
    enqueue_free_page(sb, logical_page, phys);
}

/* ---------------------------------------------------------------------------
 * Read / Write / Flush  (public — goes through cache + lazy mirror)
 * --------------------------------------------------------------------------- */

uint8_t* storage_read_with_status(StorageBackend* sb, int64_t logical_page,
                                  StorageReadStatus* out_status) {
    if (out_status) *out_status = STORAGE_NOT_FOUND;
    _cache_total++;

    /* 1. Check page cache — a cached entry is by definition valid
       (it was validated when first read in; flushes re-validate). */
    CacheEntry* ce = cache_find(&sb->cache, logical_page);
    _last_was_hit = (ce != NULL);
    if (ce) { _cache_hits++; if (out_status) *out_status = STORAGE_OK; return ce->payload; }

    /* 2. Check if page is allocated */
    int64_t offset = indir_lookup(sb, logical_page);
    if (offset == 0) return NULL;  /* not allocated → STORAGE_NOT_FOUND */

    /* 3. Read from disk via lazy mirror (with status).  Phase 27 C5:
       a CRC mismatch is now distinguished from an I/O error.  We must
       NOT zero-fill or insert garbage into the cache. */
    uint8_t* buf = malloc((size_t)sb->page_size);
    if (!buf) {
        if (out_status) *out_status = STORAGE_IO_ERROR;
        return NULL;
    }

    int mirror_status = mirror_read_with_status(sb, logical_page, buf);
    if (mirror_status != 0) {
        free(buf);
        if (out_status) {
            *out_status = (mirror_status == -2) ? STORAGE_CRC_ERROR : STORAGE_IO_ERROR;
        }
        return NULL;
    }

    /* 4. Insert into cache — cache takes ownership of buf (no copy) */
    cache_insert(&sb->cache, logical_page, buf, 0, 0);

    /* 5. Return the cached payload (cache owns buf now) */
    if (out_status) *out_status = STORAGE_OK;
    ce = cache_find(&sb->cache, logical_page);
    return ce ? ce->payload : NULL;
}

void storage_write(StorageBackend* sb, int64_t logical_page, const uint8_t* payload,
                   uint32_t priority) {
    /* Cache-only update.  Disk persistence happens in cache_flush_all, which
       invokes the lazy mirror logic per dirty page (see SPEC §3.7).  This
       makes Write() pure CPU: it copies the payload into the cache, marks
       the page dirty, and returns.  No disk I/O is performed here.

       Lazy mirror is a property of the ON-DISK page state, not the cache
       entry.  The cache holds a payload buffer + dirty flag; the
       on-disk page header (generation, mirrorPage) is owned by the storage
       backend and updated by mirror_write inside cache_flush_all. */
    cache_insert(&sb->cache, logical_page, (uint8_t*)payload, (int)priority, 1);
}

void storage_flush(StorageBackend* sb, int64_t logical_page) {
    if (logical_page < 0) {
        /* Flush all cached dirty pages in priority order (0=data first, 3=superblock last) */
        cache_flush_all(sb);

        /* Flush overflow indirection pages (priority 2).
           These are not in the cache — write them directly through lazy mirror. */
        IndirectionTable* it = &sb->indir;
        for (int i = 0; i < it->overflow_count; i++) {
            int64_t logical = it->overflow_logical[i];
            if (logical > 0) {
                mirror_write(sb, logical, (const uint8_t*)it->overflow_pages[i],
                             FLUSH_PRIO_INDIR);
            }
        }

        /* Write the header page LAST via direct pwrite (no lazy mirror).
           Page 0 is special: mount always reads from physical offset 0, and
           lazy mirror would create a sibling at a different offset.  On reopen,
           the original at offset 0 would have stale data (the updated header
           was written to the sibling).  Direct pwrite avoids this.
           Crash safety: data pages are written first, header is the atomic
           commit point.  Crash before header write → old header is still valid.
           Crash after header write → all preceding pages are on disk. */
        vfs_wr8_s(sb->header_buf, HDR_OFF_TOTAL_PAGES,  sb->total_pages, sb->page_size);
        vfs_wr8_s(sb->header_buf, HDR_OFF_PHYS_TAIL,    sb->physical_tail, sb->page_size);
        vfs_wr8_s(sb->header_buf, HDR_OFF_INDIR_HEAD,   sb->indirection_head, sb->page_size);

        uint32_t hdr_crc = vfs_crc32c(sb->header_buf, (size_t)sb->page_size);
        PageHeader ph;
        ssize_t n = pread(sb->fd, &ph, PAGE_HEADER_SIZE, 0);
        if (n == PAGE_HEADER_SIZE) {
            ph.flags     = XVFS_MAGIC;
            ph.checksum  = hdr_crc;
            ph.generation++;
            pwrite(sb->fd, &ph, PAGE_HEADER_SIZE, 0);
            pwrite(sb->fd, sb->header_buf, (size_t)sb->page_size, PAGE_HEADER_SIZE);
        }

        fsync(sb->fd);
    } else {
        cache_flush_page(sb, logical_page);
    }
}

/* Flush dirty cache pages WITHOUT fsync — used for per-file flush
   callbacks where the durability cost of fsync is not warranted.
   fsync is reserved for unmount-time durability (fuse_vfs_destroy
   calls vfs_flush which DOES fsync). */
void storage_flush_cache_only(StorageBackend* sb) {
    if (!sb) return;
    cache_flush_all(sb);
}
