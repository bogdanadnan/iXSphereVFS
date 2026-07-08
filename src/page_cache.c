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
    e->timestamp = __sync_add_and_fetch(&cache->lru_clock, 1);
}

static void lru_insert_head(PageCache* cache, CacheEntry* e) {
    e->timestamp = __sync_add_and_fetch(&cache->lru_clock, 1);
}

/* ---------------------------------------------------------------------------
 * cache_init / cache_destroy
 * --------------------------------------------------------------------------- */

void cache_init(PageCache* cache, int64_t page_size) {
    cache->bucket_count = CACHE_DEFAULT_BUCKETS;
    cache->buckets      = calloc((size_t)cache->bucket_count, sizeof(CacheEntry*));
    cache->bucket_locks = calloc((size_t)cache->bucket_count, sizeof(int));
    cache->entry_count  = 0;
    cache->max_entries  = CACHE_DEFAULT_MAX;
    cache->dirty_count  = 0;
    cache->writeback_threshold = cache->max_entries / 4;
    cache->page_size    = page_size;
    cache->lru_clock    = 1;
}

void cache_destroy(PageCache* cache) {
    /* Free all entries by walking hash buckets */
    for (int bkt = 0; bkt < cache->bucket_count; bkt++) {
        CacheEntry* e = cache->buckets[bkt];
        while (e) {
            CacheEntry* next = e->hash_next;
            free(e->payload);
            free(e);
            e = next;
        }
    }
    free(cache->buckets);
    free(cache->bucket_locks);
    cache->buckets      = NULL;
    cache->bucket_locks = NULL;
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
    /* Evict clean entries with lowest generation timestamp.
       Lock-free on the hot path — only bucket locks during scanning.
       timestamp=0 means the entry was removed (dangling). */
    while (cache->entry_count > cache->max_entries) {
        CacheEntry* victim = NULL;
        uint64_t    lowest = UINT64_MAX;
        int         victim_bkt = -1;
        CacheEntry** victim_pp = NULL;

        for (int bkt = 0; bkt < cache->bucket_count; bkt++) {
            spin_lock(&cache->bucket_locks[bkt]);
            CacheEntry** pp = &cache->buckets[bkt];
            while (*pp) {
                CacheEntry* e = *pp;
                if (!e->dirty && e->timestamp > 0 && e->timestamp < lowest) {
                    lowest = e->timestamp;
                    victim = e;
                    victim_bkt = bkt;
                    victim_pp = pp;
                }
                pp = &e->hash_next;
            }
            spin_unlock(&cache->bucket_locks[bkt]);
            if (victim && lowest == 1) break;  /* can't get lower than 1 */
        }

        if (!victim) break;  /* no clean entries to evict */

        /* Remove victim from its bucket */
        spin_lock(&cache->bucket_locks[victim_bkt]);
        if (*victim_pp == victim) {
            *victim_pp = victim->hash_next;
            cache->entry_count--;
        }
        spin_unlock(&cache->bucket_locks[victim_bkt]);

        free(victim->payload);
        free(victim);
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
    e->timestamp    = 0;

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

void cache_evict_all(StorageBackend* sb) {
    PageCache* cache = &sb->cache;

    /* Flush all dirty pages to disk before evicting, so that subsequent
       reads from disk retrieve the latest data rather than stale on-disk
       state.  cache_flush_all scans buckets in priority order. */
    cache_flush_all(sb);

    for (int bkt = 0; bkt < cache->bucket_count; bkt++) {
        spin_lock(&cache->bucket_locks[bkt]);
        CacheEntry* e = cache->buckets[bkt];
        while (e) {
            CacheEntry* next = e->hash_next;
            free(e->payload);
            free(e);
            e = next;
        }
        cache->buckets[bkt] = NULL;
        spin_unlock(&cache->bucket_locks[bkt]);
    }
    cache->entry_count = 0;
}

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
