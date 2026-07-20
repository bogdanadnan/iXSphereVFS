/* Phase 28 Type 2: rename-tombstone bin job (work in progress).
 *
 * Spec: impl/phase-28-bin-job-rename-tombstone.md
 *
 * This file is the stub created in W1 (constants + dispatch).
 * The real analysis handler (gc_handle_rename_done) and work
 * handler (gc_handle_remove_tombstone) are implemented in W2 and W3
 * respectively.  Until then, both return VFS_OK as a no-op so the
 * dispatch doesn't crash.
 *
 * The Bin's BIN_TRIGGER_TOMBSTONE_ADDED / BIN_WORK_REMOVE_TOMBSTONE
 * types are added in src/bin.h (W1), and the dispatch cases are
 * added in src/gc.c (W1).  When W2 lands, the no-op stubs here are
 * replaced with the real implementations.
 */
#include "gc.h"
#include "bin.h"
#include <stdio.h>

int gc_handle_rename_done(vfs_t* vfs, const BinEntry* entry) {
    (void)vfs;
    (void)entry;
    /* W2 stub: real analysis handler lands in W2.  For now, this
       is a no-op so the dispatch is exercised but the rename
       doesn't free anything (the original NOOP-placeholder behavior). */
    return VFS_OK;
}

int gc_handle_remove_tombstone(vfs_t* vfs, const BinEntry* entry) {
    (void)vfs;
    (void)entry;
    /* W3 stub: real work handler lands in W3.  For now, this is
       a no-op (the work entry, if any, is consumed but no pages
       are freed). */
    return VFS_OK;
}
