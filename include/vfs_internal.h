#ifndef VFS_INTERNAL_H
#define VFS_INTERNAL_H

#include "ixsphere_vfs.h"
#include "storage.h"
#include "pool.h"

#define VFS_PAGE_SIZE 8192

/* ---------------------------------------------------------------------------
 * TreeContext — runtime state for one VFS instance
 *
 * Holds the StorageBackend, Pool allocator, superblock fields, and
 * configuration needed by all tree operations (Phase 5+).
 * --------------------------------------------------------------------------- */

typedef struct {
    StorageBackend* sb;          /* page-level I/O */
    Pool            pool;        /* pool allocator for metadata entries */

    /* Superblock fields (cached in memory, written to page 1 on commit/GC) */
    int64_t  rootNodeOffset;     /* VirtualPtr to root DirNode */
    int64_t  currentEpoch;       /* latest epoch counter (even = live head) */
    int64_t  epochMapperPtr;     /* VirtualPtr to first MapperEntry, 0 = none */
    int64_t  touchedFilesPtr;    /* VirtualPtr to first TouchedFile entry */
    uint32_t nextNodeId;         /* next available node identifier */
    int64_t  treeLockState;      /* bit 63 = GC exclusive lock, bits 32-62 = reader count */

    /* Pool list head — lives here so pool.list_head points into TreeContext */
    int64_t  pool_list_head_value;

    /* Configuration (from StorageBackend header) */
    uint32_t segment_size;       /* pages per FileContent segment */
} TreeContext;

struct vfs_t {
    TreeContext* ctx;            /* heap-allocated tree context */
};

#endif /* VFS_INTERNAL_H */
