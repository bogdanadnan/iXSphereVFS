# TODO — Future Optimizations

## Phase 15 — Sparse PageNode chains [COMPLETED]

PageNode chains are now sparse — only written pages have allocated PageNodes.
Each PageNode carries a `page_index` field (offset 16, uint32) used by the
sorted-insert CAS protocol in `tree_resolve_page`.  Sparse segments reduce
pool memory overhead for files where only a fraction of pages are written.

GC cost for sparse segments is proportional to allocated count rather than
`segment_size`.  The GC walks `nextPtr` chains and copies `page_index` at
offset 16 without remapping (the value range 0..seg_size-1 does not collide
with valid VirtualPtrs, min VirtualPtr = 131072).

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
Phase 16 delivered the underlying VarArray data structure; DirIndex integration is still pending.
Phase 17 added hash fast-reject to name lookups; full DirIndex remains future work.

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

## TODO-10 — Commit Name Collision Detection [REQUIRED FIX]

### Problem
`vfs_commit` does not detect name collisions in directories when a snapshot
is committed. If fileA is renamed to "TEST" at epoch 2 (live head) and
fileB is renamed to "TEST" at epoch 1 (snapshot), committing epoch 1
produces two files named "TEST" at epoch 2 — violating directory uniqueness.

`commit_scan_dir` checks file content conflicts but not name collisions.

### Required Fix
In `commit_scan_dir`, track seen names per directory at the commit epoch.
For each visible DirContent entry, check if the name (via `nodes_read_name_hash`)
was already seen. If a collision is detected, return `VFS_ERR_EXISTS` and
abort the commit.

### Files Affected
- `src/epoch.c` — `commit_scan_dir`: add hash-set or sorted name tracking
- `test/test_epoch.c` — add test: rename two files to same name, commit, verify rejected

---

## TODO-11 — storage_allocate Tail-Advance vs. GC Mid-Table Freeing

### Problem
`storage_allocate` was redesigned (Phase 18 optimization) to use a tail-advance
fast path: `sb->total_pages` is atomically incremented and the new page is
returned. The fast path assumes the next free slot is always at the current
end of the indirection table — which holds during normal operation because
the only caller of `storage_free` is GC, and GC is not exercised during
extraction-style workloads.

After GC runs (`vfs_gc`), however, mid-table slots become free (`storage_free`
sets their indirection entry to 0). The tail-advance path then skips over
those holes — they become permanently leaked physical space.

### Required Fix
When GC reclaims mid-table slots, either:
  (a) **Rewrite GC to compact logically**: after VFS_ERR_FULL GC, scan the
      indirection table and physically copy surviving data pages into the
      unallocated tail, updating their indirection entries and advancing
      `total_pages` only as needed.  This pairs with TODO-4 (physical
      compaction) and TODO-5 (physical offset reuse stack).
  (b) **Switch to a free-list alloc policy**: maintain a bitmap / heap of free
      indirection slots, populate it on mount from the inline + overflow
      indirection table, push entries onto it as `storage_free` runs, pop in
      `storage_allocate`.  Falls back to tail-advance when empty.

Until one of these lands, do NOT run `vfs_gc` while expecting `storage_allocate`
to recover the freed slots — they will be leaked.

### Files Affected
- `src/storage.c` — `storage_allocate` and storage_free currently; free-list
  would land here
- `src/gc.c` — `vfs_gc` shadow-compaction: avoid leaving holes (TODO-4),
  or trigger free-list repopulation before returning
- `test/test_gc.c` — add a test: run GC, verify `storage_allocate` returns
  non-leaked page indices

### Status
**Active TODO** — deferred to be done alongside TODO-4 and TODO-5.  Until
then, the tail-advance optimization in `storage_allocate` is correct
because nothing frees slots during the hot path.
