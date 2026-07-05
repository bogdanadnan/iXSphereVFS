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

When resolving a page in a segment, walk the existing chain first:
```c
int64_t vp = fc_page_root;
for (j = 0; j <= page_in_segment; j++) {
    if (vp == 0) {
        // PageNode doesn't exist — allocate and link it
    }
    if (j == page_in_segment) return pool_resolve(vp);
    vp = nextPtr(vp);  // might be 0 if missing
}
```

Missing PageNodes are allocated on access with zero `versionRootPtr`,
linking them into the chain at the correct position.

### Threshold-Based Array Caching

Track how many PageNodes exist in the segment. When the count exceeds
a threshold (e.g., 64), build the segment array and cache it in the
thread-local table. Before the threshold, walk the chain on each access.

```c
// Track per-segment. Store alongside the thread-local cache.
struct {
    int64_t key;
    int64_t vptr_array[1024];
    uint32_t seg_size;
    int     page_count;   // number of non-null PageNodes
} tcache[TCACHE_SIZE];
```

When `page_count < CACHE_THRESHOLD`, don't cache this segment — just walk.
When `page_count >= CACHE_THRESHOLD`, build the full array and cache it.

### Small-File Impact

- First write (128 bytes to page 0): allocate 1 PageNode (~1µs) instead of
  1024 (~580µs). Segment is sparse (1/1024).
- Second write to same page: warm, no allocation. ~0µs.
- GC: walks sparse chain, same correctness. Slightly slower (more iterations)
  but small files don't trigger frequent GC.

### Large-File Impact

- As pages are written, PageNodes accumulate. After 64 pages (the threshold),
  the array is built and cached. Subsequent reads within the segment are O(1)
  just like today.

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
