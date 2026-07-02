#ifndef VFS_EPOCH_H
#define VFS_EPOCH_H

#include <stdint.h>
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * Phase 5a: Epoch stubs — replaced with real implementations in Phase 6.
 *
 * These stubs allow Phase 5 (Tree Operations) to be built and tested
 * independently of Phase 6 (Epoch System).
 * --------------------------------------------------------------------------- */

/* Resolve an epoch through the mapper chain.
   Current stub: identity mapping (no-op).
   Phase 6: walks the MapperEntry chain from superblock->epochMapperPtr. */
int64_t mapper_resolve(void* mapper, int64_t epoch);

/* Check whether an epoch is writable.
   Current stub: all epochs writable.
   Phase 6: returns true only for current live head and active snapshots. */
bool vfs_epoch_is_writable(void* sb, int64_t epoch, void* mapper);

/* Record that a file was touched in a given epoch (for commit conflict detection).
   Current stub: no-op.
   Phase 6: CAS-prepends a TouchedFile entry to superblock->touchedFilesPtr. */
void touchedfile_add(void* vfs, int64_t epoch, uint32_t nodeId);

#endif /* VFS_EPOCH_H */
