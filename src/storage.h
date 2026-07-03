#ifndef VFS_STORAGE_H
#define VFS_STORAGE_H

#include "ixsphere_vfs.h"
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
    volatile int overflow_lock; /* protects realloc of overflow arrays */

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
    struct CacheEntry* lru_prev;
    struct CacheEntry* lru_next;
    struct CacheEntry* hash_next;
} CacheEntry;

/* ---------------------------------------------------------------------------
 * Page cache
 * --------------------------------------------------------------------------- */

#define CACHE_DEFAULT_BUCKETS  16384
#define CACHE_DEFAULT_MAX      32768   /* 256 MB at 8KB pages */

typedef struct {
    CacheEntry** buckets;
    int          bucket_count;
    int*         bucket_locks;  /* spin-locks per bucket (0=free, 1=held) */
    CacheEntry*  lru_head;     /* most recently used */
    CacheEntry*  lru_tail;     /* least recently used */
    int          entry_count;
    int          max_entries;
    int64_t      dirty_count;
    int64_t      writeback_threshold;  /* trigger write-back when dirty_count >= this */
    int64_t      page_size;   /* cached page size for memcpy */
} PageCache;

/* ---------------------------------------------------------------------------
 * StorageBackend — the main handle
 * --------------------------------------------------------------------------- */

typedef struct {
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

    /* Per-page tracking for lazy mirror */
    int32_t*         mirror_pages;   /* mirror_page per logical page, -1 = none */
    uint32_t*        generations;    /* generation per logical page */
    int              mirror_cap;     /* allocated capacity */
    volatile int     mirror_lock;    /* protects realloc in ensure_mirror_arrays */
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

uint8_t*        storage_read(StorageBackend* sb, int64_t logical_page);
void            storage_write(StorageBackend* sb, int64_t logical_page, const uint8_t* payload, uint32_t priority);
void            storage_flush(StorageBackend* sb, int64_t logical_page);

/* Set the deferred-free queue — called by GC so storage_allocate skips
   pages that may still be referenced by in-flight readers. */
void storage_set_deferred_queue(struct DeferredFreeQueue* queue);

/* ---------------------------------------------------------------------------
 * Internal: raw I/O  (storage.c)
 * --------------------------------------------------------------------------- */

int  raw_read(StorageBackend* sb, int64_t logical_page, uint8_t* out_payload);
int  raw_write(StorageBackend* sb, int64_t logical_page, const uint8_t* payload,
               uint32_t flags);

/* Internal helpers (storage.c) */
int64_t phys_record_size(StorageBackend* sb);
int     inline_entry_count(int64_t page_size);
int     ensure_mirror_arrays(StorageBackend* sb, int min_cap);

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

int  mirror_read(StorageBackend* sb, int64_t logical_page, uint8_t* out_payload);
int  mirror_write(StorageBackend* sb, int64_t logical_page, const uint8_t* payload,
                  uint32_t flags);

/* ---------------------------------------------------------------------------
 * Internal: page cache  (page_cache.c)
 * --------------------------------------------------------------------------- */

void        cache_init(PageCache* cache, int64_t page_size);
void        cache_destroy(PageCache* cache);
CacheEntry* cache_find(PageCache* cache, int64_t logical_page);
void        cache_insert(PageCache* cache, int64_t logical_page,
                         uint8_t* payload, int priority, int dirty);
void        cache_mark_dirty(PageCache* cache, int64_t logical_page, int priority);
void        cache_flush_page(StorageBackend* sb, int64_t logical_page);
void        cache_flush_all(StorageBackend* sb);

#endif /* VFS_STORAGE_H */
