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

void cache_init(PageCache* cache, StorageBackend* sb, int64_t page_size) {
    cache->bucket_count = CACHE_DEFAULT_BUCKETS;
    cache->buckets      = calloc((size_t)cache->bucket_count, sizeof(CacheEntry*));
    cache->bucket_locks = calloc((size_t)cache->bucket_count, sizeof(int));
    cache->entry_count  = 0;
    cache->max_entries  = CACHE_DEFAULT_MAX;
    cache->dirty_count  = 0;
    cache->writeback_threshold = cache->max_entries / 4;
    cache->page_size    = page_size;
    cache->lru_clock    = 1;
    cache->sb           = sb;
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

static void cache_evict_batch(PageCache* cache, int target) {
    /* Evict up to `target` entries in one call.  Eligible candidates:
       - clean entries of any priority (LRU among them)
       - DIRTY data pages (priority == 0) only (LRU among them)
       We never evict a dirty pool/indirection/superblock page
       (priority 1-3) mid-process — a partial flush of those would
       leave the VFS in an inconsistent state.

       Each eviction: detach under bucket lock, release lock, do I/O
       (mirror_write for dirty entries), then free the entry.  No I/O
       is performed while holding a spin-lock. */
    int evicted = 0;
    while (evicted < target) {
        CacheEntry* victim      = NULL;
        CacheEntry** victim_pp  = NULL;
        int         victim_bkt  = -1;
        uint64_t    lowest      = UINT64_MAX;

        for (int bkt = 0; bkt < cache->bucket_count; bkt++) {
            spin_lock(&cache->bucket_locks[bkt]);
            CacheEntry** pp = &cache->buckets[bkt];
            while (*pp) {
                CacheEntry* e = *pp;
                int eligible = (!e->dirty) ||
                               (e->dirty && e->priority == 0);
                if (eligible &&
                    e->timestamp > 0 && e->timestamp < lowest) {
                    lowest    = e->timestamp;
                    victim    = e;
                    victim_bkt = bkt;
                    victim_pp = pp;
                }
                pp = &e->hash_next;
            }
            spin_unlock(&cache->bucket_locks[bkt]);
            if (victim && lowest == 1) break;  /* can't go lower */
        }

        if (!victim) break;  /* nothing more eligible (cache full of
                                 pool/indir/superblock dirty pages) */

        /* Detach under bucket lock; re-validate state in case a
           concurrent thread changed it. */
        spin_lock(&cache->bucket_locks[victim_bkt]);
        if (*victim_pp != victim) {
            spin_unlock(&cache->bucket_locks[victim_bkt]);
            continue;
        }
        int is_dirty = victim->dirty;
        int is_prio0  = victim->priority == 0;
        int still_eligible = (!is_dirty) || (is_dirty && is_prio0);
        if (!still_eligible) {
            spin_unlock(&cache->bucket_locks[victim_bkt]);
            continue;
        }
        *victim_pp = victim->hash_next;
        cache->entry_count--;
        if (victim->dirty) cache->dirty_count--;
        int64_t logical_page = victim->logical_page;
        uint8_t* payload     = victim->payload;
        uint32_t priority    = (uint32_t)victim->priority;
        spin_unlock(&cache->bucket_locks[victim_bkt]);

        /* Flush dirty entry to disk WITHOUT holding any spin-lock.
           mirror_write handles the lazy mirror lifecycle transparently. */
        if (is_dirty && cache->sb) {
            if (indir_lookup(cache->sb, logical_page) != 0) {
                mirror_write(cache->sb, logical_page, payload, priority);
            }
        }

        free(payload);
        free(victim);
        evicted++;
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

    /* Eviction triggers only at 100% of cache budget
       (entry_count >= max_entries).  We evict a meaningful batch
       (25% of max_entries) in one call so the eviction work is
       amortized and we don't run the scan on every insert that
       nudges us over the limit.  Eligible candidates:
         - clean entries of any priority (LRU among them)
         - dirty data pages (priority == 0) — flushed via mirror_write
           before being freed
       Dirty pool/indir/superblock pages (priority 1-3) are NEVER
       evicted mid-process — partial flush of those would leave the
       VFS in an inconsistent state.  If the cache is full of those,
       the eviction scan finds no eligible victims and returns early;
       the cache grows until unmount-time flush + free. */
    if (cache->entry_count >= cache->max_entries) {
        int target = cache->max_entries / 4;
        if (target < 16) target = 16;  /* avoid evicting in tiny crumbs */
        cache_evict_batch(cache, target);
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

    /* Re-check after acquiring the lock — another thread may have flushed
       or evicted the entry in the meantime. */
    e = cache->buckets[bkt];
    while (e && e->logical_page != logical_page) e = e->hash_next;
    if (e && e->dirty) {
        if (indir_lookup(sb, logical_page) != 0) {
            if (mirror_write(sb, logical_page, e->payload, (uint32_t)e->priority) == 0) {
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

    /* Flush in priority order: 0 (data) first, 3 (superblock) last.
       Per-page disk I/O goes through mirror_write, which transparently
       handles the lazy mirror lifecycle (first write = single copy, second
       = allocate mirror sibling, subsequent = alternate).  The mirror is a
       property of the on-disk page state, not the cache. */
    for (int prio = 0; prio <= 3; prio++) {
        for (int bkt = 0; bkt < cache->bucket_count; bkt++) {
            spin_lock(&cache->bucket_locks[bkt]);

            CacheEntry* e = cache->buckets[bkt];
            while (e) {
                CacheEntry* next = e->hash_next;
                if (e->dirty && e->priority == prio) {
                    /* Indirection must have an entry for this page (allocated
                       by pool_alloc) — otherwise skip; we don't know where to
                       write it. */
                    if (indir_lookup(sb, e->logical_page) != 0) {
                        if (mirror_write(sb, e->logical_page, e->payload,
                                         (uint32_t)e->priority) == 0) {
                            e->dirty = 0;
                            cache->dirty_count--;
                        }
                    }
                }
                e = next;
            }

            spin_unlock(&cache->bucket_locks[bkt]);
        }
    }
}

void cache_dump_dirty_by_priority(StorageBackend* sb) {
    /* No-op stub — diagnostic helper, off by default. */
    (void)sb;
}
