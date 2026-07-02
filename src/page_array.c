/* Phase 5c: In-memory page array for O(1) PageNode lookup */
#include "page_array.h"
#include <stdlib.h>
#include <string.h>

int segment_array_build(Pool* pool, int64_t fc_pageRootPtr,
                        uint32_t segment_size, SegmentArray* arr) {
    arr->vptr_array = (int64_t*)malloc((size_t)segment_size * sizeof(int64_t));
    if (!arr->vptr_array) return VFS_ERR_NOMEM;

    int64_t vp = fc_pageRootPtr;
    uint32_t i;
    for (i = 0; i < segment_size; i++) {
        if (vp == VFS_VPTR_NULL) {
            /* Fewer pages than segment_size → fill rest with null */
            for (; i < segment_size; i++)
                arr->vptr_array[i] = VFS_VPTR_NULL;
            break;
        }
        arr->vptr_array[i] = vp;

        /* Walk to next PageNode */
        uint8_t* slot = pool_resolve(pool, vp);
        if (!slot) {
            /* Page not in cache — can't walk further, fill rest as null */
            for (i++; i < segment_size; i++)
                arr->vptr_array[i] = VFS_VPTR_NULL;
            break;
        }
        vp = vfs_rd8(slot, PAGENODE_OFF_NEXTPTR);
    }

    arr->built = true;
    return VFS_OK;
}

uint8_t* segment_array_resolve(Pool* pool, SegmentArray* arr,
                               uint32_t page_index) {
    if (!arr->built) return NULL;
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
