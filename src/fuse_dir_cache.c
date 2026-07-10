/* fuse_dir_cache.c — implementation of the FUSE dir cache.
 *
 * Each slot holds:
 *   - path_hash : FNV-1a 64-bit hash of the directory's path, or 0
 *                 if the directory was only seen via fh-keyed access.
 *   - fh        : fi->fh value from opendir, or 0 if only path-keyed.
 *   - dir_vp    : resolved VirtualPtr (for staleness check).
 *
 * Lookups match EITHER path_hash OR fh.  This way the same directory
 * is cached once, regardless of which key libfuse uses.
 *
 * Invalidation by path or fh finds the slot and clears it.  Since
 * both keys live in the same slot, one signal evicts the entry.
 *
 * Eviction is LRU based on a monotonic counter incremented on every
 * successful lookup.  Cache size is FUSEDIR_CACHE_SIZE (32).
 */

#include "fuse_dir_cache.h"
#include <stdlib.h>
#include <string.h>

uint64_t fusedir_hash_path(const char* path) {
    /* FNV-1a 64-bit.  Not cryptographic — just needs to spread
       paths across the cache for O(1)-ish slot lookup. */
    uint64_t h = 14695981039346656037ULL;  /* offset basis */
    for (const unsigned char* p = (const unsigned char*)path; *p; p++) {
        h ^= (uint64_t)*p;
        h *= 1099511628211ULL;  /* FNV prime */
    }
    return h;
}

void fusedir_cache_init(FusedirCache* cache) {
    if (!cache) return;
    memset(cache, 0, sizeof(*cache));
    pthread_mutex_init(&cache->lock, NULL);
    cache->lru_counter = 0;
}

void fusedir_cache_destroy(FusedirCache* cache) {
    if (!cache) return;
    pthread_mutex_lock(&cache->lock);
    for (int i = 0; i < FUSEDIR_CACHE_SIZE; i++) {
        if (cache->slots[i].entries) {
            vfs_free_dirents(cache->slots[i].entries);
            cache->slots[i].entries = NULL;
        }
    }
    pthread_mutex_unlock(&cache->lock);
    pthread_mutex_destroy(&cache->lock);
}

/* Clear a slot's data without freeing its entries pointer (caller
   decides whether to free first). */
static void clear_slot(FusedirCacheSlot* slot) {
    if (slot->entries) vfs_free_dirents(slot->entries);
    memset(slot, 0, sizeof(*slot));
}

/* Find a slot matching path_hash.  Caller holds the lock. */
static FusedirCacheSlot* find_slot_by_path(FusedirCache* cache, uint64_t path_hash) {
    if (path_hash == 0) return NULL;
    for (int i = 0; i < FUSEDIR_CACHE_SIZE; i++) {
        if (cache->slots[i].path_hash == path_hash &&
            cache->slots[i].entries != NULL) {
            return &cache->slots[i];
        }
    }
    return NULL;
}

/* Find a slot matching fh.  Caller holds the lock. */
static FusedirCacheSlot* find_slot_by_fh(FusedirCache* cache, int64_t fh) {
    if (fh == 0) return NULL;
    for (int i = 0; i < FUSEDIR_CACHE_SIZE; i++) {
        if (cache->slots[i].fh == fh &&
            cache->slots[i].entries != NULL) {
            return &cache->slots[i];
        }
    }
    return NULL;
}

/* Same as above but matches even on empty entries (for invalidation). */
static FusedirCacheSlot* find_slot_by_path_any(FusedirCache* cache, uint64_t path_hash) {
    if (path_hash == 0) return NULL;
    for (int i = 0; i < FUSEDIR_CACHE_SIZE; i++) {
        if (cache->slots[i].path_hash == path_hash) {
            return &cache->slots[i];
        }
    }
    return NULL;
}

static FusedirCacheSlot* find_slot_by_fh_any(FusedirCache* cache, int64_t fh) {
    if (fh == 0) return NULL;
    for (int i = 0; i < FUSEDIR_CACHE_SIZE; i++) {
        if (cache->slots[i].fh == fh) {
            return &cache->slots[i];
        }
    }
    return NULL;
}

/* Find a slot whose (path_hash, fh) match EITHER of the given values. */
static FusedirCacheSlot* find_slot_either(FusedirCache* cache,
                                          uint64_t path_hash,
                                          int64_t fh) {
    for (int i = 0; i < FUSEDIR_CACHE_SIZE; i++) {
        FusedirCacheSlot* s = &cache->slots[i];
        if (s->entries == NULL) continue;
        if ((path_hash != 0 && s->path_hash == path_hash) ||
            (fh != 0 && s->fh == fh)) {
            return s;
        }
    }
    return NULL;
}

/* Find an empty slot or the LRU slot to evict.  Returns the slot,
   already cleared.  Caller must hold the lock. */
static FusedirCacheSlot* acquire_slot(FusedirCache* cache) {
    /* First pass: empty slot */
    for (int i = 0; i < FUSEDIR_CACHE_SIZE; i++) {
        if (cache->slots[i].entries == NULL) {
            return &cache->slots[i];
        }
    }
    /* All slots occupied: pick the one with smallest last_used. */
    int oldest = 0;
    for (int i = 1; i < FUSEDIR_CACHE_SIZE; i++) {
        if (cache->slots[i].last_used < cache->slots[oldest].last_used)
            oldest = i;
    }
    FusedirCacheSlot* slot = &cache->slots[oldest];
    clear_slot(slot);
    return slot;
}

/* Build a cache slot.  Acquires a free slot (evicting LRU if full),
   populates path_hash/fh/dir_vp, and calls vfs_readdir_alloc to fill
   in the entries.  Returns the slot on success, NULL on error.  Caller
   holds the lock. */
static FusedirCacheSlot* build_slot(FusedirCache* cache,
                                   uint64_t path_hash,
                                   int64_t fh,
                                   int64_t dir_vp,
                                   vfs_t* vfs,
                                   int64_t epoch) {
    FusedirCacheSlot* slot = acquire_slot(cache);
    slot->path_hash = path_hash;
    slot->fh        = fh;
    slot->dir_vp    = dir_vp;

    vfs_dirent_t* entries = NULL;
    int count = 0;
    int rc = vfs_readdir_alloc(vfs, dir_vp, &entries, &count, epoch);
    if (rc != VFS_OK) {
        slot->path_hash = 0;
        slot->fh = 0;
        slot->dir_vp = 0;
        return NULL;
    }
    slot->entries = entries;
    slot->count = count;
    return slot;
}

/* Common logic for get_by_path / get_by_fh: tries hit, falls back to
   rebuild.  Sets *out_entries / *out_count. */
static int get_internal(FusedirCache* cache,
                        uint64_t path_hash,
                        int64_t fh,
                        int64_t dir_vp,
                        vfs_t* vfs,
                        int64_t epoch,
                        vfs_dirent_t** out_entries,
                        int* out_count) {
    if (!cache || !vfs || !out_entries || !out_count) return VFS_ERR_IO;
    *out_entries = NULL;
    *out_count = 0;

    pthread_mutex_lock(&cache->lock);

    /* Try hit: match by either path_hash or fh. */
    FusedirCacheSlot* slot = find_slot_either(cache, path_hash, fh);
    if (slot && slot->dir_vp == dir_vp) {
        slot->last_used = ++cache->lru_counter;
        /* Promote: if hit by path_hash but slot has no path_hash,
           fill it in (and vice versa for fh).  This way subsequent
           lookups using either key hit the same slot. */
        if (path_hash != 0 && slot->path_hash == 0) slot->path_hash = path_hash;
        if (fh != 0 && slot->fh == 0) slot->fh = fh;
        vfs_dirent_t* entries = slot->entries;
        int count = slot->count;
        pthread_mutex_unlock(&cache->lock);
        *out_entries = entries;
        *out_count = count;
        return VFS_OK;
    }

    /* If we hit a slot but dir_vp differs, the underlying dir was
       deleted and re-created at the same path/fh.  Evict and rebuild. */
    if (slot) {
        clear_slot(slot);
    }

    /* Build a new entry.  Note: rebuild uses only one of the keys
       (the one that triggered the lookup).  The other key gets
       filled in on the next hit by the promotion logic above. */
    if (path_hash != 0) {
        slot = build_slot(cache, path_hash, 0, dir_vp, vfs, epoch);
    } else {
        slot = build_slot(cache, 0, fh, dir_vp, vfs, epoch);
    }
    if (!slot) {
        pthread_mutex_unlock(&cache->lock);
        return VFS_ERR_IO;
    }

    vfs_dirent_t* entries = slot->entries;
    int count = slot->count;
    pthread_mutex_unlock(&cache->lock);
    *out_entries = entries;
    *out_count = count;
    return VFS_OK;
}

int fusedir_cache_get(FusedirCache* cache,
                      const char* path,
                      int64_t dir_vp,
                      vfs_t* vfs,
                      int64_t epoch,
                      vfs_dirent_t** out_entries,
                      int* out_count) {
    uint64_t path_hash = path ? fusedir_hash_path(path) : 0;
    /* If we know the dir_vp from a previous readdir, we may have an fh
       recorded too.  For path-keyed lookups, leave fh=0 (the get
       path will find it via path_hash and promote). */
    return get_internal(cache, path_hash, 0, dir_vp, vfs, epoch,
                        out_entries, out_count);
}

int fusedir_cache_get_fh(FusedirCache* cache,
                         int64_t fh,
                         int64_t dir_vp,
                         vfs_t* vfs,
                         int64_t epoch,
                         vfs_dirent_t** out_entries,
                         int* out_count) {
    return get_internal(cache, 0, fh, dir_vp, vfs, epoch,
                        out_entries, out_count);
}

void fusedir_cache_invalidate_path(FusedirCache* cache, const char* path) {
    if (!cache || !path) return;
    uint64_t h = fusedir_hash_path(path);
    pthread_mutex_lock(&cache->lock);
    FusedirCacheSlot* slot = find_slot_by_path_any(cache, h);
    if (slot) {
        /* Clear entries but preserve fh key so the slot can still be
           looked up by fh if the same fd is used after the directory
           was rebuilt.  Actually no — the entries are stale, so the
           slot must be cleared entirely.  Subsequent lookup will
           rebuild. */
        clear_slot(slot);
    }
    pthread_mutex_unlock(&cache->lock);
}

void fusedir_cache_invalidate_fh(FusedirCache* cache, int64_t fh) {
    if (!cache || fh == 0) return;
    pthread_mutex_lock(&cache->lock);
    FusedirCacheSlot* slot = find_slot_by_fh_any(cache, fh);
    if (slot) clear_slot(slot);
    pthread_mutex_unlock(&cache->lock);
}

/* Invalidate all cache entries whose path starts with the given
   prefix.  Without storing the original path, the only correct action
   is a full flush.  Acceptable given the 32-slot cap. */
void fusedir_cache_invalidate_prefix(FusedirCache* cache, const char* prefix) {
    if (!cache || !prefix) return;
    (void)prefix;
    pthread_mutex_lock(&cache->lock);
    for (int i = 0; i < FUSEDIR_CACHE_SIZE; i++) {
        if (cache->slots[i].entries) {
            vfs_free_dirents(cache->slots[i].entries);
        }
        memset(&cache->slots[i], 0, sizeof(FusedirCacheSlot));
    }
    pthread_mutex_unlock(&cache->lock);
}