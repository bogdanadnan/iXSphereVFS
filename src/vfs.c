/*
 * src/vfs.c — iXSphereVFS entry point (Phase 1 stub)
 *
 * This is a minimal stub that compiles and links. Replace with
 * implementation as you progress through the phases.
 */
#include "ixsphere_vfs.h"
#include <stdlib.h>
#include <string.h>

/* ── Error strings ─────────────────────────────────────── */

const char* vfs_error_string(vfs_error_t err) {
    switch (err) {
        case VFS_OK:            return "OK";
        case VFS_ERR_IO:        return "I/O error";
        case VFS_ERR_NOTFOUND:  return "Not found";
        case VFS_ERR_EXISTS:    return "Already exists";
        case VFS_ERR_NOTDIR:    return "Not a directory";
        case VFS_ERR_NOTEMPTY:  return "Directory not empty";
        case VFS_ERR_CONFLICT:  return "Conflict";
        case VFS_ERR_FULL:      return "No space left";
        case VFS_ERR_NOMEM:     return "Out of memory";
        default:                return "Unknown error";
    }
}

/* ── Stub: vfs_open ────────────────────────────────────── */

vfs_t* vfs_open(const char* path) {
    (void)path;
    /* TODO: Phase 2 (StorageBackend) + Phase 5 (Tree Operations) */
    return NULL;
}

/* ── Stub: vfs_close ───────────────────────────────────── */

void vfs_close(vfs_t* vfs) {
    if (vfs) {
        /* TODO: Phase 2 + Phase 5 */
    }
}
