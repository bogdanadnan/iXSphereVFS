/* fuse_dir_cache.h — small FUSE-side directory listing cache
 *
 * Phase 20 prep: caches the full directory listing (as returned by
 * vfs_readdir_alloc) so that cursor-based readdir can serve large
 * directories without re-walking the chain for every FUSE callback.
 *
 * The cache is keyed by path hash (FNV-1a 64-bit) and stores up to
 * N slots in an LRU ring.  Each slot holds:
 *   - path_hash    : cache key
 *   - dir_vp       : resolved directory VirtualPtr (for verification)
 *   - entries      : heap-allocated vfs_dirent_t[] from vfs_readdir_alloc
 *   - count        : number of valid entries
 *   - last_used    : monotonic counter for LRU eviction
 *
 * Concurrency: protected by a single mutex.  Reads and writes are
 * fast — the cache is small (FUSEDIR_CACHE_SIZE = 32), so even the
 * critical section is short.
 */

#ifndef FUSE_DIR_CACHE_H
#define FUSE_DIR_CACHE_H

#include "ixsphere/vfs.h"
#include <stdint.h>
#include <pthread.h>

#define FUSEDIR_CACHE_SIZE 32

/* ---------------------------------------------------------------------------
 * Cache keying
 *
 * libfuse may call readdir with:
 *   - the directory's full path (high-level, no opendir fd yet), or
 *   - NULL path but a valid fi->fh from opendir (low-level, fd-keyed).
 *
 * The same directory can be referenced both ways during one open.  We
 * store BOTH the path_hash and the fi->fh value in each slot, and
 * lookups match either.  Invalidation needs only one signal (path
 * or fh) to evict.
 * --------------------------------------------------------------------------- */

typedef struct {
    uint64_t path_hash;       /* 0 = empty slot (path_hash is non-zero for any real path) */
    int64_t  fh;              /* 0 = no fh known for this dir; otherwise fi->fh from opendir */
    int64_t  dir_vp;          /* resolved VirtualPtr (for verification) */
    vfs_dirent_t* entries;     /* owned by the slot; freed via vfs_free_dirents */
    int count;
    uint64_t last_used;
} FusedirCacheSlot;

typedef struct {
    FusedirCacheSlot slots[FUSEDIR_CACHE_SIZE];
    uint64_t lru_counter;
    pthread_mutex_t lock;
} FusedirCache;

/* Initialize the cache.  Must be called once before any readdir
   callback.  Safe to call again to reset. */
void fusedir_cache_init(FusedirCache* cache);

/* Free all cached entries and the mutex.  Call at shutdown. */
void fusedir_cache_destroy(FusedirCache* cache);

/* Look up a directory by path.  If found and dir_vp matches the
   resolved directory, returns the cached entries (caller may read
   them without holding the lock).  If not found or dir_vp mismatches,
   rebuilds via vfs_readdir_alloc, evicting an LRU slot if needed.
   Returns VFS_OK on success; on error *out_entries = NULL,
   *out_count = 0.  The returned buffer is owned by the cache; do not
   free. */
int fusedir_cache_get(FusedirCache* cache,
                      const char* path,
                      int64_t dir_vp,
                      vfs_t* vfs,
                      int64_t epoch,
                      vfs_dirent_t** out_entries,
                      int* out_count);

/* Look up a directory by fi->fh value (post-opendir readdir).
   Behaves like fusedir_cache_get but uses the fh value as the key
   instead of the path.  dir_vp is the resolved directory pointer
   (carried in fi->fh).  Returns VFS_OK on success. */
int fusedir_cache_get_fh(FusedirCache* cache,
                         int64_t fh,
                         int64_t dir_vp,
                         vfs_t* vfs,
                         int64_t epoch,
                         vfs_dirent_t** out_entries,
                         int* out_count);

/* FNV-1a 64-bit hash of a NUL-terminated path.  Exposed for the
   readdir callback which hashes the incoming path string. */
uint64_t fusedir_hash_path(const char* path);

/* Invalidate a single cache entry by path.  Called on create/mkdir/
   unlink/rmdir/rename so the next readdir rebuilds from scratch.
   Safe to call on paths not in the cache (no-op). */
void fusedir_cache_invalidate_path(FusedirCache* cache, const char* path);

/* Invalidate a single cache entry by fh value.  Called when the
   opendir fd is closed if we want to free the listing eagerly; most
   callers don't need this (LRU evicts eventually). */
void fusedir_cache_invalidate_fh(FusedirCache* cache, int64_t fh);

/* Invalidate by resolved directory VirtualPtr (the most reliable
   signal).  Called from create/mkdir/unlink/rmdir/rename where we
   have the parent_vp from resolve_full_path but not a canonical
   path matching what FUSE sees. */
void fusedir_cache_invalidate_vp(FusedirCache* cache, int64_t vp);

/* Invalidate all cache entries whose path starts with the given
   prefix.  Called when a rename/move changes a parent path.  The
   prefix should end with '/' for directory invalidation (so "/foo"
   doesn't match "/foobar"). */
void fusedir_cache_invalidate_prefix(FusedirCache* cache, const char* prefix);

#endif /* FUSE_DIR_CACHE_H */