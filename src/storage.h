#ifndef VFS_STORAGE_H
#define VFS_STORAGE_H

#include "ixsphere/vfs.h"
#include "platform.h"
#include "page_buf.h"
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * PageHeader  (16 bytes, transparent to callers)
 *
 * Written at the physical start of every page on disk.
 * Read/Write payloads only; the StorageBackend handles the header.
 * --------------------------------------------------------------------------- */

typedef struct {
    uint32_t flags;        /* bits 0–1 = flush priority; page 0 uses 0x56585346 ("XVFS") */
    uint32_t checksum;     /* CRC32C of the payload bytes */
    uint32_t generation;   /* incremented on each write; higher = active */
    int32_t  mirror_page;  /* logical page index of mirror sibling; -1 = none */
} PageHeader;

#define XVFS_MAGIC          0x56585346u  /* "XVFS" in little-endian */
#define PAGE_HEADER_SIZE    16
#define HDR_FLAG_PRIORITY_MASK  0x3u     /* bits 0–1 */

/* Header page payload offsets */
#define HDR_OFF_TOTAL_PAGES     0
#define HDR_OFF_PAGE_SIZE       8
#define HDR_OFF_SEGMENT_SIZE    16
#define HDR_OFF_PHYS_TAIL       24
#define HDR_OFF_INDIR_HEAD      32
#define HDR_OFF_ENTRIES         40

/* Flush priorities (stored in flags bits 0–1) */
#define FLUSH_PRIO_DATA        0
#define FLUSH_PRIO_POOL        1
#define FLUSH_PRIO_INDIR       2
#define FLUSH_PRIO_SUPERBLOCK  3

/* ---------------------------------------------------------------------------
 * Indirection table
 * --------------------------------------------------------------------------- */

typedef struct {
    int64_t* inline_entries;   /* points into header page payload at offset 40 */
    int      inline_count;     /* (page_size - 40) / 8 */

    /* Overflow chain — dynamically grown array of page buffers */
    int64_t** overflow_pages;  /* each points to a page_size buffer (next + entries) */
    int64_t*  overflow_logical; /* logical page index for each overflow page */
    int       overflow_count;  /* number of overflow pages */
    int       overflow_cap;    /* allocated capacity */
    volatile int overflow_lock; /* Phase 27 C6: protects the entire
                                 * overflow-page allocation path in
                                 * indir_ensure_capacity — the realloc,
                                 * the chain link, and the array
                                 * assignment.  Held under double-checked
                                 * locking (lock-free fast path for the
                                 * common case where no new overflow page
                                 * is needed).  Previously only protected
                                 * the realloc, which left the chain link
                                 * and array assignment racy under
                                 * multi-threaded storage_allocate. */

    int64_t   entries_per_overflow; /* (page_size / 8) - 1 */
} IndirectionTable;

/* ---------------------------------------------------------------------------
 * Page cache entry
 * --------------------------------------------------------------------------- */

typedef struct CacheEntry {
    int64_t  logical_page;
    uint8_t* payload;           /* malloc'd, size = page_size */
    int      priority;          /* flush priority 0–3 */
    int      dirty;             /* 1 if modified since last flush */
    uint64_t timestamp;         /* generation for eviction (lock-free) */
    struct CacheEntry* hash_next;
} CacheEntry;

/* ---------------------------------------------------------------------------
 * Page cache
 * --------------------------------------------------------------------------- */

#define CACHE_DEFAULT_BUCKETS  16384
#define CACHE_DEFAULT_MAX      65536   /* 512 MB at 8KB pages */

typedef struct {
    CacheEntry** buckets;
    int          bucket_count;
    int*         bucket_locks;  /* spin-locks per bucket (0=free, 1=held) */
    int          entry_count;
    int          max_entries;
    int64_t      dirty_count;
    int64_t      writeback_threshold;
    int64_t      page_size;
    volatile uint64_t lru_clock; /* monotonic generation counter for eviction */
    struct StorageBackend* sb;  /* back-pointer for flush-during-evict */
} PageCache;

/* ---------------------------------------------------------------------------
 * StorageBackend — the main handle
 * --------------------------------------------------------------------------- */

typedef struct StorageBackend {
    int              fd;            /* backing file descriptor */
    int64_t          total_pages;   /* highest allocated logical page + 1 */
    int64_t          page_size;     /* payload size in bytes */
    uint32_t         segment_size;  /* pages per FileContent segment */
    int64_t          physical_tail; /* next available physical byte offset */
    int64_t          indirection_head; /* logical page of first overflow page, 0 = none */

    IndirectionTable indir;
    PageCache        cache;

    /* Header page buffer — kept in memory, flushed on Flush(-1) */
    uint8_t*         header_buf;    /* page_size bytes, the header page payload */
} StorageBackend;

/* ---------------------------------------------------------------------------
 * Public API (storage.c)
 * --------------------------------------------------------------------------- */

/* Forward declaration — full type in gc.h */
struct DeferredFreeQueue;

StorageBackend* storage_open(const char* path, int64_t page_size);
void            storage_close(StorageBackend* sb);

int64_t         storage_allocate(StorageBackend* sb, int count);
int             storage_acquire(StorageBackend* sb, int64_t logical_page);
void            storage_free(StorageBackend* sb, int64_t logical_page);

/* Status of a storage_read attempt.  Phase 27 C5: distinguishes
   "page not allocated" (sparse-file semantics: zero-fill) from
   "I/O error" (read failed) and "CRC error" (data corruption).
   Previously all three were collapsed into a NULL return, which
   caused silent data loss on disk corruption. */
typedef enum {
    STORAGE_OK         = 0,  /* payload valid, do not free */
    STORAGE_NOT_FOUND  = 1,  /* not allocated in indirection table (sparse) */
    STORAGE_IO_ERROR   = 2,  /* pread/pwrite failed */
    STORAGE_CRC_ERROR  = 3   /* CRC32C mismatch — data corruption detected */
} StorageReadStatus;

/* Read with explicit status.  Returns a cache-resident payload on
   success (do not free); returns NULL on failure, with *out_status
   set to the reason.  *out_status may be NULL (caller does not
   distinguish "not allocated" from "I/O error" from "CRC error"). */
uint8_t*        storage_read_with_status(StorageBackend* sb, int64_t logical_page,
                                         StorageReadStatus* out_status);

void            storage_write(StorageBackend* sb, int64_t logical_page, const uint8_t* payload, uint32_t priority);
void            storage_flush(StorageBackend* sb, int64_t logical_page);
void            storage_flush_cache_only(StorageBackend* sb);

/* Set the deferred-free queue — called by GC so storage_allocate skips
   pages that may still be referenced by in-flight readers. */
void storage_set_deferred_queue(struct DeferredFreeQueue* queue);

/* Internal helpers (storage.c) */
int64_t phys_record_size(StorageBackend* sb);

/* Diagnostics — dump lazy mirror metrics if VFS_LAZY_MIRROR_METRICS=1.
   Stashed instrumentation; opt-in via env var. */
void mirror_metrics_dump(void);
void* mirror_metrics_pump(void* arg);
int     inline_entry_count(int64_t page_size);

/* ---------------------------------------------------------------------------
 * Internal: indirection  (indirection.c)
 * --------------------------------------------------------------------------- */

void    indir_init(StorageBackend* sb);
int64_t indir_lookup(StorageBackend* sb, int64_t logical_page);
void    indir_set(StorageBackend* sb, int64_t logical_page, int64_t physical_offset);
int     indir_ensure_capacity(StorageBackend* sb, int needed);

/* ---------------------------------------------------------------------------
 * Internal: lazy mirror  (lazy_mirror.c)
 * --------------------------------------------------------------------------- */

/* Phase 27 C5: status-returning variant.  0 on success, -1 on I/O
   error, -2 on CRC error.  See lazy_mirror.c for the distinction. */
int  mirror_read_with_status(StorageBackend* sb, int64_t logical_page, uint8_t* out_payload);
int  mirror_write(StorageBackend* sb, int64_t logical_page, const uint8_t* payload,
                  uint32_t flags);

/* ---------------------------------------------------------------------------
 * Internal: page cache  (page_cache.c)
 * --------------------------------------------------------------------------- */

void cache_init(PageCache* cache, struct StorageBackend* sb, int64_t page_size);
void cache_destroy(PageCache* cache);
CacheEntry* cache_find(PageCache* cache, int64_t logical_page);
void        cache_insert(PageCache* cache, int64_t logical_page,
                         uint8_t* payload, int priority, int dirty);
void        cache_mark_dirty(PageCache* cache, int64_t logical_page, int priority);
void        cache_flush_page(StorageBackend* sb, int64_t logical_page);
void        cache_evict_all(StorageBackend* sb);
void        cache_flush_all(StorageBackend* sb);

/* Dump per-priority dirty page count (diagnostic only; thread-safe traversal). */
void        cache_dump_dirty_by_priority(StorageBackend* sb);

/* ---------------------------------------------------------------------------
 * Cache performance counters (for benchmark tracking).
 * --------------------------------------------------------------------------- */

/* Reset cache counters to zero. */
int64_t vfs_cache_get_max_entries(StorageBackend* sb);
void vfs_cache_evict_all(StorageBackend* sb);
void vfs_cache_reset(void);

/* Total cache lookups / hits since last reset (all pages). */
int64_t vfs_cache_total(void);
int64_t vfs_cache_hits(void);
int     vfs_cache_was_last_hit(void);

/* Data-page read tracking (counted by vfs_read per data page). */
int64_t vfs_data_total(void);
int64_t vfs_data_hits(void);
void    vfs_data_inc_total(void);
void    vfs_data_inc_hits(void);

/* Number of cache misses since last reset (= total - hits). */
static inline int64_t vfs_cache_misses(void) {
    return vfs_cache_total() - vfs_cache_hits();
}

/* Hit ratio as a double between 0.0 and 1.0. */
static inline double vfs_cache_hit_ratio(void) {
    int64_t t = vfs_cache_total();
    return (t > 0) ? (double)vfs_cache_hits() / (double)t : 0.0;
}

#endif /* VFS_STORAGE_H */
