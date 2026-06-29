# Phase 3: Pool Allocator

## Goal
32-byte fixed-slot pool page allocator with CAS-based free list and VirtualPtr.

## Workloads

### 3.1 Pool Page Layout
- 255 slots × 32 bytes + nextPoolPage(8) + poolState(4) + reserved(4) + padding(16)
- Fresh page init: slot[i].next_free = i+1, terminal 0xFFFF
- `poolState = (freeCount << 16) | firstFreeSlot`
- Init header on allocation (gen=1, mirror=-1) before linking into list

### 3.2 Slot Allocation
- CAS on `poolState` to pop from free list
- When `freeCount == 0`: allocate new page, init header, CAS `poolListHead`
- CAS failure: retry with updated `poolListHead` (retry-prepend, don't abandon)
- Return `VirtualPtr = (poolPage << 8) | slotIndex`
- Thread safety: per-page `poolState` CAS, global `poolListHead` CAS
- Arena optimization (optional): per-thread preferred page cursors

### 3.3 VirtualPtr
- 8-byte packed: `(poolPageIndex << 8) | (slotIndex & 0xFF)`
- `VFS_VPTR_NULL = 0`, `VFS_VPTR_PAGE(vp)`, `VFS_VPTR_SLOT(vp)`, `VFS_VPTR_MAKE(p, s)`
- Resolve: `page = cache_lookup(VFS_VPTR_PAGE(vp))` → `&slots[VFS_VPTR_SLOT(vp)]`
- Page 0 never a pool page → `(0, 0)` is safely null

### 3.4 Pool Page List
- Global chain rooted at `poolListHead` in superblock
- `nextPoolPage` write-once, set when page enters list
- Scan for pages with `freeCount > 0` for allocation
- Lazy rebuild at mount: walk tree collecting pool page indices from VirtualPtrs

### 3.5 Tests
- Allocate 255 slots → page full → new page allocated
- Concurrent allocation from 4 threads, 1024 slots
- VirtualPtr round-trip: pack → unpack → resolve → verify content
- CAS retry on full page

## Deliverables
- `src/pool.c`, `src/virtual_ptr.c`
- `test/test_pool.c`
