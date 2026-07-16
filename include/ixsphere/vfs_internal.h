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

/* ---------------------------------------------------------------------------
 * Phase 27 W8: error-propagation macro + debug check
 *
 * The public VFS API (vfs_read, vfs_write, vfs_create, etc.) returns
 * the number of bytes / objects on success and -1 on error.  The FUSE
 * layer maps the error to errno via:
 *
 *   return vfs_error_to_errno(vfs_last_error(state->vfs));
 *
 * This contract requires that EVERY return -1 is preceded by a
 * `vfs->ctx->last_error = <code>` assignment.  vfs_read/vfs_write
 * alone had 8+ such paths that did not set last_error (tree_resolve_page
 * failure, storage_allocate failure, pool_alloc failure, pool_acquire
 * failure, etc.) — meaning the FUSE layer would see a stale or
 * undefined last_error and report the wrong errno to the application.
 *
 * VFS_RETURN_ERROR(vfs, err) replaces the pattern
 *   vfs->ctx->last_error = err; return -1;
 * with a single macro that is hard to misuse (both halves happen
 * atomically in one statement).
 *
 * vfs_return(vfs, ret) is a wrapper for the END of public APIs: if
 * ret is -1 and last_error is still VFS_OK, it sets last_error to
 * VFS_ERR_IO and (in debug builds) logs a warning.  This catches any
 * future regression where a raw `return -1` slips in.
 * --------------------------------------------------------------------------- */

#define VFS_RETURN_ERROR(vfs, err) \
    do { \
        (vfs)->ctx->last_error = (err); \
        return -1; \
    } while (0)

static inline int vfs_return(vfs_t* vfs, int ret) {
    if (ret == -1) {
        if (vfs->ctx->last_error == VFS_OK) {
            /* BUG: public API returned -1 without setting last_error.
             * Default to VFS_ERR_IO so the FUSE layer can map to a
             * sane errno.  In debug builds, log to make the bug
             * visible. */
            #ifdef VFS_DEBUG
            fprintf(stderr, "WARN: vfs function returned -1 without setting last_error; defaulting to VFS_ERR_IO\n");
            #endif
            vfs->ctx->last_error = VFS_ERR_IO;
        }
    }
    return ret;
}

#endif /* VFS_INTERNAL_H */
