# TODO — Future Optimizations

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

## TODO-6 — NodeId/VirtualPtr API Mismatch [REQUIRED FIX]

### Problem
The public API documents file/directory handles as `nodeId` (uint32_t, sequential
identifier). But every internal function (`vfs_write`, `vfs_read`, `vfs_file_size`,
`vfs_file_mtime`, `pool_resolve`) passes this value directly to `pool_resolve`,
which expects a **VirtualPtr** (`(page << 16) | slot`). These are different
namespaces — nodeIds start at 0 and increment by 1, while VirtualPtrs encode
pool page + slot indices.

All current callers work around this by passing VirtualPtrs (obtained via
`resolve_child_vp` or `get_file_vp`), not nodeIds. `vfs_create` returns the
actual nodeId but callers discard it and use `resolve_child_vp` instead.
The API is inconsistent — it says nodeId but operates on VirtualPtrs.

### Required Fix
Add a `NodeTable` — an in-memory `nodeId → VirtualPtr` hash table built at
mount by walking the tree. All internal functions that currently receive a
"file" parameter use the NodeTable to convert nodeId → VirtualPtr before
calling `pool_resolve`. This bridges the gap without changing the public API
signatures.

### Files Affected
- `src/node_table.c`, `src/node_table.h` — new files
- `src/vfs.c` — build NodeTable on mount via tree walk
- `src/tree.c` — `vfs_write`, `vfs_read`, `vfs_file_size`, `vfs_file_mtime`,
  `vfs_file_ctime` — add `node_table_lookup` before `pool_resolve`
- `test/test_tree.c` — update tests to pass nodeIds instead of VirtualPtrs

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
