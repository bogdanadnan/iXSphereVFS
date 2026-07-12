/* Phase 5c: In-memory page array for O(1) PageNode lookup */
#include "page_array.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

int segment_array_build(Pool* pool, int64_t fc_pageRootPtr,
                        uint32_t segment_size, SegmentArray* arr) {
    arr->vptr_array = (int64_t*)malloc((size_t)segment_size * sizeof(int64_t));
    if (!arr->vptr_array) return VFS_ERR_NOMEM;

    /* Fill with VFS_VPTR_NULL (0) — unallocated pages map to NULL */
    memset(arr->vptr_array, 0, segment_size * sizeof(int64_t));
    arr->seg_size = segment_size;

    /* Walk the sparse PageNode chain, placing each node at its page_index */
    int64_t page_size = pool->sb->page_size;
    int64_t vp = fc_pageRootPtr;
    while (vp != 0) {
        /* Phase 25: by-value pool slot (read-only).  Closing the C1
           hazard on the build path — segment_array is read-only after
           build, so a stale pointer here would corrupt every subsequent
           tcache hit. */
        PoolSlot slot = {0};
        pool_acquire(pool, vp, false, &slot);
        if (slot.vptr == VFS_VPTR_NULL) break;
        uint32_t pn_idx;
        int64_t pn_next;
        int64_t pn_ver_root;
        nodes_read_pagenode(slot.bytes, &pn_ver_root, &pn_next, &pn_idx, page_size);
        (void)pn_ver_root;
        if (pn_idx < segment_size)
            arr->vptr_array[pn_idx] = vp;
        pool_release(pool, &slot);
        vp = pn_next;
    }

    arr->built = true;
    return VFS_OK;
}

bool segment_array_resolve(Pool* pool, SegmentArray* arr,
                           uint32_t page_index, PoolSlot* out) {
    if (!out) return false;
    out->vptr = VFS_VPTR_NULL;
    out->pinnedPage = 0;
    memset(out->bytes, 0, VFS_POOL_SLOT_SIZE);

    if (!arr->built) return false;
    assert(page_index < arr->seg_size);
    int64_t vp = arr->vptr_array[page_index];
    if (vp == VFS_VPTR_NULL) return false;
    /* Phase 25: by-value pool slot (read-only).  Caller gets a
       stack-local copy; no pin, no release needed. */
    pool_acquire(pool, vp, false, out);
    return (out->vptr != VFS_VPTR_NULL);
}

void segment_array_destroy(SegmentArray* arr) {
    if (arr->vptr_array) {
        free(arr->vptr_array);
        arr->vptr_array = NULL;
    }
    arr->built = false;
}
