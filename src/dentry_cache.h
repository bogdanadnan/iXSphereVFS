#ifndef VFS_DENTRY_CACHE_H
#define VFS_DENTRY_CACHE_H

#include "ixsphere/vfs.h"
#include "pool.h"
#include "mapper.h"
#include <stdbool.h>

/* Maximum entries per directory in the dentry cache */
#define DENTRY_CACHE_MAX    1024

/* ---------------------------------------------------------------------------
 * DentryEntry — one cached directory entry
 * --------------------------------------------------------------------------- */

typedef struct {
    int64_t childNodeId;       /* child node identifier */
    int64_t childPtr;          /* VirtualPtr to child DirNode or FileNode */
    char    name[256];         /* entry name */
    bool    isDir;             /* true if child is a directory */
} DentryEntry;

/* ---------------------------------------------------------------------------
 * DentryCache — per-directory cache, invalidated on headPtr change
 * --------------------------------------------------------------------------- */

typedef struct {
    bool        valid;               /* false = must rebuild on next readdir */
    int64_t     last_headPtr_page;   /* VFS_VPTR_PAGE(headPtr) when cached */
    DentryEntry entries[DENTRY_CACHE_MAX];
    int         count;               /* number of valid entries in the cache */
} DentryCache;

/* Build or rebuild the dentry cache for a directory.
 *
 * Walks the DirNode's headPtr chain, applies the read-rule
 * (epoch-based dedup by childNodeId), resolves names, and
 * populates arr->entries.  Sets arr->valid = true and records
 * VFS_VPTR_PAGE(headPtr) in arr->last_headPtr_page.
 *
 * Returns VFS_OK on success, VFS_ERR_NOMEM if too many entries.
 *
 * pool     — pool allocator for resolving pool slots
 * root_vp  — VirtualPtr to the DirNode
 * epoch    — query epoch (for read-rule dedup)
 * arr      — cache array to populate
 */
int dentry_cache_build(Pool* pool, Mapper* mapper, int64_t root_vp, int64_t epoch,
                       DentryCache* arr);

/* Check whether the cache is still valid for a given headPtr.
 * Returns true if arr->valid and VFS_VPTR_PAGE(headPtr) == arr->last_headPtr_page. */
bool dentry_cache_is_valid(DentryCache* arr, int64_t headPtr);

/* Invalidate the cache.  Called on any create/delete/rename in the directory. */
void dentry_cache_invalidate(DentryCache* arr);

#endif /* VFS_DENTRY_CACHE_H */
