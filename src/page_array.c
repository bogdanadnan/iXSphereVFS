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
        uint8_t* slot = pool_resolve(pool, vp);
        if (!slot) break;
        uint32_t pn_idx;
        int64_t pn_next;
        int64_t pn_ver_root;
        nodes_read_pagenode(slot, &pn_ver_root, &pn_next, &pn_idx, page_size);
        (void)pn_ver_root;
        if (pn_idx < segment_size)
            arr->vptr_array[pn_idx] = vp;
        vp = pn_next;
    }

    arr->built = true;
    return VFS_OK;
}

uint8_t* segment_array_resolve(Pool* pool, SegmentArray* arr,
                               uint32_t page_index) {
    if (!arr->built) return NULL;
    assert(page_index < arr->seg_size);
    int64_t vp = arr->vptr_array[page_index];
    if (vp == VFS_VPTR_NULL) return NULL;
    return pool_resolve(pool, vp);
}

void segment_array_destroy(SegmentArray* arr) {
    if (arr->vptr_array) {
        free(arr->vptr_array);
        arr->vptr_array = NULL;
    }
    arr->built = false;
}
