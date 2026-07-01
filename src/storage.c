#include "storage.h"
#include "page_buf.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

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
    vfs_wr8(sb->header_buf, HDR_OFF_TOTAL_PAGES,  2);     /* pages 0 and 1 reserved */
    vfs_wr8(sb->header_buf, HDR_OFF_PAGE_SIZE,     ps);
    vfs_wr4(sb->header_buf, HDR_OFF_SEGMENT_SIZE,  1024);
    vfs_wr8(sb->header_buf, HDR_OFF_PHYS_TAIL,     2 * (ps + PAGE_HEADER_SIZE));
    vfs_wr8(sb->header_buf, HDR_OFF_INDIR_HEAD,    0);

    /* Indirection entries: page 0 at physical 0, page 1 at physical (ps+16) */
    int ic = inline_entry_count(ps);
    /* Zero all entries first */
    for (int i = 0; i < ic; i++) {
        vfs_wr8(sb->header_buf, HDR_OFF_ENTRIES + i * 8, 0);
    }
    vfs_wr8(sb->header_buf, HDR_OFF_ENTRIES + 0 * 8, 0);                   /* page 0 → offset 0 */
    vfs_wr8(sb->header_buf, HDR_OFF_ENTRIES + 1 * 8, ps + PAGE_HEADER_SIZE); /* page 1 → offset ps+16 */

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

    /* Set in-memory state */
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
    int64_t ps = sb->page_size;  /* not yet known — read from header */

    /* Read PageHeader at offset 0 */
    PageHeader ph;
    ssize_t n = pread(sb->fd, &ph, PAGE_HEADER_SIZE, 0);
    if (n != PAGE_HEADER_SIZE) return -1;

    /* Validate XVFS magic */
    if (ph.flags != XVFS_MAGIC) return -1;

    /* Allocate header buffer and read payload */
    uint8_t* hdr = calloc(1, 8192);  /* temp — we don't know page_size yet */
    if (!hdr) return -1;

    n = pread(sb->fd, hdr, 8192, PAGE_HEADER_SIZE);
    if (n != 8192) { free(hdr); return -1; }

    /* Validate CRC32C */
    uint32_t crc = vfs_crc32c(hdr, 8192);
    if (crc != ph.checksum) { free(hdr); return -1; }

    /* Read header fields */
    ps = vfs_rd8(hdr, HDR_OFF_PAGE_SIZE);
    if (ps < 512 || ps > 65536) { free(hdr); return -1; }

    /* Re-read with correct page size if different */
    if (ps != 8192) {
        free(hdr);
        hdr = calloc(1, (size_t)ps);
        if (!hdr) return -1;
        n = pread(sb->fd, hdr, (size_t)ps, PAGE_HEADER_SIZE);
        if (n != ps) { free(hdr); return -1; }
        /* Re-validate CRC */
        crc = vfs_crc32c(hdr, (size_t)ps);
        if (crc != ph.checksum) { free(hdr); return -1; }
    }

    sb->page_size      = ps;
    sb->total_pages     = vfs_rd8(hdr, HDR_OFF_TOTAL_PAGES);
    sb->segment_size    = (uint32_t)vfs_rd4(hdr, HDR_OFF_SEGMENT_SIZE);
    sb->physical_tail   = vfs_rd8(hdr, HDR_OFF_PHYS_TAIL);
    sb->indirection_head = vfs_rd8(hdr, HDR_OFF_INDIR_HEAD);
    sb->header_buf      = hdr;

    return 0;
}

/* ---------------------------------------------------------------------------
 * Allocate mirror/generation tracking arrays
 * --------------------------------------------------------------------------- */

int ensure_mirror_arrays(StorageBackend* sb, int min_cap) {
    if (sb->mirror_cap >= min_cap) return 0;
    int new_cap = sb->mirror_cap ? sb->mirror_cap * 2 : 256;
    while (new_cap < min_cap) new_cap *= 2;

    int32_t*  mp = realloc(sb->mirror_pages, (size_t)new_cap * sizeof(int32_t));
    uint32_t* gn = realloc(sb->generations,  (size_t)new_cap * sizeof(uint32_t));
    if (!mp || !gn) return -1;

    /* Initialize new entries */
    for (int i = sb->mirror_cap; i < new_cap; i++) {
        mp[i] = -1;   /* no mirror */
        gn[i] = 0;     /* generation 0 = never written */
    }

    sb->mirror_pages = mp;
    sb->generations  = gn;
    sb->mirror_cap   = new_cap;
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

/* Spin-lock helpers (reuse from page_cache) */
static inline void sb_spin_lock(volatile int* lock) {
    while (__sync_lock_test_and_set(lock, 1)) { /* spin */ }
}
static inline void sb_spin_unlock(volatile int* lock) {
    __sync_lock_release(lock);
}

int64_t storage_allocate(StorageBackend* sb, int count) {
    if (count <= 0) return -1;

    /* Lock the scan+claim to prevent two threads from claiming the same entry */
    sb_spin_lock(&sb->alloc_lock);

    /* Ensure indirection table has room (must be inside lock for thread safety) */
    if (indir_ensure_capacity(sb, count) != 0) {
        sb_spin_unlock(&sb->alloc_lock);
        return -1;
    }

    int64_t total_entries = (int64_t)sb->indir.inline_count +
                            (int64_t)sb->indir.overflow_count * sb->indir.entries_per_overflow;

    int64_t run_start = -1;
    int     run_len   = 0;

    for (int64_t i = 2; i < total_entries && run_len < count; i++) {
        if (indir_lookup(sb, i) == 0) {
            if (run_len == 0) run_start = i;
            run_len++;
        } else {
            run_start = -1;
            run_len   = 0;
        }
    }

    if (run_len < count) {
        sb_spin_unlock(&sb->alloc_lock);
        return -1;
    }

    /* Allocate physical pages for each logical page in the run */
    for (int j = 0; j < count; j++) {
        int64_t logical = run_start + j;
        int64_t old_tail = sb->physical_tail;
        int64_t new_tail = old_tail + phys_record_size(sb);

        while (vfs_cas_i64(&sb->physical_tail, old_tail, new_tail) != old_tail) {
            old_tail = sb->physical_tail;
            new_tail = old_tail + phys_record_size(sb);
        }

        indir_set(sb, logical, old_tail);
        ensure_mirror_arrays(sb, (int)(logical + 1));
    }

    /* Update total_pages if needed */
    int64_t new_total = run_start + count;
    int64_t old_total;
    do {
        old_total = sb->total_pages;
        if (new_total <= old_total) break;
    } while (vfs_cas_i64(&sb->total_pages, old_total, new_total) != old_total);

    sb_spin_unlock(&sb->alloc_lock);
    return run_start;
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

    /* CAS check: entry must be 0 */
    int64_t current = indir_lookup(sb, logical_page);
    if (current != 0) return 0;  /* already allocated */

    /* CAS advance physical_tail */
    int64_t old_tail = sb->physical_tail;
    int64_t new_tail = old_tail + phys_record_size(sb);
    while (vfs_cas_i64(&sb->physical_tail, old_tail, new_tail) != old_tail) {
        old_tail = sb->physical_tail;
        new_tail = old_tail + phys_record_size(sb);
    }

    /* CAS-set the entry from 0 to old_tail */
    indir_set(sb, logical_page, old_tail);

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
    /* 1. Check page cache */
    CacheEntry* ce = cache_find(&sb->cache, logical_page);
    if (ce) return ce->payload;

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

    /* 4. Insert into cache as clean */
    cache_insert(&sb->cache, logical_page, buf, 0, 0);

    /* cache_insert copies buf; return the cached copy */
    ce = cache_find(&sb->cache, logical_page);
    free(buf);  /* cache made its own copy */
    return ce ? ce->payload : NULL;
}

void storage_write(StorageBackend* sb, int64_t logical_page, const uint8_t* payload) {
    /* 1. Write through lazy mirror to disk */
    uint32_t flags = 0;  /* default priority 0 (data) — VFS layer sets this */
    mirror_write(sb, logical_page, payload, flags);

    /* 2. Update cache — mark dirty */
    cache_insert(&sb->cache, logical_page, (uint8_t*)payload,
                 flags & HDR_FLAG_PRIORITY_MASK, 1);
}

void storage_flush(StorageBackend* sb, int64_t logical_page) {
    if (logical_page < 0) {
        /* First: flush the header page (always at physical offset 0).
           The header buffer holds live indirection entries + total_pages/physical_tail
           that are updated by Allocate/Acquire/Free.  These changes must reach disk
           before any cached data pages so the file is consistent on crash. */

        /* Update header fields from live state */
        vfs_wr8(sb->header_buf, HDR_OFF_TOTAL_PAGES,  sb->total_pages);
        vfs_wr8(sb->header_buf, HDR_OFF_PHYS_TAIL,    sb->physical_tail);
        vfs_wr8(sb->header_buf, HDR_OFF_INDIR_HEAD,   sb->indirection_head);

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

        /* Then flush all cached dirty pages in priority order */
        cache_flush_all(sb);
        fsync(sb->fd);
    } else {
        cache_flush_page(sb, logical_page);
    }
}
