#ifndef VFS_MAPPER_H
#define VFS_MAPPER_H

#include "ixsphere_vfs.h"
#include "pool.h"
#include "nodes.h"
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * Mapper — epoch mapping state for one VFS instance
 *
 * Manages the epoch mapper chain rooted at superblock->epochMapperPtr.
 * All operations use pool_alloc/pool_resolve via the VFS context's pool.
 * --------------------------------------------------------------------------- */

typedef struct {
    Pool*        pool;              /* pool allocator for mapper entries */
    int64_t*     epochMapperPtr;    /* pointer to the chain head in TreeContext */
} Mapper;

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
