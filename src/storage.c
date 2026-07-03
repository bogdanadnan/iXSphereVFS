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
static volatile int64_t _cache_hits  = 0;

int64_t vfs_cache_get_max_entries(StorageBackend* sb) {
    return sb ? (int64_t)sb->cache.max_entries : (int64_t)CACHE_DEFAULT_MAX;
}

void vfs_cache_evict_all(StorageBackend* sb) {
    if (sb) cache_evict_all(&sb->cache);
}

void vfs_cache_reset(void) {
    _cache_total = 0;
    _cache_hits  = 0;
}

int64_t vfs_cache_total(void) { return _cache_total; }
int64_t vfs_cache_hits(void)  { return _cache_hits; }

int64_t vfs_cache_max_entries(void) { return CACHE_DEFAULT_MAX; }

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
 * raw_read / raw_write  (Stage A — no mirror, no cache)
 * --------------------------------------------------------------------------- */

int raw_read(StorageBackend* sb, int64_t logical_page, uint8_t* out_payload) {
    int64_t offset = indir_lookup(sb, logical_page);
    if (offset == 0) return -1;  /* never written */

    /* Read the 16-byte PageHeader */
    PageHeader ph;
    ssize_t n = pread(sb->fd, &ph, PAGE_HEADER_SIZE, offset);
    if (n != PAGE_HEADER_SIZE) return -1;

    /* Read the payload */
    n = pread(sb->fd, out_payload, (size_t)sb->page_size, offset + PAGE_HEADER_SIZE);
    if (n != sb->page_size) return -1;

    /* Validate CRC32C */
    uint32_t computed = vfs_crc32c(out_payload, (size_t)sb->page_size);
    if (computed != ph.checksum) return -1;

    return 0;
}

int raw_write(StorageBackend* sb, int64_t logical_page, const uint8_t* payload,
              uint32_t flags) {
    int64_t offset = indir_lookup(sb, logical_page);
    if (offset == 0) return -1;  /* not allocated */

    /* Compute CRC32C */
    uint32_t crc = vfs_crc32c(payload, (size_t)sb->page_size);

    /* Build PageHeader — preserve existing mirror/generation if any */
    PageHeader ph;
    ssize_t n = pread(sb->fd, &ph, PAGE_HEADER_SIZE, offset);
    if (n != PAGE_HEADER_SIZE) {
        memset(&ph, 0, sizeof(ph));
    }
    ph.flags      = flags;
    ph.checksum   = crc;
    ph.generation += 1;
    if (ph.mirror_page == 0) ph.mirror_page = -1;  /* 0 is not a valid mirror; -1 = none */

    /* Write header */
    n = pwrite(sb->fd, &ph, PAGE_HEADER_SIZE, offset);
    if (n != PAGE_HEADER_SIZE) return -1;

    /* Write payload */
    n = pwrite(sb->fd, payload, (size_t)sb->page_size, offset + PAGE_HEADER_SIZE);
    if (n != sb->page_size) return -1;

    return 0;
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

#define HDR_OFF_TOTAL_PAGES     0
#define HDR_OFF_PAGE_SIZE       8
#define HDR_OFF_SEGMENT_SIZE    16
#define HDR_OFF_PHYS_TAIL       24
#define HDR_OFF_INDIR_HEAD      32
#define HDR_OFF_ENTRIES         40

/* Number of inline indirection entries in the header page. */
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
    ensure_mirror_arrays(sb, 2);
    sb->generations[0]   = 1;
    sb->mirror_pages[0]  = -1;

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

    /* Sync in-memory lazy mirror tracking from on-disk PageHeader */
    ensure_mirror_arrays(sb, 1);
    sb->generations[0]  = ph.generation;
    sb->mirror_pages[0] = ph.mirror_page;

    return 0;
}

/* ---------------------------------------------------------------------------
 * Allocate mirror/generation tracking arrays
 * --------------------------------------------------------------------------- */

int ensure_mirror_arrays(StorageBackend* sb, int min_cap) {
    /* Fast path: already big enough */
    if (sb->mirror_cap >= min_cap) return 0;

    /* Slow path: acquire lock for realloc */
    while (__sync_lock_test_and_set(&sb->mirror_lock, 1)) { /* spin */ }

    /* Double-check after acquiring lock (another thread may have grown) */
    if (sb->mirror_cap >= min_cap) {
        __sync_lock_release(&sb->mirror_lock);
        return 0;
    }

    int new_cap = sb->mirror_cap ? sb->mirror_cap * 2 : 256;
    while (new_cap < min_cap) new_cap *= 2;

    int32_t*  mp = realloc(sb->mirror_pages, (size_t)new_cap * sizeof(int32_t));
    uint32_t* gn = realloc(sb->generations,  (size_t)new_cap * sizeof(uint32_t));
    if (!mp || !gn) {
        __sync_lock_release(&sb->mirror_lock);
        return -1;
    }

    /* Initialize new entries */
    for (int i = sb->mirror_cap; i < new_cap; i++) {
        mp[i] = -1;   /* no mirror */
        gn[i] = 0;     /* generation 0 = never written */
    }

    sb->mirror_pages = mp;
    sb->generations  = gn;
    /* Memory barrier: ensure arrays are visible before updating cap */
    __sync_synchronize();
    sb->mirror_cap   = new_cap;

    __sync_lock_release(&sb->mirror_lock);
    return 0;
}

/* ---------------------------------------------------------------------------
 * storage_open / storage_close  (W2.6)
 * --------------------------------------------------------------------------- */

StorageBackend* storage_open(const char* path, int64_t page_size) {
    StorageBackend* sb = calloc(1, sizeof(StorageBackend));
    if (!sb) return NULL;

    sb->page_size  = page_size > 0 ? page_size : 8192;
    sb->fd         = -1;
    sb->mirror_pages = NULL;
    sb->generations  = NULL;
    sb->mirror_cap   = 0;

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
    ensure_mirror_arrays(sb, (int)sb->total_pages);

    /* Initialize page cache */
    cache_init(&sb->cache, sb->page_size);

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
    free(sb->mirror_pages);
    free(sb->generations);

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
    if (indir_lookup(sb, logical_page) != 0) return 0;  /* fast path: already taken */

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

int64_t storage_allocate(StorageBackend* sb, int count) {
    if (count <= 0) return -1;

    /* Ensure indirection table has room */
    if (indir_ensure_capacity(sb, count) != 0) return -1;

    /* Lock-free: scan for 'count' consecutive free entries and CAS-claim them.
       If any CAS fails (another thread claimed the slot), restart the scan. */
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

            ensure_mirror_arrays(sb, (int)(logical + 1));
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
    ensure_mirror_arrays(sb, (int)(logical_page + 1));

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

void storage_free(StorageBackend* sb, int64_t logical_page) {
    if (logical_page < 2) return;  /* don't free header or superblock */

    int64_t offset = indir_lookup(sb, logical_page);
    if (offset == 0) return;  /* already free */

    /* Free the mirror sibling if it exists */
    if (logical_page < sb->mirror_cap) {
        int32_t mp = sb->mirror_pages[logical_page];
        if (mp >= 0) {
            indir_set(sb, mp, 0);
            sb->mirror_pages[logical_page] = -1;
        }
    }

    /* Free this page */
    indir_set(sb, logical_page, 0);
    sb->generations[logical_page] = 0;
    sb->mirror_pages[logical_page] = -1;
}

/* ---------------------------------------------------------------------------
 * Read / Write / Flush  (public — goes through cache + lazy mirror)
 * --------------------------------------------------------------------------- */

uint8_t* storage_read(StorageBackend* sb, int64_t logical_page) {
    _cache_total++;

    /* 1. Check page cache */
    CacheEntry* ce = cache_find(&sb->cache, logical_page);
    if (ce) { _cache_hits++; return ce->payload; }

    /* 2. Check if page is allocated */
    int64_t offset = indir_lookup(sb, logical_page);
    if (offset == 0) return NULL;  /* never written */

    /* 3. Read from disk via lazy mirror */
    uint8_t* buf = malloc((size_t)sb->page_size);
    if (!buf) return NULL;

    if (mirror_read(sb, logical_page, buf) != 0) {
        free(buf);
        return NULL;
    }

    /* 4. Insert into cache — cache takes ownership of buf (no copy) */
    cache_insert(&sb->cache, logical_page, buf, 0, 0);

    /* 5. Return the cached payload (cache owns buf now) */
    ce = cache_find(&sb->cache, logical_page);
    return ce ? ce->payload : NULL;
}

void storage_write(StorageBackend* sb, int64_t logical_page, const uint8_t* payload,
                   uint32_t priority) {
    /* 1. Write through lazy mirror to disk */
    mirror_write(sb, logical_page, payload, priority);

    /* 2. Update cache — mark dirty */
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
