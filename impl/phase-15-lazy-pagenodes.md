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

Each PageNode in the chain has a `page_index` (new field, uint32_t).
The chain is a singly-linked list sorted by `page_index`. Gaps are allowed —
`nextPtr` skips directly to the next allocated PageNode regardless of how
many pages are between them.

```c
// New field in PageNode layout:
#define PAGENODE_OFF_PAGEINDEX  8   // uint32_t — page index within segment
```

Chain walk with gaps:
```c
int64_t vp = fc_page_root;
int64_t prev_vp = 0;
int total_pages = 0;

while (vp != 0) {
    uint8_t* pn = pool_resolve(&ctx->pool, vp);
    uint32_t idx = vfs_rd4_s(pn, PAGENODE_OFF_PAGEINDEX, ctx->page_size);

    if (idx == page_in_segment) {
        total_pages++;
        return pn;   // found it
    }
    if (idx > page_in_segment) {
        // Target page doesn't exist — allocate it now, link before this one
        int64_t new_vp = pool_alloc(&ctx->pool);
        uint8_t* new_pn = pool_resolve(&ctx->pool, new_vp);
        nodes_write_pagenode(new_pn, 0, vp, ctx->page_size);
        vfs_wr4_s(new_pn, PAGENODE_OFF_PAGEINDEX, (uint32_t)page_in_segment, ctx->page_size);
        // Link: CAS prev.nextPtr = new_vp (or fc_root = new_vp)
        total_pages++;
        return new_pn;
    }

    total_pages++;
    prev_vp = vp;
    vp = vfs_rd8_s(pn, PAGENODE_OFF_NEXTPTR, ctx->page_size);
}

// Chain exhausted — target is beyond the last entry. Allocate at end.
int64_t new_vp = pool_alloc(&ctx->pool);
uint8_t* new_pn = pool_resolve(&ctx->pool, new_vp);
nodes_write_pagenode(new_pn, 0, 0, ctx->page_size);
vfs_wr4_s(new_pn, PAGENODE_OFF_PAGEINDEX, (uint32_t)page_in_segment, ctx->page_size);
// Link: CAS prev.nextPtr = new_vp (or fc_root = new_vp if segment empty)
total_pages++;
return new_pn;
```

**Example**: segment has pages 0 and 5 allocated. Chain: [PageNode(idx=0) → PageNode(idx=5)].
Write at page 3: walk finds idx=0 < 3, advance. idx=5 > 3 → create PageNode(idx=3),
link between them. Chain becomes [0 → 3 → 5]. No allocation of pages 1,2,4.

Write at page 8: walk exhausts chain at idx=5 < 8 → create PageNode(idx=8),
append at end. Chain becomes [0 → 3 → 5 → 8].

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

### Threshold-Based Array Caching

`total_pages` counts how many PageNodes exist in this segment (tracked during
the walk). If `total_pages >= CACHE_THRESHOLD` (e.g., 64), build the sparse
array from the chain and cache it in the thread-local table. Unwritten pages
are NULL (0) in the array. Access to a NULL entry triggers the chain walk,
which finds or creates the PageNode and updates the cached entry.

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
