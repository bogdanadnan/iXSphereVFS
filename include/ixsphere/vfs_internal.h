#ifndef IXSPHERE_VFS_INTERNAL_H
#define IXSPHERE_VFS_INTERNAL_H

#include "ixsphere/vfs.h"
#include "storage.h"
#include "pool.h"
#include "page_array.h"
#include "mapper.h"

typedef struct SegmentArray SegmentArray;

/* ---------------------------------------------------------------------------
 * TreeContext — runtime state for one VFS instance
 * --------------------------------------------------------------------------- */

typedef struct {
    StorageBackend* sb;          /* page-level I/O */
    Pool            pool;        /* pool allocator for metadata entries */

    /* Superblock fields (cached in memory, written to page 1 on commit/GC) */
    int64_t  rootNodeOffset;     /* VirtualPtr to root DirNode */
    int64_t  currentEpoch;       /* latest epoch counter (even = live head) */
    int64_t  epochMapperPtr;     /* VirtualPtr to first MapperEntry, 0 = none */
    uint32_t nextNodeId;         /* next available node identifier */
    int64_t  treeLockState;      /* bit 63 = GC exclusive lock, bits 32-62 = reader count */
    int64_t  gc_generation;       /* incremented after each GC compaction, atomically loaded */

    /* Pool list head — lives here so pool.list_head points into TreeContext */
    int64_t  pool_list_head_value;

    /* Configuration (from StorageBackend header) */
    uint32_t segment_size;       /* pages per FileContent segment */
    int64_t  page_size;          /* VFS page size (from sb->page_size) */

    /* In-memory page array cache — one entry for the most recently accessed segment */

    /* Epoch mapper */
    Mapper mapper;

    /* In-memory mapper table snapshot for query-heavy workloads */
    MapperTable mapper_table;

    /* Last error code — set by operations, reset to VFS_OK on success */
    vfs_error_t last_error;

    /* W6: chain-walk tcache — per-`vfs_t` (replaces the pre-W6
     * `__thread` static).  The pre-W6 `__thread` static had a
     * cross-VFS collision risk (the key `file_vp << 20 | segment_idx`
     * can collide across two VFS instances on the same thread).
     * Per-`vfs_t` matches the W0 lock-table model and eliminates
     * the collision.  Entries are invalidated on GC (gc_generation
     * check at lookup time) and on vfs_destroy. */
    struct {
        int64_t       key;            /* (file_vp << 20) | segment_idx */
        SegmentArray  arr;
        bool          populated;
        int64_t       gen;            /* gc_generation at build time */
    } chain_walk_tcache[16];
    int        chain_walk_tcache_next;   /* round-robin index for replacement */
} TreeContext;

struct vfs_t {
    TreeContext* ctx;            /* heap-allocated tree context */
    struct LockTable* lock_table; /* per-vfs_t lock table (Phase 26 / W0) */
};

#endif /* VFS_INTERNAL_H */
