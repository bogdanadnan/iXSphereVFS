#ifndef VFS_PAGE_ARRAY_H
#define VFS_PAGE_ARRAY_H

#include "ixsphere/vfs.h"
#include "pool.h"
#include "page_buf.h"
#include "nodes.h"
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * SegmentArray — in-memory array for O(1) PageNode lookup
 *
 * On first access to a FileContent segment, the VFS walks the PageNode
 * chain and builds a flat array of VirtualPtrs.  Subsequent lookups are
 * O(1) via indexed access into the array.
 *
 * One SegmentArray per segment.  The array is rebuilt after GC.
 * --------------------------------------------------------------------------- */

typedef struct SegmentArray {
    int64_t* vptr_array;     /* malloc'd array of VirtualPtrs, size = segment_size */
    bool     built;           /* set to true after chain walk completes */
} SegmentArray;

/* Build the in-memory array by walking the PageNode chain rooted at
 * fc_pageRootPtr (the FileContent's pageRootPtr field).
 * Allocates vptr_array via malloc; caller must call segment_array_destroy.
 * Returns VFS_OK on success, VFS_ERR_NOMEM on allocation failure. */
int segment_array_build(Pool* pool, int64_t fc_pageRootPtr,
                        uint32_t segment_size, SegmentArray* arr);

/* Resolve a page within the segment to its PageNode slot pointer.
 * The page_index must be < segment_size.
 * Returns a pointer into the page cache (via pool_resolve), or NULL
 * if the VirtualPtr is null or the page is not in cache. */
uint8_t* segment_array_resolve(Pool* pool, SegmentArray* arr,
                               uint32_t page_index);

/* Free the vptr_array and reset the struct to zero-initialized state. */
void segment_array_destroy(SegmentArray* arr);

#endif /* VFS_PAGE_ARRAY_H */
