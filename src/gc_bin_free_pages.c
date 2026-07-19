/* Phase 28 Type 1: file-deletion bin job (work handler).
 *
 * Spec: impl/phase-28-bin-job-file-deletion.md §4.1
 *
 * Work type: BIN_WORK_FREE_PAGES
 *   context  = head of per-batch linked list (pool-allocated)
 *   context2 = number of entries in the batch
 *
 * The list was built by the analysis handler (src/gc_bin_file_deleted.c).
 * Each batch slot is 32 bytes (one pool slot) with the layout:
 *   offset  0: int64 next_batch_slot (VP of next batch slot, 0 = end)
 *   offset  8: int32 logical_page   (the page to free)
 *   offset 12: int32 padding        (zero)
 *
 * The handler iterates the list, reads the PageHeader of each
 * logical page to find the mirror sibling, and calls storage_free
 * on both.  Per §6.2 of the spec, storage_free does NOT free the
 * mirror — the work handler does it explicitly.
 *
 * The batch slots themselves are NOT freed (TODO-12: pool_free is
 * a stub).  They leak for now; an out-of-band shadow-compaction
 * reclaims them.
 */
#include "gc.h"
#include "bin.h"
#include "storage.h"
#include <unistd.h>

/* Forward decl. */
static int64_t read_mirror_page(StorageBackend* sb, int64_t logical);

int gc_handle_free_pages(vfs_t* vfs, const BinEntry* entry) {
    if (!vfs || !vfs->ctx || !entry) return VFS_ERR_IO;
    TreeContext* ctx = vfs->ctx;
    int64_t pages_head  = entry->context;
    int64_t pages_count = entry->context2;

    if (pages_head == 0 || pages_count <= 0) {
        return VFS_OK;  /* empty batch; nothing to do */
    }

    int64_t slot = pages_head;
    int64_t processed = 0;
    for (int64_t i = 0; i < pages_count && slot != 0; i++) {
        PoolSlot bs = {0};
        pool_acquire(&ctx->pool, slot, false, &bs);
        if (bs.vptr == VFS_VPTR_NULL) {
            /* Slot was freed by a prior run or is corrupt.  Stop
               the iteration; remaining pages are reclaimed by the
               next shadow-compaction. */
            break;
        }
        int32_t logical = vfs_rd4_s(bs.bytes, 8, ctx->page_size);
        int64_t next    = vfs_rd8_s(bs.bytes, 0, ctx->page_size);
        pool_release(&ctx->pool, &bs);

        /* Free the logical page.  storage_free is idempotent
           (checks indir_lookup before acting). */
        storage_free(ctx->sb, (int64_t)logical);
        processed++;

        /* Find and free the mirror sibling, if any.  The current
           storage_free does NOT free mirrors (per §6.2 finding in
           the spec).  The work handler reads the PageHeader to
           find the mirror, matching the existing
           deferred_free_enqueue pattern in src/gc.c. */
        int64_t mirror = read_mirror_page(ctx->sb, (int64_t)logical);
        if (mirror >= 0) {
            storage_free(ctx->sb, mirror);
        }
        slot = next;
    }

    /* The batch slots themselves are NOT freed here (TODO-12:
       pool_free is a stub).  They leak for now.  An out-of-band
       shadow-compaction reclaims them.  See the spec's §4.2 for
       the expected leak rate. */
    return VFS_OK;
}

/* ---------------------------------------------------------------------------
 * read_mirror_page — read the PageHeader of a logical page and
 * return its mirror sibling's logical page index (-1 if none).
 *
 * Duplicated from src/gc_bin_file_deleted.c (kept local to avoid
 * cross-file coupling; the function is small).
 * --------------------------------------------------------------------------- */
static int64_t read_mirror_page(StorageBackend* sb, int64_t logical) {
    if (!sb || logical < 2) return -1;
    int64_t phys = indir_lookup(sb, logical);
    if (phys == 0) return -1;
    PageHeader ph;
    ssize_t n = pread(sb->fd, &ph, PAGE_HEADER_SIZE, phys);
    if (n != PAGE_HEADER_SIZE) return -1;
    return (int64_t)ph.mirror_page;
}
