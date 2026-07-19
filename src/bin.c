#include "bin.h"
#include "page_buf.h"
#include "platform.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* ---------------------------------------------------------------------------
 * Phase 28 Bin — implementation (spec: impl/phase-28-gc.md)
 *
 * Concurrency model:
 *   - bin_push: multi-threaded.  Common case (append to existing tail
 *     with room) is lock-free via per-page CAS on count.  Rare case
 *     (tail full or queue empty) takes a short-lived spinlock to
 *     allocate a new Bin page and link it.
 *   - bin_pop:  single-threaded (only the GC thread pops).  Per-page
 *     CAS on count + global CAS on bin_head when advancing past a
 *     drained Bin page.
 *   - bin_peek: single-threaded, lock-free (read-only).
 *
 * Cache interaction:
 *   - Bin pages are read/written via storage_read_with_status
 *     (cache-aware).  All writes go to the cached payload; the page
 *     is marked dirty so the change is flushed by cache_flush_all.
 *   - The Bin page's count is at payload+8 (per the layout in §4.2
 *     of the spec).  The CAS targets the cached payload's count
 *     field directly.
 * --------------------------------------------------------------------------- */

/* Spinlock for the rare "new Bin page" path.  Uncontended in normal
   operation (the Bin tail is rarely full).  Held for nanoseconds
   during the page allocation + linking. */
static volatile int _bin_push_lock = 0;

static inline void bin_push_lock(void) {
    while (__sync_lock_test_and_set(&_bin_push_lock, 1)) { /* spin */ }
}

static inline void bin_push_unlock(void) {
    __sync_lock_release(&_bin_push_lock);
}

/* ---------------------------------------------------------------------------
 * Bin page I/O helpers (cache-aware)
 *
 * All reads go through storage_read_with_status to hit the cache.
 * All writes go to the cached payload + cache_mark_dirty.
 * --------------------------------------------------------------------------- */

/* Read the count field of a Bin page (offset 8 in the payload). */
static int read_bin_page_count(StorageBackend* sb, int64_t bin_page) {
    StorageReadStatus st;
    uint8_t* payload = storage_read_with_status(sb, bin_page, &st);
    if (!payload || st != STORAGE_OK) return -1;
    return (int)vfs_rd4_s(payload, 8, sb->page_size);
}

/* Write the count field of a Bin page. */
static void write_bin_page_count(StorageBackend* sb, int64_t bin_page, int count) {
    StorageReadStatus st;
    uint8_t* payload = storage_read_with_status(sb, bin_page, &st);
    if (!payload || st != STORAGE_OK) return;
    vfs_wr4_s(payload, 8, (int32_t)count, sb->page_size);
    cache_mark_dirty(&sb->cache, bin_page, FLUSH_PRIO_POOL);
}

/* Read an entry at position [idx] in a Bin page. */
static int read_bin_page_entry(StorageBackend* sb, int64_t bin_page, int idx,
                                BinEntry* out) {
    StorageReadStatus st;
    uint8_t* payload = storage_read_with_status(sb, bin_page, &st);
    if (!payload || st != STORAGE_OK) return -1;
    int64_t off = 16 + (int64_t)idx * (int64_t)sizeof(BinEntry);
    out->context  = vfs_rd8_s(payload, off,      sb->page_size);
    out->type     = vfs_rd4_s(payload, off + 8,  sb->page_size);
    out->rsvd     = vfs_rd4_s(payload, off + 12, sb->page_size);
    out->context2 = vfs_rd8_s(payload, off + 16, sb->page_size);
    return 0;
}

/* Write an entry at position [idx] in a Bin page. */
static void write_bin_page_entry(StorageBackend* sb, int64_t bin_page, int idx,
                                  const BinEntry* entry) {
    StorageReadStatus st;
    uint8_t* payload = storage_read_with_status(sb, bin_page, &st);
    if (!payload || st != STORAGE_OK) return;
    int64_t off = 16 + (int64_t)idx * (int64_t)sizeof(BinEntry);
    vfs_wr8_s(payload, off,      entry->context,  sb->page_size);
    vfs_wr4_s(payload, off + 8,  entry->type,     sb->page_size);
    vfs_wr4_s(payload, off + 12, entry->rsvd,     sb->page_size);
    vfs_wr8_s(payload, off + 16, entry->context2, sb->page_size);
    cache_mark_dirty(&sb->cache, bin_page, FLUSH_PRIO_POOL);
}

/* Initialize a freshly-allocated Bin page.  Sets next=0, count=0,
   capacity=K.  The page is pre-populated in the cache by the
   allocator (see alloc_bin_page below), so the payload is in
   memory and ready to write. */
static void init_bin_page(StorageBackend* sb, int64_t bin_page) {
    StorageReadStatus st;
    uint8_t* payload = storage_read_with_status(sb, bin_page, &st);
    if (!payload || st != STORAGE_OK) return;
    vfs_wr8_s(payload, 0, 0, sb->page_size);   /* next_bin_page = 0 */
    vfs_wr4_s(payload, 8, 0, sb->page_size);   /* count = 0 */
    vfs_wr4_s(payload, 12, (int32_t)bin_page_capacity(sb->page_size), sb->page_size);
    cache_mark_dirty(&sb->cache, bin_page, FLUSH_PRIO_POOL);
}

/* Read the next_bin_page field (offset 0). */
static int64_t read_bin_page_next(StorageBackend* sb, int64_t bin_page) {
    StorageReadStatus st;
    uint8_t* payload = storage_read_with_status(sb, bin_page, &st);
    if (!payload || st != STORAGE_OK) return 0;
    return vfs_rd8_s(payload, 0, sb->page_size);
}

/* Write the next_bin_page field. */
static void write_bin_page_next(StorageBackend* sb, int64_t bin_page, int64_t next) {
    StorageReadStatus st;
    uint8_t* payload = storage_read_with_status(sb, bin_page, &st);
    if (!payload || st != STORAGE_OK) return;
    vfs_wr8_s(payload, 0, next, sb->page_size);
    cache_mark_dirty(&sb->cache, bin_page, FLUSH_PRIO_POOL);
}

/* ---------------------------------------------------------------------------
 * Bin page allocation
 *
 * Allocates a new Bin page via storage_allocate_tail_advance (the
 * internal tail-advance-only helper that bypasses the free-list
 * check, avoiding the producer-consumer cycle described in the
 * spec's R6 fix for the free-list).  Pre-populates the cache with
 * a fresh zero-initialized buffer so subsequent reads/writes find
 * the page in the cache.
 * --------------------------------------------------------------------------- */

static int64_t alloc_bin_page(StorageBackend* sb) {
    /* Call the internal helper directly.  Declared in storage.c via
       a forward declaration — but we can't include storage.c, so we
       duplicate the declaration here.  The signature must match
       storage.c::storage_allocate_tail_advance exactly. */
    extern int64_t storage_allocate_tail_advance(StorageBackend* sb, int count);
    int64_t new_vp = storage_allocate_tail_advance(sb, 1);
    if (new_vp < 0) return -1;

    /* Pre-populate the cache with a fresh zero buffer (same pattern
       as alloc_free_list_page in storage.c).  The cache_insert path
       with dirty=1 takes ownership of the malloc'd buffer (or copies
       it).  After this, the page is in the cache and ready to write. */
    uint8_t* fresh = calloc(1, (size_t)sb->page_size);
    if (!fresh) return -1;
    cache_insert(&sb->cache, new_vp, fresh, FLUSH_PRIO_POOL, 1);
    free(fresh);

    return new_vp;
}

/* ---------------------------------------------------------------------------
 * bin_push — the producer side
 *
 * Common case: append to existing tail (lock-free via per-page CAS).
 * Rare case: tail is full or queue is empty (spinlock + new page).
 *
 * The lock-free path retries with the latest count if the CAS loses.
 * If retries exceed BIN_PUSH_MAX_RETRIES, return BIN_ERR_AGAIN.
 * --------------------------------------------------------------------------- */

int bin_push(StorageBackend* sb, int32_t type,
             int64_t context, int64_t context2) {
    if (!sb || !sb->header_buf) return BIN_ERR_IO;
    BinEntry entry = { .type = type, .context = context, .context2 = context2 };

    int64_t* hdr_bin_head = (int64_t*)(sb->header_buf + HDR_OFF_BIN_HEAD);
    int64_t* hdr_bin_count = (int64_t*)(sb->header_buf + HDR_OFF_BIN_COUNT);

    /* Fast path: try to append to the current tail.  Bounded retry
       loop for CAS contention. */
    for (int retry = 0; retry < BIN_PUSH_MAX_RETRIES; retry++) {
        int64_t tail = vfs_atomic_load_i64((int64_t*)(sb->header_buf + HDR_OFF_BIN_TAIL));
        if (tail == 0) break;  /* queue empty — fall through to slow path */

        int count = read_bin_page_count(sb, tail);
        if (count < 0) return BIN_ERR_IO;
        if (count >= bin_page_capacity(sb->page_size)) break;  /* tail full — slow path */

        /* Re-read the cached payload to get a fresh count pointer.
           The payload pointer can change between read_bin_page_count
           and the CAS below (the page might be evicted and re-read
           from disk), so we re-fetch here. */
        StorageReadStatus st;
        uint8_t* payload = storage_read_with_status(sb, tail, &st);
        if (!payload || st != STORAGE_OK) return BIN_ERR_IO;
        int32_t* count_ptr = (int32_t*)(payload + 8);

        int32_t expected = (int32_t)count;
        if (vfs_cas_i32(count_ptr, expected, expected + 1) != expected) {
            continue;  /* CAS lost; retry */
        }

        /* We won the count CAS.  Now write the entry at position [count].
           The payload is still in cache; write to it and mark dirty. */
        int64_t eoff = 16 + (int64_t)expected * (int64_t)sizeof(BinEntry);
        vfs_wr8_s(payload, eoff,      entry.context,  sb->page_size);
        vfs_wr4_s(payload, eoff + 8,  entry.type,     sb->page_size);
        vfs_wr4_s(payload, eoff + 12, 0,              sb->page_size);  /* rsvd */
        vfs_wr8_s(payload, eoff + 16, entry.context2, sb->page_size);
        cache_mark_dirty(&sb->cache, tail, FLUSH_PRIO_POOL);

        /* Increment global count. */
        vfs_atomic_add_i64(hdr_bin_count, 1);
        return BIN_OK;
    }

    /* Slow path: tail is null, full, or count was unattainable.  Take
       the lock to serialize new-page allocation, then use the SAME
       per-page CAS as the fast path to coordinate with concurrent
       fast-path pushes.  This is critical: writing the count without
       CAS races with the fast path and can corrupt the count
       (e.g., count goes past capacity). */
    bin_push_lock();

    int rc = BIN_ERR_AGAIN;
    for (int retry = 0; retry < BIN_PUSH_MAX_RETRIES; retry++) {
        int64_t tail = vfs_atomic_load_i64((int64_t*)(sb->header_buf + HDR_OFF_BIN_TAIL));
        int count = (tail != 0) ? read_bin_page_count(sb, tail) : 0;
        if (count < 0) count = 0;  /* I/O error — treat as full */

        if (tail == 0 || count >= bin_page_capacity(sb->page_size)) {
            /* Need a new Bin page.  Allocate, init, link. */
            int64_t new_tail = alloc_bin_page(sb);
            if (new_tail < 0) {
                rc = BIN_ERR_FULL;
                goto unlock;
            }
            init_bin_page(sb, new_tail);

            if (tail != 0) {
                /* Link the new page as the old tail's next.  Use a
                   plain store: under the lock, no other writer can
                   race. */
                write_bin_page_next(sb, tail, new_tail);
            }

            /* Update bin_tail.  Use store (not CAS): under the lock,
               the only contention is with the GC thread's pop, which
               reads bin_tail but never writes it. */
            vfs_atomic_store_i64((int64_t*)(sb->header_buf + HDR_OFF_BIN_TAIL), new_tail);

            /* If the queue was empty, also set bin_head. */
            if (vfs_atomic_load_i64(hdr_bin_head) == 0) {
                vfs_atomic_store_i64(hdr_bin_head, new_tail);
            }

            tail = new_tail;
            count = 0;
        }

        /* Per-page CAS (same as fast path).  This coordinates with
           concurrent fast-path pushes: if a fast path won the race,
           our CAS fails and we re-read the count (which may have
           advanced past capacity, triggering another new-page
           allocation). */
        StorageReadStatus st;
        uint8_t* payload = storage_read_with_status(sb, tail, &st);
        if (!payload || st != STORAGE_OK) {
            rc = BIN_ERR_IO;
            goto unlock;
        }
        int32_t* count_ptr = (int32_t*)(payload + 8);
        int32_t expected = (int32_t)count;
        if (vfs_cas_i32(count_ptr, expected, expected + 1) != expected) {
            continue;  /* CAS lost; retry */
        }

        /* We won the count CAS.  Write the entry at position [count]. */
        int64_t eoff = 16 + (int64_t)count * (int64_t)sizeof(BinEntry);
        vfs_wr8_s(payload, eoff,      entry.context,  sb->page_size);
        vfs_wr4_s(payload, eoff + 8,  entry.type,     sb->page_size);
        vfs_wr4_s(payload, eoff + 12, 0,              sb->page_size);  /* rsvd */
        vfs_wr8_s(payload, eoff + 16, entry.context2, sb->page_size);
        cache_mark_dirty(&sb->cache, tail, FLUSH_PRIO_POOL);

        vfs_atomic_add_i64(hdr_bin_count, 1);
        rc = BIN_OK;
        goto unlock;
    }
    /* Fell through the retry loop — extreme contention. */
    rc = BIN_ERR_AGAIN;

unlock:
    bin_push_unlock();
    return rc;
}

/* ---------------------------------------------------------------------------
 * bin_pop — the consumer side (single-threaded; only the GC thread pops)
 * --------------------------------------------------------------------------- */

int bin_pop(StorageBackend* sb, BinEntry* out_entry) {
    if (!sb || !sb->header_buf || !out_entry) return BIN_ERR_IO;
    int64_t* hdr_bin_head = (int64_t*)(sb->header_buf + HDR_OFF_BIN_HEAD);
    int64_t* hdr_bin_count = (int64_t*)(sb->header_buf + HDR_OFF_BIN_COUNT);

    /* Bounded retry loop for the "head drained" case: when the
       current head's count reaches 0, we advance to the next Bin
       page and retry.  This handles multi-page Bins transparently
       (the consumer doesn't need to know how many Bin pages there
       are).  The retry count is bounded to prevent livelock in
       the pathological case (Bin is structurally broken — e.g.,
       a page's count is 0 but next_bin_page is also 0, and
       bin_count is non-zero). */
    for (int retry = 0; retry < BIN_PUSH_MAX_RETRIES; retry++) {
        int64_t count = vfs_atomic_load_i64(hdr_bin_count);
        if (count <= 0) return BIN_ERR_EMPTY;

        int64_t head = vfs_atomic_load_i64(hdr_bin_head);
        if (head == 0) return BIN_ERR_EMPTY;

        int head_count = read_bin_page_count(sb, head);
        if (head_count < 0) return BIN_ERR_IO;
        if (head_count == 0) {
            /* Head drained — advance to next and free the old head.
             * Use storage_free (not indir_set) so the page is
             * enqueued into the free-page queue and reused by
             * future allocations, rather than permanently leaked.
             * (Phase 28 W4 review B1 fix.) */
            int64_t next = read_bin_page_next(sb, head);
            if (vfs_cas_i64(hdr_bin_head, head, next) == head) {
                storage_free(sb, head);  /* enqueues drained page for reuse */
            }
            /* Retry with the new head (or 0 if Bin is now empty). */
            continue;
        }

        /* Pop the LAST entry (page-local LIFO for cache locality). */
        int pop_idx = head_count - 1;
        if (read_bin_page_entry(sb, head, pop_idx, out_entry) != 0) {
            return BIN_ERR_IO;
        }

        /* CAS-decrement the per-page count. */
        StorageReadStatus st;
        uint8_t* payload = storage_read_with_status(sb, head, &st);
        if (!payload || st != STORAGE_OK) return BIN_ERR_IO;
        int32_t* count_ptr = (int32_t*)(payload + 8);
        int32_t expected = (int32_t)head_count;
        if (vfs_cas_i32(count_ptr, expected, expected - 1) != expected) {
            /* Race lost — the page was modified under us.  This
               shouldn't happen in a single-threaded consumer, but
               handle it gracefully by retrying.  (If a future
               multi-threaded pop is added, this is where the
               contention surfaces.) */
            continue;
        }

        /* Decrement global count. */
        vfs_atomic_add_i64(hdr_bin_count, -1);

        /* If the head is now empty, advance to next and free the
           old head.  Use storage_free so the page is enqueued
           into the free-page queue (not leaked).  (Phase 28 W4
           review B1 fix.) */
        if (head_count == 1) {
            int64_t next = read_bin_page_next(sb, head);
            if (vfs_cas_i64(hdr_bin_head, head, next) == head) {
                storage_free(sb, head);  /* enqueues drained page for reuse */
            }
        }

        return BIN_OK;
    }
    /* Safety: gave up after BIN_PUSH_MAX_RETRIES retries (Bin
       is structurally broken).  Return EMPTY — the caller (GC
       thread) will sleep and retry later. */
    return BIN_ERR_EMPTY;
}

/* ---------------------------------------------------------------------------
 * bin_peek — read-only, single-threaded
 * --------------------------------------------------------------------------- */

int bin_peek(StorageBackend* sb, BinEntry* out_entry) {
    if (!sb || !sb->header_buf || !out_entry) return BIN_ERR_IO;
    int64_t count = vfs_atomic_load_i64((int64_t*)(sb->header_buf + HDR_OFF_BIN_COUNT));
    if (count <= 0) return BIN_ERR_EMPTY;

    int64_t head = vfs_atomic_load_i64((int64_t*)(sb->header_buf + HDR_OFF_BIN_HEAD));
    if (head == 0) return BIN_ERR_EMPTY;

    int head_count = read_bin_page_count(sb, head);
    if (head_count <= 0) return BIN_ERR_EMPTY;

    return read_bin_page_entry(sb, head, head_count - 1, out_entry);
}

/* ---------------------------------------------------------------------------
 * Mount-time validation walk
 *
 * Called from storage_open after indir_init.  Walks the Bin chain
 * using raw pread (NOT the cache, to avoid putting Bin pages in the
 * cache and triggering the W5-style cache interaction).  Validates
 * each entry's basic fields and rebuilds bin_count if it doesn't
 * match the sum of per-page counts.
 *
 * On a corrupted chain (broken next_bin_page link), truncates the
 * Bin at the last valid page; lost entries are accepted as a
 * recovery cost.
 * --------------------------------------------------------------------------- */

void validate_bin_on_mount(StorageBackend* sb) {
    if (!sb || !sb->header_buf) return;
    int64_t* hdr_bin_head  = (int64_t*)(sb->header_buf + HDR_OFF_BIN_HEAD);
    int64_t* hdr_bin_tail  = (int64_t*)(sb->header_buf + HDR_OFF_BIN_TAIL);
    int64_t* hdr_bin_count = (int64_t*)(sb->header_buf + HDR_OFF_BIN_COUNT);

    int64_t head  = vfs_atomic_load_i64(hdr_bin_head);
    int64_t tail  = vfs_atomic_load_i64(hdr_bin_tail);
    int64_t stored_count = vfs_atomic_load_i64(hdr_bin_count);
    int64_t file_size = lseek(sb->fd, 0, SEEK_END);
    if (file_size < 0) file_size = 0;

    /* Empty queue: head=0, tail=0, count=0 (all consistent). */
    if (head == 0) {
        if (tail != 0 || stored_count != 0) {
            /* Inconsistent empty state — fix it. */
            vfs_atomic_store_i64(hdr_bin_tail, 0);
            vfs_atomic_store_i64(hdr_bin_count, 0);
        }
        return;
    }

    int64_t total_valid = 0;
    int64_t cur = head;
    int64_t last_valid_page = 0;
    int page_walked = 0;
    int chain_broken = 0;
    int cap = bin_page_capacity(sb->page_size);

    while (cur != 0 && page_walked < 10000) {
        int64_t bin_off = indir_lookup(sb, cur);
        if (bin_off == 0) { chain_broken = 1; break; }
        int64_t payload_off = bin_off + PAGE_HEADER_SIZE;

        /* Read the per-page count. */
        uint8_t cbuf[4];
        if (pread(sb->fd, cbuf, 4, payload_off + 8) != 4) { chain_broken = 1; break; }
        int32_t stored_pc;
        memcpy(&stored_pc, cbuf, 4);
        int scan = (stored_pc >= 0 && stored_pc <= cap) ? stored_pc : cap;
        int valid_count = 0;
        for (int i = 0; i < scan; i++) {
            int64_t eoff = payload_off + 16 + (int64_t)i * (int64_t)sizeof(BinEntry);
            /* Read each field via separate pread into distinct local
               buffers.  The compiler may otherwise alias the source
               and destination stack slots, tripping __chk_fail_overflow
               on macOS (the i * 24 stride in particular can confuse
               alias analysis).  Using distinct buffers per field
               sidesteps the aliasing. */
            uint8_t  ctx_raw[8]  __attribute__((aligned(8)));
            uint8_t  ctx2_raw[8] __attribute__((aligned(8)));
            uint8_t  tp_raw[4];
            ssize_t r1 = pread(sb->fd, ctx_raw,  8, eoff);
            ssize_t r2 = pread(sb->fd, tp_raw,   4, eoff + 8);
            ssize_t r3 = pread(sb->fd, ctx2_raw, 8, eoff + 16);
            if (r1 != 8 || r2 != 4 || r3 != 8) { chain_broken = 1; break; }
            /* Use vfs_rd helpers via a fake payload view, but since
               we have raw bytes, decode manually with explicit
               unaligned-safe access.  For little-endian (all
               supported targets) we can just memcpy to the
               destination type.  The 8-byte alignment on ctx_raw
               and ctx2_raw ensures no UB on platforms that
               require aligned int64 loads. */
            int64_t ctx;
            int32_t tp;
            int64_t ctx2;
            memcpy(&ctx,  ctx_raw,  8);
            memcpy(&tp,   tp_raw,   4);
            memcpy(&ctx2, ctx2_raw, 8);
            if (ctx != 0 && (uint32_t)tp < (uint32_t)(BIN_TYPE_WORK_THRESHOLD * 2)) {
                valid_count++;
            }
        }
        if (chain_broken) break;
        total_valid += valid_count;
        last_valid_page = cur;

        /* Read next_bin_page. */
        uint8_t nbuf[8];
        if (pread(sb->fd, nbuf, 8, payload_off) != 8) { chain_broken = 1; break; }
        memcpy(&cur, nbuf, 8);
        page_walked++;
    }

    /* Rebuild if needed. */
    if (total_valid != stored_count || chain_broken) {
        vfs_atomic_store_i64(hdr_bin_count, total_valid);
        if (chain_broken) {
            /* Truncate the chain: bin_tail = last_valid_page, set
               last_valid_page's next to 0.  Lost entries are
               accepted as a recovery cost. */
            vfs_atomic_store_i64(hdr_bin_tail, last_valid_page);
        }
        /* Update on-disk header: rewrite the count, write the
         * corrected payload back to disk, and refresh CRC.
         * (Phase 28 W4 review B2 fix: previously we only wrote
         * the new CRC to the PageHeader, leaving the on-disk
         * payload inconsistent with the header — would cause
         * mount failure on next boot.) */
        uint8_t* payload = malloc((size_t)sb->page_size);
        if (payload) {
            if (pread(sb->fd, payload, (size_t)sb->page_size,
                      PAGE_HEADER_SIZE) == sb->page_size) {
                vfs_wr8_s(payload, HDR_OFF_BIN_COUNT, total_valid, sb->page_size);
                /* Write the corrected payload back to disk BEFORE
                 * writing the new CRC, so the on-disk state is
                 * always consistent (payload matches its CRC). */
                if (pwrite(sb->fd, payload, (size_t)sb->page_size,
                           PAGE_HEADER_SIZE) != sb->page_size) {
                    fprintf(stderr, "bin validation: pwrite payload failed\n");
                }
                uint32_t new_crc = vfs_crc32c(payload, (size_t)sb->page_size);
                PageHeader ph;
                if (pread(sb->fd, &ph, PAGE_HEADER_SIZE, 0) == PAGE_HEADER_SIZE) {
                    ph.checksum = new_crc;
                    pwrite(sb->fd, &ph, PAGE_HEADER_SIZE, 0);
                }
            }
            free(payload);
        }
        if (chain_broken) {
            fprintf(stderr,
                "bin validation: chain broken; truncated at page %lld "
                "(count %lld -> %lld)\n",
                (long long)last_valid_page,
                (long long)stored_count, (long long)total_valid);
        } else {
            fprintf(stderr,
                "bin validation: global count %lld -> %lld\n",
                (long long)stored_count, (long long)total_valid);
        }
    }
}
