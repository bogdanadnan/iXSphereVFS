/*
 * include/vfs_internal.h — iXSphereVFS Internal Structures
 *
 * Declares structures shared across source files. Will be populated
 * during Phase 2 (StorageBackend) and Phase 4 (Node Types).
 */
#ifndef VFS_INTERNAL_H
#define VFS_INTERNAL_H

#include "ixsphere_vfs.h"

/* ── Page constants ────────────────────────────────────── */

#define VFS_PAGE_SIZE       8192

/* ── Forward declarations ──────────────────────────────── */

/* Filled in by Phase 2 */
struct vfs_t {
    int placeholder;  /* REMOVE when real fields are added */
};

#endif /* VFS_INTERNAL_H */
