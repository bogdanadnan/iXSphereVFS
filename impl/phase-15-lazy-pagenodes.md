# Phase 15: Lazy PageNode Allocation

## Goal
Only allocate the PageNode for the page being written, not all 1024 upfront.
For small files (128 bytes → 1 page), this saves 1,023 `pool_alloc` +
1,023 `nodes_write_pagenode` = ~580µs per file creation. For large files,
allocations happen incrementally as pages are accessed, and the segment array
cache is built once the segment has enough PageNodes to justify it.

## Current State

`tree_resolve_page` allocates ALL 1024 PageNodes when it first encounters a
segment boundary:
```
for p = 1023..0: pool_alloc() + nodes_write_pagenode()
```
This takes ~580µs. A small-file write (128 bytes to page 0) uses 1 of 1024.

## Proposed State

`tree_resolve_page` only allocates the PageNode for the requested page.
The chain becomes sparse — some entries are missing.

### Sparse Chain Walk

When resolving a page in a segment, walk the existing chain to the target
position. Missing PageNodes are allocated on-demand and linked into the chain.

```c
int64_t vp = fc_page_root;
int64_t prev_vp = 0;
int pages_seen = 0;

for (int64_t j = 0; j <= page_in_segment; j++) {
    if (vp == 0 || vp == VFS_VPTR_NULL) {
        // PageNode doesn't exist at this position — allocate it
        vp = pool_alloc(&ctx->pool);
        uint8_t* pn = pool_resolve(&ctx->pool, vp);
        nodes_write_pagenode(pn, 0, 0, ctx->page_size);
        // Link into chain at prev_vp.nextPtr (CAS)
    } else {
        pages_seen++;  // page existed before this access
    }

    if (j == page_in_segment) return pool_resolve(&ctx->pool, vp);

    // Advance to next PageNode
    uint8_t* pn = pool_resolve(&ctx->pool, vp);
    prev_vp = vp;
    vp = (pn) ? nextPtr(pn) : 0;
}
```

### Threshold-Based Array Caching

No separate counter needed. During the chain walk, we already count pages
encountered. If `pages_seen >= CACHE_THRESHOLD` (e.g., 64), build the array
from the chain and cache it BEFORE returning:

```c
if (pages_seen >= CACHE_THRESHOLD && !already_cached) {
    // Build full array: re-walk the chain, fill array entries.
    // Missing positions get NULL — allocated on-demand later.
    int64_t array[1024];
    vp = fc_page_root;
    for (j = 0; j < seg_size; j++) {
        array[j] = vp;   // might be 0 or VFS_VPTR_NULL
        if (vp) vp = nextPtr(pool_resolve(vp));
    }
    // Cache in thread-local table
}
```

The cached array has NULLs for unwritten pages. When a future access hits a
NULL slot, the chain walk finds or allocates the PageNode and updates the
cached array entry. This is a write to the thread-local cache — safe, lock-free.

### What "Build the Array" Means

Build from EXISTING pages. Don't allocate missing pages. Missing positions
are NULL in the array. The array mirrors the chain as-is at the time of
building. New pages are added to the array incrementally as they're created.

### Small-File Impact

- First write: create 1 PageNode, chain length = 1. `pages_seen = 1 < 64`.
  No array built. ~1µs.
- 64th write: chain length = 64, array built. ~5µs for the build.
  Subsequent reads: O(1) from array.

## Implementation

### `tree_resolve_page` changes

```c
// Walk existing chain to find or create the PageNode
int64_t vp = (fc_slot) ? read_root_ptr(fc_slot) : 0;
int64_t prev_vp = 0;  // for linking new PageNodes
for (int64_t j = 0; j <= page_in_segment; j++) {
    if (vp == 0 || vp == VFS_VPTR_NULL) {
        // Allocate new PageNode
        vp = pool_alloc(&ctx->pool);
        uint8_t* pn = pool_resolve(&ctx->pool, vp);
        nodes_write_pagenode(pn, 0, 0, ctx->page_size);  // nextPtr=0, versionRoot=0
        segment_page_count++;
        // Link into chain
        if (prev_vp) { /* CAS prev->nextPtr = vp */ }
        else { /* CAS fc_root = vp */ }
    }
    uint8_t* pn = pool_resolve(&ctx->pool, vp);
    int64_t next = (pn) ? read_next_ptr(pn) : 0;
    if (j == page_in_segment) return pn;
    prev_vp = vp;
    vp = next;
}
```

### Threshold check

```c
if (segment_page_count >= CACHE_THRESHOLD) {
    // Build full array, cache in thread-local table
}
```

## Files Changed

| File | Change |
|------|--------|
| `src/tree.c` | `tree_resolve_page`: lazy allocation, threshold-based caching |

## Acceptance

- [ ] First 128-byte write to a new file: <10µs (was ~600µs)
- [ ] Sequential writes to large file: same throughput as today after 64 pages
- [ ] Random access to sparse segment: O(chained pages) chain walk
- [ ] GC handles sparse segments correctly
- [ ] Small-file write benchmark: >10,000 ops/sec (was 1,255)
- [ ] All 8 tests pass
