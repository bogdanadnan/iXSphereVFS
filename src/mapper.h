#ifndef VFS_MAPPER_H
#define VFS_MAPPER_H

#include "ixsphere/vfs.h"
#include "pool.h"
#include "nodes.h"
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * Mapper — epoch mapping state for one VFS instance
 *
 * Manages the epoch mapper chain rooted at superblock->epochMapperPtr.
 * All operations use pool_alloc / pool_acquire / pool_release via the
 * VFS context's pool.
 * --------------------------------------------------------------------------- */

typedef struct {
    Pool*        pool;              /* pool allocator for mapper entries */
    int64_t*     epochMapperPtr;    /* pointer to the chain head in TreeContext */
} Mapper;

/* ---------------------------------------------------------------------------
 * MapperEntry (in-memory cache row, distinct from 32-byte pool slot)
 * --------------------------------------------------------------------------- */

typedef struct {
    uint32_t fromEpoch;
    uint32_t toEpoch;
    bool     traversalApply;
} MapperEntryRow;

/* ---------------------------------------------------------------------------
 * MapperTable — in-memory snapshot of the epoch mapper chain.
 * Used for query-heavy workloads that don't need pool-based iteration.
 * --------------------------------------------------------------------------- */

typedef struct {
    MapperEntryRow* entries;        /* dynamic array of cached entries */
    int             count;          /* number of live entries */
    int             capacity;       /* allocated capacity */
    int64_t*        epochMapperPtr; /* pointer to the chain head in TreeContext */
    Pool*           pool;           /* pool allocator */
} MapperTable;

#define MAPPER_TABLE_INITIAL_CAPACITY  8

/* Initialize a MapperTable (allocates initial entries array). Returns VFS_OK or VFS_ERR_NOMEM. */
int  mapper_table_init(MapperTable* tbl, Pool* pool, int64_t* epochMapperPtr);

/* Destroy a MapperTable (frees entries array). */
void mapper_table_destroy(MapperTable* tbl);

/* Rebuild the table by clearing entries and re-walking the pool chain.
 * Called after GC compaction to refresh the in-memory cache. Returns count, or negative error. */
int  mapper_table_rebuild(MapperTable* tbl);

/* Resolve an epoch through the table (linear scan). Returns toEpoch or epoch itself if not found. */
int64_t mapper_table_resolve(MapperTable* tbl, int64_t epoch);

/* Check whether traversalApply is set for a given epoch in the table.
 * Linear scan of entries[] for fromEpoch match. Returns the flag or false. */
bool mapper_table_traversal_apply(MapperTable* tbl, int64_t epoch);

/* Insert a new mapping: writes to pool chain first (via mapper_insert),
 * then appends to in-memory entries[] with release barrier. */
int  mapper_table_insert(MapperTable* tbl, uint32_t fromEpoch, uint32_t toEpoch, bool traversalApply);

/* Append to in-memory entries[] only (no pool write). */
int  mapper_table_append(MapperTable* tbl, uint32_t fromEpoch, uint32_t toEpoch, bool traversalApply);

/* Initialize a Mapper handle.
 * pool — the VFS instance's pool allocator
 * epochMapperPtr — pointer to the int64_t field (e.g., &ctx->epochMapperPtr) */
void mapper_init(Mapper* m, Pool* pool, int64_t* epochMapperPtr);

/* Insert a new mapping {fromEpoch, toEpoch, flags}.
 * Walks the chain to enforce the single-hop invariant:
 *   - If fromEpoch already exists as a from in a live entry → VFS_ERR_EXISTS
 *   - If toEpoch already exists as a to in a live entry → VFS_ERR_EXISTS
 *   - If toEpoch already exists as a from in a live entry → VFS_ERR_EXISTS
 *   - If fromEpoch already exists as a to in a live entry → VFS_ERR_EXISTS
 * Allocates a pool slot, writes MapperEntry, CAS-prepends to chain.
 * Returns VFS_OK on success, or a negative error code. */
int mapper_insert(Mapper* m, uint32_t fromEpoch, uint32_t toEpoch,
                  uint16_t flags);

/* Resolve an epoch through the mapper chain.
 * Walks the MapperEntry chain from *epochMapperPtr.
 * Returns toEpoch if a mapping for epoch exists, or epoch itself if not. */
int64_t mapper_resolve(Mapper* m, int64_t epoch);

/* Check whether traversalApply is set for a given epoch.
 * Returns true if a MapperEntry for epoch exists with flags & 1.
 * Returns false if no mapping exists or if flags bit 0 is clear. */
bool mapper_traversal_apply(Mapper* m, int64_t epoch);

/* Walk the entire mapper chain, validating reachability of all entries.
 * Returns the number of entries walked, or a negative error code. */
int mapper_validate(Mapper* m);

#endif /* VFS_MAPPER_H */
