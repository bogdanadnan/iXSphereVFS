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
position. Every NULL in the chain is lazily allocated — there are no gaps.

```c
int64_t vp = fc_page_root;   // first PageNode, may be NULL for empty segment
int64_t prev_vp = 0;
int pages_seen = 0;

for (int64_t j = 0; j <= page_in_segment; j++) {
    if (vp == 0 || vp == VFS_VPTR_NULL) {
        // Gap in the chain — allocate this PageNode now.
        // This happens for ALL pages between the chain end and the target.
        vp = pool_alloc(&ctx->pool);
        uint8_t* pn = pool_resolve(&ctx->pool, vp);
        nodes_write_pagenode(pn, 0, 0, ctx->page_size);  // versionRoot=0, nextPtr=0
        // Link into chain: CAS prev.nextPtr = vp (or fc_root = vp if j==0)
    } else {
        pages_seen++;
    }
    if (j == page_in_segment) return pool_resolve(&ctx->pool, vp);
    uint8_t* pn = pool_resolve(&ctx->pool, vp);
    prev_vp = vp;
    vp = (pn) ? vfs_rd8_s(pn, PAGENODE_OFF_NEXTPTR, ctx->page_size) : 0;
}
```

**Example**: segment is empty (fc_page_root = NULL). First write at page 5.
Loop iterates j=0..5. Every iteration hits `vp == NULL` → allocates.
Result: 6 PageNodes created, pages 0-4 have `versionRootPtr=0`, page 5 has
the VersionPage from the write. Next write at page 3: chain walk traverses
pages 0,1,2,3. No allocations needed (`vp != NULL` for all). Returns page 3's
PageNode. `pages_seen = 6 ≥ 64?` No, still below threshold.

**Threshold trigger**: at write #64, `pages_seen = 64 ≥ 64` → build the
full 1024-entry array. Walk chain one more time, filling `array[j] = vp`
for positions 0-63 (valid VirtualPtrs) and positions 64-1023 (NULL —
not yet allocated). Cache the array. Subsequent reads within the segment
hit the array. Writes to pages > 63 allocate new PageNodes, update the
array entry in-place.

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

## GC Impact

GC walks the FileContent chain to find PageNodes. Sparse segments have fewer
PageNodes — GC is FASTER for segments below the threshold (fewer entries to
copy). Above threshold, the full array is built and used — same as today.
GC rebuilds pool pages, which invalidates thread-local caches. Next access
after GC does a fresh chain walk — pages are re-allocated lazily.
