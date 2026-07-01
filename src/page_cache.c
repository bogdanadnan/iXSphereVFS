#include "storage.h"
#include "page_buf.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------
 * Page cache implementation
 *
 * Hash table with per-bucket spin-locks.  LRU eviction (clean pages only).
 * --------------------------------------------------------------------------- */

/* Simple hash: splitmix64 */
static uint64_t hash_page(int64_t page) {
    uint64_t x = (uint64_t)page;
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return x;
}

static inline int bucket_index(PageCache* cache, int64_t page) {
    return (int)(hash_page(page) & (uint64_t)(cache->bucket_count - 1));
}

/* Spin-lock helpers */
static inline void spin_lock(int* lock) {
    while (__sync_lock_test_and_set(lock, 1)) {
        /* spin */
    }
}
static inline void spin_unlock(int* lock) {
    __sync_lock_release(lock);
}

/* ---------------------------------------------------------------------------
 * LRU list helpers (caller holds the bucket lock)
 * --------------------------------------------------------------------------- */

static void lru_promote(PageCache* cache, CacheEntry* e) {
    if (cache->lru_head == e) return;  /* already at head */

    /* Remove from current position */
    if (e->lru_prev) e->lru_prev->lru_next = e->lru_next;
    if (e->lru_next) e->lru_next->lru_prev = e->lru_prev;
    if (cache->lru_tail == e) cache->lru_tail = e->lru_prev;

    /* Insert at head */
    e->lru_prev = NULL;
    e->lru_next = cache->lru_head;
    if (cache->lru_head) cache->lru_head->lru_prev = e;
    cache->lru_head = e;
    if (!cache->lru_tail) cache->lru_tail = e;
}

static void lru_insert_head(PageCache* cache, CacheEntry* e) {
    e->lru_prev = NULL;
    e->lru_next = cache->lru_head;
    if (cache->lru_head) cache->lru_head->lru_prev = e;
    cache->lru_head = e;
    if (!cache->lru_tail) cache->lru_tail = e;
}

static void lru_remove(PageCache* cache, CacheEntry* e) {
    if (e->lru_prev) e->lru_prev->lru_next = e->lru_next;
    if (e->lru_next) e->lru_next->lru_prev = e->lru_prev;
    if (cache->lru_head == e) cache->lru_head = e->lru_next;
    if (cache->lru_tail == e) cache->lru_tail = e->lru_prev;
}

/* ---------------------------------------------------------------------------
 * cache_init / cache_destroy
 * --------------------------------------------------------------------------- */

void cache_init(PageCache* cache, int64_t page_size) {
    cache->bucket_count = CACHE_DEFAULT_BUCKETS;
    cache->buckets      = calloc((size_t)cache->bucket_count, sizeof(CacheEntry*));
    cache->bucket_locks = calloc((size_t)cache->bucket_count, sizeof(int));
    cache->lru_head     = NULL;
    cache->lru_tail     = NULL;
    cache->entry_count  = 0;
    cache->max_entries  = CACHE_DEFAULT_MAX;
    cache->dirty_count  = 0;
    cache->writeback_threshold = cache->max_entries / 4;
    cache->page_size    = page_size;
}

void cache_destroy(PageCache* cache) {
    /* Free all entries */
    CacheEntry* e = cache->lru_head;
    while (e) {
        CacheEntry* next = e->lru_next;
        free(e->payload);
        free(e);
        e = next;
    }
    free(cache->buckets);
    free(cache->bucket_locks);
    cache->buckets      = NULL;
    cache->bucket_locks = NULL;
    cache->lru_head     = NULL;
    cache->lru_tail     = NULL;
    cache->entry_count  = 0;
}

/* ---------------------------------------------------------------------------
 * cache_find
 * --------------------------------------------------------------------------- */

CacheEntry* cache_find(PageCache* cache, int64_t logical_page) {
    int bkt = bucket_index(cache, logical_page);
    spin_lock(&cache->bucket_locks[bkt]);

    CacheEntry* e = cache->buckets[bkt];
    while (e) {
        if (e->logical_page == logical_page) {
            lru_promote(cache, e);
            spin_unlock(&cache->bucket_locks[bkt]);
            return e;
        }
        e = e->hash_next;
    }

    spin_unlock(&cache->bucket_locks[bkt]);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Evict clean pages from LRU tail
 * --------------------------------------------------------------------------- */

static void cache_evict(PageCache* cache) {
    /* Walk from tail, evicting clean pages */
    CacheEntry* e = cache->lru_tail;
    while (e && cache->entry_count >= cache->max_entries) {
        CacheEntry* prev = e->lru_prev;

        if (!e->dirty) {
            /* Remove from hash chain */
            int bkt = bucket_index(cache, e->logical_page);
            spin_lock(&cache->bucket_locks[bkt]);

            CacheEntry** pp = &cache->buckets[bkt];
            while (*pp && *pp != e) pp = &(*pp)->hash_next;
            if (*pp == e) *pp = e->hash_next;

            lru_remove(cache, e);
            cache->entry_count--;

            spin_unlock(&cache->bucket_locks[bkt]);

            free(e->payload);
            free(e);
        }

        e = prev;
    }
}

/* ---------------------------------------------------------------------------
 * cache_insert
 * --------------------------------------------------------------------------- */

void cache_insert(PageCache* cache, int64_t logical_page,
                  uint8_t* payload, int priority, int dirty) {
    /* For clean inserts (from storage_read), take ownership of the caller's
       malloc'd buffer — no copy needed.  For dirty inserts (from storage_write),
       the caller's buffer is stack/const — we must copy it. */
    int take_ownership = !dirty;

    int bkt = bucket_index(cache, logical_page);
    spin_lock(&cache->bucket_locks[bkt]);

    /* Check if entry already exists */
    CacheEntry* e = cache->buckets[bkt];
    while (e) {
        if (e->logical_page == logical_page) {
            /* Update existing entry — we already own the payload buffer */
            memcpy(e->payload, payload, (size_t)cache->page_size);
            e->priority = priority;
            if (dirty) {
                if (!e->dirty) cache->dirty_count++;
                e->dirty = 1;
            }
            lru_promote(cache, e);
            spin_unlock(&cache->bucket_locks[bkt]);
            if (take_ownership) free(payload);
            return;
        }
        e = e->hash_next;
    }

    /* Allocate new entry */
    e = calloc(1, sizeof(CacheEntry));
    if (!e) { spin_unlock(&cache->bucket_locks[bkt]); return; }

    if (take_ownership) {
        e->payload = payload;  /* take ownership — no copy */
    } else {
        e->payload = malloc((size_t)cache->page_size);
        if (!e->payload) { free(e); spin_unlock(&cache->bucket_locks[bkt]); return; }
        memcpy(e->payload, payload, (size_t)cache->page_size);
    }

    e->logical_page = logical_page;
    e->priority     = priority;
    e->dirty        = dirty;
    e->lru_prev     = NULL;
    e->lru_next     = NULL;

    /* Insert into hash chain */
    e->hash_next = cache->buckets[bkt];
    cache->buckets[bkt] = e;

    /* Insert at LRU head */
    lru_insert_head(cache, e);
    cache->entry_count++;
    if (dirty) cache->dirty_count++;

    spin_unlock(&cache->bucket_locks[bkt]);

    /* Evict if over capacity */
    if (cache->entry_count > cache->max_entries) {
        cache_evict(cache);
    }
}

/* ---------------------------------------------------------------------------
 * cache_mark_dirty
 * --------------------------------------------------------------------------- */

void cache_mark_dirty(PageCache* cache, int64_t logical_page, int priority) {
    int bkt = bucket_index(cache, logical_page);
    spin_lock(&cache->bucket_locks[bkt]);

    CacheEntry* e = cache->buckets[bkt];
    while (e) {
        if (e->logical_page == logical_page) {
            if (!e->dirty) cache->dirty_count++;
            e->dirty    = 1;
            e->priority = priority;
            lru_promote(cache, e);
            break;
        }
        e = e->hash_next;
    }

    spin_unlock(&cache->bucket_locks[bkt]);
}

/* ---------------------------------------------------------------------------
 * Flush a single page to disk
 * --------------------------------------------------------------------------- */

void cache_flush_page(StorageBackend* sb, int64_t logical_page) {
    PageCache* cache = &sb->cache;
    CacheEntry* e = cache_find(cache, logical_page);
    if (!e || !e->dirty) return;

    int bkt = bucket_index(cache, logical_page);
    spin_lock(&cache->bucket_locks[bkt]);

    /* Write through the storage backend */
    int64_t offset = indir_lookup(sb, logical_page);
    if (offset != 0) {
        uint32_t crc = vfs_crc32c(e->payload, (size_t)sb->page_size);

        PageHeader ph;
        ssize_t n = pread(sb->fd, &ph, PAGE_HEADER_SIZE, offset);
        if (n == PAGE_HEADER_SIZE) {
            ph.checksum = crc;
            ssize_t w1 = pwrite(sb->fd, &ph, PAGE_HEADER_SIZE, offset);
            ssize_t w2 = pwrite(sb->fd, e->payload, (size_t)sb->page_size,
                                offset + PAGE_HEADER_SIZE);
            if (w1 == PAGE_HEADER_SIZE && w2 == sb->page_size) {
                e->dirty = 0;
                cache->dirty_count--;
            }
        }
    }

    spin_unlock(&cache->bucket_locks[bkt]);
}

/* ---------------------------------------------------------------------------
 * Flush all dirty pages in priority order
 * --------------------------------------------------------------------------- */

void cache_flush_all(StorageBackend* sb) {
    PageCache* cache = &sb->cache;

    /* Flush in priority order: 0 (data) first, 3 (superblock) last */
    for (int prio = 0; prio <= 3; prio++) {
        /* Scan all buckets */
        for (int bkt = 0; bkt < cache->bucket_count; bkt++) {
            spin_lock(&cache->bucket_locks[bkt]);

            CacheEntry* e = cache->buckets[bkt];
            while (e) {
                CacheEntry* next = e->hash_next;
                if (e->dirty && e->priority == prio) {
                    /* Write to disk */
                    int64_t offset = indir_lookup(sb, e->logical_page);
                    if (offset != 0) {
                        uint32_t crc = vfs_crc32c(e->payload, (size_t)sb->page_size);
                        PageHeader ph;
                        ssize_t n = pread(sb->fd, &ph, PAGE_HEADER_SIZE, offset);
                        if (n == PAGE_HEADER_SIZE) {
                            ph.checksum = crc;
                            ssize_t w1 = pwrite(sb->fd, &ph, PAGE_HEADER_SIZE, offset);
                            ssize_t w2 = pwrite(sb->fd, e->payload, (size_t)sb->page_size,
                                                offset + PAGE_HEADER_SIZE);
                            if (w1 == PAGE_HEADER_SIZE && w2 == sb->page_size) {
                                e->dirty = 0;
                                cache->dirty_count--;
                            }
                        }
                    }
                }
                e = next;
            }

            spin_unlock(&cache->bucket_locks[bkt]);
        }
    }
}
