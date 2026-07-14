#ifndef VFS_EPOCH_H
#define VFS_EPOCH_H

#include <stdint.h>
#include <stdbool.h>
#include "mapper.h"
#include "ixsphere/vfs_internal.h"

/* ---------------------------------------------------------------------------
 * Phase 6: Epoch system — replaces Phase 5a stubs.
 *
 * mapper_resolve is now defined in mapper.h (takes Mapper*).
 * The stub in epoch.c has been replaced with delegation to mapper.c.
 * --------------------------------------------------------------------------- */

/* Check whether an epoch is writable.
   - Live head (currentEpoch) is always writable.
   - Odd snapshot epoch, not in the mapper chain (i.e., active snapshot)
     is writable.
   - Committed or soft-deleted snapshots (in mapper) are not writable.
   - Other even epochs (past even, not current) are not writable. */
bool vfs_epoch_is_writable(TreeContext* ctx, int64_t epoch);

/* Create a snapshot by advancing the epoch counter by 2.
   Returns the snapshot epoch (always odd), or -1 on error.
   Zero I/O — no superblock flush. */
int64_t vfs_snapshot(vfs_t* vfs);

#endif /* VFS_EPOCH_H */
