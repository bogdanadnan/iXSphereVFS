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

## TODO-12 — Pool Slot Freeing (deferred from Phase 28 first bin job)

### Problem

The current pool allocator (`src/pool.c::pool_alloc`) only supports
allocation; there is no `pool_free` operation. Individual pool slots
are never freed back to the pool. The existing stop-the-world GC
handles this implicitly by shadow-compacting the entire pool into
new pages and freeing the old pool pages wholesale.

The new per-bin-job GC (Phase 28) frees individual slots (chain
slots — FileNode, FileContent, PageNode, FileSize, DirContent for
the create + tombstone removal) without rebuilding pool pages. We
need a way to return freed slots to the pool's free list.

### Proposed Solution

Add a `pool_free(Pool* pool, int64_t slot_vp)` operation that:
- Pushes the slot onto the page's free list (use slot's bytes 0-1
  to store the next free index, same layout as the freshly-allocated
  page per SPEC §5.3).
- Atomically updates the page's `poolState` (freeCount / firstFreeSlot)
  via CAS to reflect the new free count.
- Thread-safe (matches the existing `pool_alloc` per-page CAS pattern).

Required by the Phase 28 file-deletion bin job (and subsequent bin
jobs: snapshot delete, commit, truncate, etc.). Until this lands,
the file-deletion bin job's spec uses a stub `pool_free` that
returns success without doing anything (a leak — pool slots are
recovered only on next shadow-compaction by an out-of-band GC pass).

### Impact

- Pool memory: 32 bytes per freed slot returned to the free list
  (small vs. the data pages being freed, but a real win for
  delete-heavy workloads that create and delete many small files).
- `pool_free`: O(1) per-page CAS (same cost as `pool_alloc`).
- Crashes: free-list is in-memory only (matches `pool_alloc`); on
  remount, the free-list is rebuilt from `poolState` per page.
  Slots freed in a crash window are leaked — same semantics as
  the existing stop-the-world GC's behavior under crash.

### Files Affected
- `src/pool.c`, `src/pool.h` — add `pool_free`
- `src/gc.c` (or per-bin-job files) — call `pool_free` for each
  chain slot identified as freeable by the bin job
- `test/test_pool.c` — add a test: allocate, free, re-allocate
  (the same slot should be returned)

---

## TODO-11 — storage_allocate Tail-Advance vs. GC Mid-Table Freeing [RESOLVED]

### Problem (historical)
`storage_allocate` was redesigned (Phase 18 optimization) to use a tail-advance
fast path: `sb->total_pages` is atomically incremented and the new page is
returned. The fast path assumes the next free slot is always at the current
end of the indirection table — which holds during normal operation because
the only caller of `storage_free` is GC, and GC is not exercised during
extraction-style workloads.

After GC runs (`vfs_gc`), however, mid-table slots become free (`storage_free`
sets their indirection entry to 0). The tail-advance path then skips over
those holes — they become permanently leaked physical space.

### Resolution (Phase 27 W3 / W5 / W6)

Option (b) was implemented by the Phase 27 free-page queue (W3 initial,
W5 enqueue, W6 dequeue):

- **W3:** `storage_allocate` now tries the free-list first
  (`dequeue_from_free_list`) before falling through to
  `storage_allocate_tail_advance`.
- **W5:** `storage_free` enqueues `(logical, phys)` into the free-list
  via `enqueue_free_page` (and validates on mount via
  `validate_free_list_on_mount`).
- **W6:** `dequeue_from_free_list` is retry-safe (1000-iteration cap)
  with CAS on the per-page count and the head/tail pointers.

The free-list is on-disk (linked pages chained via
`HDR_OFF_FREE_LIST_HEAD` / `_TAIL` / `_COUNT` in the storage header),
crash-safe (Phase 27 W5 payload CRC repair + free-list validator), and
thread-safe (CAS-based).

After `storage_free`, the indirection entry is 0 and the (logical, old_phys)
is on the free-list. The next `storage_allocate` pops it via
`dequeue_from_free_list` and returns the same logical page; the next
writer claims it via `try_claim_entry` (which writes a new physical offset
into the cleared indirection entry).

**Status:** RESOLVED. The free-list alloc policy is in production. No
rework needed for the Phase 28 per-bin-job GC; `storage_free` is the
correct primitive for freeing data pages.
