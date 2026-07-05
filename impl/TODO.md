# TODO — Future Optimizations

## Phase 15 — Sparse PageNode chains & v2 format [COMPLETED]

PageNode chains are now sparse — only written pages have allocated PageNodes.
Each PageNode carries a `page_index` field (offset 16, uint32).  The format
version in the superblock (`SB_OFF_FORMAT_VERSION`) distinguishes v1 (dense,
no page_index) from v2 (sparse, with page_index).  v1 files are auto-migrated
to v2 on first mount via `tree_migrate_v1_to_v2`.

GC cost for sparse segments is proportional to allocated count rather than
`segment_size`, reducing overhead for files where only a fraction of pages are
written.  The GC walks `nextPtr` chains and copies `page_index` at offset 16
without remapping (the value range 0..seg_size-1 does not collide with valid
VirtualPtrs, min VirtualPtr = 131072).

---

Items deferred from current implementation. Not blocking any phase.

---

## TODO-4 — GC Physical Compaction

### Problem
GC reclaims logical pages (setting indirection entries to 0) but never
compacts physical space. Between GC cycles, the backing file grows
monotonically. Data pages from dropped VersionPages leave physical holes.

### Proposed Solution
During GC shadow-compaction, allocate a new sequential range of physical
offsets for surviving data pages, copy their content, and update indirection
entries. After the swap, `ftruncate` the backing file to reclaim unused
physical space.

### Impact
- GC time: +O(surviving data pages) for copy (already O(metadata) for tree walk)
- Backing file size: shrinks after GC
- Complexity: must coordinate physical offsets with deferred-free queue

---

## TODO-5 — Physical Offset Reuse Stack

### Problem
Every `Free` sets the indirection entry to 0, but the physical space at the
old offset is never reused. Between GC cycles, `physical_tail` advances
monotonically, creating physical holes.

### Proposed Solution
Per the deferred optimization noted in SPEC.md §3.8: maintain a stack of freed
physical offsets. `Allocate` pops from the stack when non-empty, falling back
to advancing `physical_tail`. All pages are the same size (`page_size + 16`),
so there is zero fragmentation.

### Impact
- `Allocate`: O(1) pop or CAS advance (no change)
- `Free`: O(1) push (currently just sets entry to 0)
- Backing file: stops growing between GC cycles

## TODO-6 — NodeId/VirtualPtr API Mismatch [RESOLVED]

Resolved by the Phase 14 VirtualPtr API change — `vfs_create` and `vfs_mkdir`
now return the child's VirtualPtr directly, eliminating the need for a NodeTable.
See `phase-14-vptr-api.md` for details.

## TODO-7 — Directory Index [Optional]

Same as TODO-1 but marked as optional — may not be implemented.

## TODO-8 — Lazy Tree Caching [Optional]

Unified VfsNode tree with cached children per directory. Eliminates all
remaining chain walks from the read path. Optional — deferred based on
benchmark results.

## TODO-9 — VFS Multi-Threaded Concurrency [REQUIRED FIX]

### Problem
The VFS crashes (segfault) with 2+ threads sharing a single `vfs_t` handle.
Benchmark works around this with per-thread handles — measuring multi-connection
throughput, not true VFS concurrency. The VFS is designed for thread safety
(CAS allocators, per-bucket page cache locks, per-epoch file locks) but
has not been stress-tested at the shared-handle level.

### Required Fix
Run `vfs_bench --threads=2` under lldb/gdb to identify the crash site.
Likely candidates: `ensure_mirror_arrays` or `indir_ensure_capacity` realloc
race, page cache bucket corruption, or `pool_resolve` dirty-marking race.
Fix the crash, then validate with `--threads=4,8,16`.

### Files Affected
- VFS internals — crash site to be identified by debugger
- `bench/bench.c` — switch workers from per-handle mounts to shared `vfs_t*`
