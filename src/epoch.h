#ifndef VFS_EPOCH_H
#define VFS_EPOCH_H

#include <stdint.h>
#include <stdbool.h>
#include "mapper.h"
#include "vfs_internal.h"

/* ---------------------------------------------------------------------------
 * Phase 6: Epoch system — replaces Phase 5a stubs.
 *
 * mapper_resolve is now defined in mapper.h (takes Mapper*).
 * The stub in epoch.c has been replaced with delegation to mapper.c.
 * --------------------------------------------------------------------------- */

/* Check whether an epoch is writable.
   Phase 6: returns true only for current live head and active snapshots. */
bool vfs_epoch_is_writable(TreeContext* ctx, int64_t epoch);

/* Record that a file was touched in a given epoch (for commit conflict detection).
   Current stub: no-op.
   Phase 6: CAS-prepends a TouchedFile entry to superblock->touchedFilesPtr. */
void touchedfile_add(void* vfs, int64_t epoch, uint32_t nodeId);

/* Test helper: override vfs_epoch_is_writable return value.
   Pass non-zero for writable (default), zero for frozen. */
void test_set_epoch_writable(int writable);

#endif /* VFS_EPOCH_H */
