#ifndef VFS_BIN_H
#define VFS_BIN_H

#include "ixsphere/vfs.h"
#include "storage.h"

/* ---------------------------------------------------------------------------
 * Phase 28 Bin (spec: impl/phase-28-gc.md)
 *
 * A persistent FIFO queue stored in the VFS backing file, used by the GC
 * to deliver garbage-collection work entries from producers (user-facing
 * operations) to the GC thread (consumer).
 *
 * On-disk layout:
 *   Header page (page 0):
 *     offset 64: int64_t  bin_head   (VP of first Bin page; 0 = empty)
 *     offset 72: int64_t  bin_tail   (VP of last Bin page;  0 = empty)
 *     offset 80: int64_t  bin_count  (total entries across all Bin pages)
 *     offset 88: int64_t  reserved   (zero; reserved for one future field)
 *
 *   Bin page (regular storage page with custom layout):
 *     offset  0: int64_t  next_bin_page  (VP of next Bin page; 0 = end)
 *     offset  8: int32_t  count          (entries currently in this page)
 *     offset 12: int32_t  capacity       (max entries; set at allocation)
 *     offset 16: BinEntry entries[]      (each entry is 16 bytes)
 *
 * Capacity: 510 entries per 8KB page, 255 per 4KB page.
 * --------------------------------------------------------------------------- */

/* BinEntry: 24-byte typed entry in the Bin.
 *
 * Phase 28 Type 1: file-deletion bin job needs two int64 contexts
 * (file_vp + tombstone_vp).  The 16-byte layout (8+4+4) only fits
 * one int64 + two int32 — too small.  Layout expanded to 24 bytes
 * (8+4+4+8) for full int64 support.  Capacity drops from 510 to
 * 340 entries per 8KB page (still ample; the Bin is a transient
 * queue, not a storage backbone).
 *
 * On-disk layout per entry (at offset 16 + idx * 24 in a Bin page):
 *   offset  0: int64_t context
 *   offset  8: int32_t type
 *   offset 12: int32_t reserved (zero)
 *   offset 16: int64_t context2 */
typedef struct {
    int64_t context;   /* type-specific identifier (8 bytes) */
    int32_t type;      /* bin_entry_type_t — trigger or work tag (4 bytes) */
    int32_t rsvd;      /* reserved for alignment (4 bytes) */
    int64_t context2;  /* second type-specific identifier (8 bytes) */
} BinEntry;

/* Type tag threshold.  Entries with type < THRESHOLD are TRIGGER
 * entries; entries with type >= THRESHOLD are WORK entries.
 * The framework's dispatch in gc_process_entry branches on this. */
#define BIN_TYPE_WORK_THRESHOLD  0x100

/* Trigger types (initial; the framework's initial implementation only
 * uses BIN_TRIGGER_NOOP.  Other trigger types are added by per-bin-job
 * specs.) */
typedef enum {
    BIN_TRIGGER_NOOP               = 0,   /* placeholder: no analysis, just delete */
    BIN_TRIGGER_FILE_DELETED       = 1,   /* Phase 28 Type 1: file-deletion bin job
                                            (spec: impl/phase-28-bin-job-file-deletion.md).
                                            context = file VP, context2 = tombstone VP. */
    /* Future: BIN_TRIGGER_FILE_TRUNCATED, BIN_TRIGGER_EPOCH_COMMITTED, ... */
} bin_trigger_type_t;

/* Work types (Phase 28 Type 1: file-deletion bin job). */
typedef enum {
    /* BIN_TYPE_WORK_THRESHOLD + 0 = 0x100.  context = head of per-batch
       linked list (pool-allocated), context2 = pages count.
       The work handler (src/gc_bin_free_pages.c) iterates the list,
       calls storage_free on each logical page, and reads the PageHeader
       to find + free the mirror sibling. */
    BIN_WORK_FREE_PAGES            = 0x100,
    /* Future: BIN_WORK_REMOVE_TOMBSTONE, BIN_WORK_DROP_SOFT_DELETE, ... */
} bin_work_type_t;

/* Return values */
#define BIN_OK              0   /* success */
#define BIN_ERR_EMPTY      -2   /* bin_pop / bin_peek: queue is empty */
#define BIN_ERR_FULL       -7   /* bin_push: could not allocate Bin page (disk full) */
#define BIN_ERR_IO         -1   /* I/O error */
#define BIN_ERR_AGAIN      -8   /* bin_push: CAS contention exceeded retry limit */

/* Maximum number of bin_push retries (for the per-page CAS on count). */
#define BIN_PUSH_MAX_RETRIES  1000

/* Maximum number of entries in a Bin page (depends on page_size).
   Computed as (page_size - 16) / sizeof(BinEntry). */
static inline int bin_page_capacity(int64_t page_size) {
    return (int)((page_size - 16) / (int)sizeof(BinEntry));
}

/* ---------------------------------------------------------------------------
 * Public API (bin.c)
 * --------------------------------------------------------------------------- */

/* Append a BinEntry to the Bin.
 *
 * Thread-safe.  Multiple FUSE worker threads can call concurrently.
 * The common case (appending to an existing Bin page with room) is
 * lock-free via per-page CAS on count.  The rare case (Bin page full
 * or queue empty) takes a short-lived spinlock to allocate a new Bin
 * page and link it.
 *
 * On failure, the push is dropped after bounded retry (10ms × 3
 * attempts for storage_allocate_bin_page; 1000 CAS attempts for
 * per-page count contention).  The Bin entry is lost; the garbage
 * remains on disk and will be re-identified by the next related
 * producer's push (trigger analysis is idempotent).
 *
 * Returns BIN_OK on success, BIN_ERR_FULL on disk-full failure
 * (after retry), or BIN_ERR_AGAIN on CAS contention exhaustion. */
int bin_push(StorageBackend* sb, int32_t type,
             int64_t context, int64_t context2);

/* Pop the next BinEntry from the Bin.
 *
 * Single-threaded (only the GC thread pops).  Returns BIN_OK on
 * success (entry written to *out_entry), BIN_ERR_EMPTY if the Bin
 * is empty, or BIN_ERR_IO on I/O error. */
int bin_pop(StorageBackend* sb, BinEntry* out_entry);

/* Peek at the next BinEntry without removing it.
 *
 * Same as bin_pop but does not modify the Bin.  Returns BIN_OK on
 * success, BIN_ERR_EMPTY if the Bin is empty. */
int bin_peek(StorageBackend* sb, BinEntry* out_entry);

/* Mount-time validation walk.  Called from storage_open after
 * indir_init.  Walks the Bin chain (using raw pread, NOT the cache,
 * to avoid putting Bin pages in the cache and triggering the
 * W5-style cache interaction) and verifies that bin_count matches
 * the sum of per-page counts.  On mismatch, rebuilds bin_count.
 *
 * Also validates each entry's stored fields (context, type, context2)
 * for basic sanity (non-zero context, type in valid range).  Bad
 * entries are skipped (count decremented).
 *
 * This is best-effort recovery for the crash window between
 * per-page CAS and on-disk flush. */
void validate_bin_on_mount(StorageBackend* sb);

#endif /* VFS_BIN_H */
