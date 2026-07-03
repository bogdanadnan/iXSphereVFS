#ifndef VFS_TOUCHED_H
#define VFS_TOUCHED_H

#include "ixsphere/vfs_internal.h"
#include "nodes.h"
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * TouchedFile — tracks files modified in a given epoch for commit conflict
 * detection.  Each entry records {epoch, nodeId}.  The chain is rooted at
 * ctx->touchedFilesPtr.  Entries are deduplicated: at most one entry per
 * (epoch, nodeId) pair.
 * --------------------------------------------------------------------------- */

/* Record that a file was modified in a given epoch.
 * Walks the chain for dedup — if an entry for (epoch, nodeId) already
 * exists, returns VFS_OK without allocating (idempotent).
 * Otherwise allocates a pool slot, writes TouchedFile, CAS-prepends.
 * Returns VFS_OK or a negative error code. */
int touchedfile_add(Pool* pool, int64_t* touchedFilesPtr,
                    uint32_t epoch, uint32_t nodeId);

/* Collect all nodeIds for a given epoch into an output array.
 * Walks the chain, filters by epoch, deduplicates by nodeId.
 * Returns the number of distinct nodeIds written (up to max_count).
 * Stores nodeIds in the provided uint32_t array. */
int touchedfile_collect(Pool* pool, int64_t touchedFilesPtr,
                        uint32_t epoch, uint32_t* out_nodeIds, int max_count);

/* Mark all entries for a given epoch as reclaimable by setting their
 * epoch to EPOCH_RECLAIMED (0xFFFFFFFF), or simply leave them for GC.
 * Current implementation: no-op — entries are reclaimed by GC during
 * the next garbage collection cycle.  Called after commit or soft-delete. */
void touchedfile_drop(Pool* pool, int64_t* touchedFilesPtr, uint32_t epoch);

#endif /* VFS_TOUCHED_H */
