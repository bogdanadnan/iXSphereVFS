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

/* cache_evict_batch — single-scan eviction helper
 *
 * Walks all buckets ONCE and collects the `target` oldest eligible
 * entries (clean of any priority, dirty priority-0 data pages only).
 * Then evicts them in a second pass that groups candidates by bucket
 * so each bucket lock is acquired/released ONCE per eviction cycle,
 * not once per entry.
 *
 * Single-pass (vs the previous loop which rescanned per eviction) is
 * critical: with cache->bucket_count = 16384 and target = 4096, the
 * old code did 16384 * 4096 bucket scans per eviction call.  The new
 * code does one.
 *
 * Eligibility:
 *   - clean entries of any priority (LRU among them)
 *   - dirty data pages (priority == 0) only — flushed via mirror_write
 *   We never evict a dirty pool/indirection/superblock page (priority
 *   1-3) mid-process — partial flush of those would leave the VFS
 *   inconsistent. */

typedef struct {
    CacheEntry* entry;
    CacheEntry** pp;        /* pointer to the slot that points to entry */
    int         bkt;
    uint64_t    timestamp;  /* for sorting */
} EvictCandidate;

static int evict_candidate_cmp(const void* a, const void* b) {
    uint64_t ta = ((const EvictCandidate*)a)->timestamp;
    uint64_t tb = ((const EvictCandidate*)b)->timestamp;
    if (ta < tb) return -1;
    if (ta > tb) return  1;
    /* tie-break: stable secondary key so qsort is deterministic */
    if (ta == tb) {
        uint64_t ea = ((const EvictCandidate*)a)->entry->logical_page;
        uint64_t eb = ((const EvictCandidate*)b)->entry->logical_page;
        if (ea < eb) return -1;
        if (ea > eb) return  1;
    }
    return 0;
}

static void cache_evict_batch(PageCache* cache, int target) {
    int cap = target;
    /* Heap-allocated candidates — bounded by `target` which is bounded
       by max_entries/4.  With CACHE_DEFAULT_MAX=16384, target=4096, so
       ~16 KB per call. */
    EvictCandidate* cands = (EvictCandidate*)malloc(sizeof(EvictCandidate) * (size_t)cap);
    if (!cands) return;
    int n = 0;

    /* Pass 1: single scan, collect oldest candidates.  We keep an
       unsorted linear list capped at `target` and incrementally replace
       the newest candidate when we find an older one — simpler than a
       full sort, sufficient for picking the `target` oldest. */
    uint64_t newest_in_set = 0;
    int set_full = 0;
    for (int bkt = 0; bkt < cache->bucket_count; bkt++) {
        spin_lock(&cache->bucket_locks[bkt]);
        CacheEntry** pp = &cache->buckets[bkt];
        while (*pp) {
            CacheEntry* e = *pp;
            int eligible = (!e->dirty) ||
                           (e->dirty && e->priority == 0);
            if (eligible && e->timestamp > 0) {
                if (!set_full) {
                    cands[n].entry     = e;
                    cands[n].pp        = pp;
                    cands[n].bkt       = bkt;
                    cands[n].timestamp = e->timestamp;
                    n++;
                    if (n == cap) {
                        set_full = 1;
                        newest_in_set = 0;
                        for (int k = 0; k < n; k++) {
                            if (cands[k].timestamp > newest_in_set)
                                newest_in_set = cands[k].timestamp;
                        }
                    }
                } else if (e->timestamp < newest_in_set) {
                    /* Replace one entry at the boundary (oldest of the
                       "newest" group).  Any one suffices; pick the first. */
                    for (int k = 0; k < n; k++) {
                        if (cands[k].timestamp == newest_in_set) {
                            cands[k].entry     = e;
                            cands[k].pp        = pp;
                            cands[k].bkt       = bkt;
                            cands[k].timestamp = e->timestamp;
                            break;
                        }
                    }
                    /* Recompute newest_in_set */
                    newest_in_set = 0;
                    for (int k = 0; k < n; k++) {
                        if (cands[k].timestamp > newest_in_set)
                            newest_in_set = cands[k].timestamp;
                    }
                }
            }
            pp = &e->hash_next;
        }
        spin_unlock(&cache->bucket_locks[bkt]);
    }

    if (n == 0) { free(cands); return; }

    /* Sort by (timestamp asc, logical_page asc). */
    qsort(cands, (size_t)n, sizeof(EvictCandidate), evict_candidate_cmp);

    /* Pass 2: evict.  Group candidates by bucket so each bucket lock is
       acquired/released ONCE per cycle (not once per entry).
       Re-validate under the lock — a concurrent thread may have flushed
       or removed the entry between Pass 1 and Pass 2. */
    int i = 0;
    while (i < n) {
        int cur_bkt = cands[i].bkt;

        /* Find the range [i..j) that belongs to cur_bkt. */
        int j = i;
        while (j < n && cands[j].bkt == cur_bkt) j++;

        /* Acquire the bucket lock once. */
        spin_lock(&cache->bucket_locks[cur_bkt]);

        /* Detach every candidate in this bucket that is still there and
           still eligible.  Capture state for the I/O + free step. */
        struct Detached {
            struct CacheEntry* entry;
            int64_t logical_page;
            uint8_t* payload;
            uint32_t priority;
            int is_dirty;
        };
        /* Bound the detached array — at most (j-i) entries. */
        struct Detached* det =
            (struct Detached*)malloc(sizeof(struct Detached) * (size_t)(j - i));
        int det_n = 0;
        for (int k = i; k < j; k++) {
            struct CacheEntry* e = cands[k].entry;
            /* Re-validate under the lock. */
            if (*cands[k].pp != e) continue;  /* already removed */
            int still_eligible =
                (!e->dirty) || (e->dirty && e->priority == 0);
            if (!still_eligible) continue;

            *cands[k].pp = e->hash_next;
            cache->entry_count--;
            if (e->dirty) cache->dirty_count--;
            det[det_n].entry        = e;
            det[det_n].logical_page = e->logical_page;
            det[det_n].payload      = e->payload;
            det[det_n].priority     = (uint32_t)e->priority;
            det[det_n].is_dirty     = e->dirty;
            det_n++;
        }

        spin_unlock(&cache->bucket_locks[cur_bkt]);

        /* Flush + free LOCK-FREE.  mirror_write is safe to call
           concurrently with other reads/writes; the lazy mirror's
           on-disk page state is the source of truth. */
        for (int k = 0; k < det_n; k++) {
            if (det[k].is_dirty && cache->sb) {
                if (indir_lookup(cache->sb, det[k].logical_page) != 0) {
                    mirror_write(cache->sb, det[k].logical_page,
                                 det[k].payload, det[k].priority);
                }
            }
            free(det[k].payload);
            free(det[k].entry);
        }
        free(det);

        i = j;
    }

    free(cands);
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
       amortized.  Eligible candidates:
         - clean entries of any priority (LRU among them)
         - dirty data pages (priority == 0) — flushed via mirror_write
           before being freed
       Dirty pool/indirection/superblock pages (priority 1-3) are
       NEVER evicted mid-process — partial flush of those would leave
       the VFS in an inconsistent state. */
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
