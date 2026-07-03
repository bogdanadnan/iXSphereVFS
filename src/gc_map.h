#ifndef VFS_GC_MAP_H
#define VFS_GC_MAP_H

#include "vfs_internal.h"
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * GC VirtualPtr map — open-addressing hash table for old_vp → new_vp remapping
 *
 * During shadow-compaction, pool entries are copied to new pool pages.
 * This map records the mapping from old VirtualPtr to new VirtualPtr so
 * that pointers within entries can be updated after compaction.
 *
 * The table uses open addressing with linear probing.  A sentinel value
 * indicates an empty slot (VFS_VPTR_NULL = 0 is used for "no mapping",
 * but VirtualPtr 0 is also a valid pointer — however no pool slot has
 * VirtualPtr 0, so this is safe).
 * --------------------------------------------------------------------------- */

typedef struct {
    int64_t old_vp;      /* old VirtualPtr (key) — 0 means empty slot */
    int64_t new_vp;      /* new VirtualPtr (value) */
} GCMapEntry;

typedef struct {
    GCMapEntry* entries; /* heap-allocated array */
    int         count;   /* number of active entries */
    int         capacity; /* allocated capacity (power of 2) */
} GCMap;

/* Initialize a GCMap with the given initial capacity (rounded up to power of 2).
 * Allocates entries array via calloc.  Returns VFS_OK on success. */
int gc_map_init(GCMap* map, int initial_capacity);

/* Insert or update a mapping.  Returns VFS_OK on success.
 * If the table is too full (> 75% load), it grows automatically. */
int gc_map_put(GCMap* map, int64_t old_vp, int64_t new_vp);

/* Look up a mapping.  Returns the new_vp if found, or old_vp (identity) if not. */
int64_t gc_map_get(GCMap* map, int64_t old_vp);

/* Remove a mapping.  Returns true if the entry was found and removed. */
bool gc_map_remove(GCMap* map, int64_t old_vp);

/* Free all memory used by the map. */
void gc_map_destroy(GCMap* map);

#endif /* VFS_GC_MAP_H */
