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

## TODO-12 — Pool Slot Freeing (deferred from Phase 28 first bin job) [PARTIALLY RESOLVED — chain slots fully resolved by Phase 28 W6 + Type 2, page-level reclamation deferred]

### Problem (historical)

The original pool allocator (`src/pool.c::pool_alloc`) only supported
allocation; there was no `pool_free` operation. Individual pool slots
were never freed back to the pool. The existing stop-the-world GC
handled this implicitly by shadow-compacting the entire pool into
new pages and freeing the old pool pages wholesale.

The new per-bin-job GC (Phase 28) frees individual slots (chain
slots — FileNode, FileContent, PageNode, FileSize, DirContent for
the create + tombstone removal) without rebuilding pool pages. We
needed a way to return freed slots to the pool's free list.

### Resolution (Phase 28 W6, commit `f1318ba`)

`pool_free(Pool* pool, int64_t slot_vp)` is implemented in
`src/pool.c` / declared in `src/pool.h`:

- O(1) per free, one CAS on the page's `poolState` + one write to
  the slot's bytes 0-1 to thread it onto the per-page free list.
- Bounded retry (`POOL_PUSH_MAX_RETRIES=1000`) on the per-page CAS,
  same pattern as `pool_alloc` and `bin_push`.
- Validates `page_index >= 2` (header + superblock never freed)
  and `slot_vp != VFS_VPTR_NULL`.
- On CAS success: marks the page dirty and decrements
  `pool->alloc_count` so `pool_alloc_count()` reflects the net
  allocation.
- Not idempotent: a double-free would corrupt the free list (slot
  appears twice, `freeCount` inflated). The bin job handles this
  via `drop_dir_entries` returning 1 (no-op) on re-run; the free
  is skipped in that case.

The file-deletion bin job (`gc_handle_file_deleted` in
`src/gc_bin_file_deleted.c`) calls `pool_free` on the tombstone
slot and the create slot (when distinct) only when the drop
actually removed entries (`drc == 0`).

Test coverage (`test/test_pool.c`, 7 new tests):

- `test_pool_free_basic` — alloc 5, free 5, re-alloc 5.
- `test_pool_free_lifo` — alloc 1, free 1, re-alloc returns same
  slot (LIFO invariant).
- `test_pool_free_lifo_two` — free slot A, then slot B; next
  alloc returns B (last freed = head of list).
- `test_pool_free_then_alloc` — free 0..19, alloc 20; verify
  alloc order is 0, 1, ..., 19 (LIFO push-pop, not FIFO).
- `test_pool_free_slot_reusable` — free slot, re-alloc, write
  distinct value, read back — verify no stale content.
- `test_pool_free_fill_page` — free all 255 slots, verify
  free_count == 255 and the firstFreeSlot chain is valid.
- `test_pool_free_invalid` — null VP, VP with `page_index < 2`,
  VP with no backing page — all return `VFS_ERR_IO`.

Plus 1 new test in `test/test_gc_thread.c`:
- `test_file_deletion_pool_slots_reused` — end-to-end: create +
  delete + GC + recreate. `pool_alloc_count` stays bounded
  (within a small slack for other GC allocations).

All 16/16 ctest pass; test_pool 35105/35105, test_gc_thread 163/163.

### Resolution (Phase 28 Type 2, commit `18cc501`) — OLD NameEntry leak fixed

The rename bin job (`gc_handle_rename_done` in
`src/gc_bin_rename_tombstone.c`) now also calls `pool_free` on
the OLD NameEntry's first chain slot.  The work handler
(`process_name_entry`) walks the chain via `ANCHOR_OFF_SIBPTR`
and frees each chain slot.

This was the second half of the original "chain slot leak"
problem: file deletion only freed the chain slots for the
create + tombstone, but rename was leaving the OLD NameEntry
chain orphaned in the pool.  With the rename bin job in
place, the "no space leak after GC" guarantee extends to
`vfs_rename` as well.

Per the W5 spec review (M1), no tree-wide lookup is needed for
the OLD NameEntry: each `vfs_create` calls `nodes_write_name`
which calls `pool_alloc` for fresh slots, so two creates for
the same name produce two different NameEntry VPs.  The OLD
NameEntry is referenced only by the create being freed.  A
defensive check verifies the slot content matches the
expected name to catch the theoretical VP-reuse case.

Multi-slot NameEntry chains (names > 16 bytes) are handled by
walking the chain via `ANCHOR_OFF_SIBPTR` from the first slot.
The spec's §4.4 covers the multi-slot case explicitly.

3 new tests in `test/test_gc_thread.c`:

- `test_rename_tombstone_bin_job_basic` — same-dir rename
  "foo" → "bar", verify OLD name not findable on remount,
  pool count bounded.
- `test_rename_tombstone_with_active_snapshot` — snapshot
  before rename, verify OLD name still findable at the
  snapshot, NEW name findable at the head.
- `test_rename_no_space_leak` — 20 renames, verify pool count
  bounded (delta < 2000, accounts for radix index and
  DirSegment allocations; the test verifies BOUNDED growth,
  not small absolute count).

### Remaining work — pool-page-level reclamation

The pool page itself is NOT freed even if all 255 slots become
free. A pool page holds 8 KB of indirection and remains in the
`pool.list_head` linked list forever, with the indirection
table occupying physical space.

Layered on top of `pool_free`: when a page's `freeCount` reaches
255, the page is fully empty and could be returned to the storage
layer (`storage_free` on the indirection entry + remove from
`pool.list_head`).

**Why deferred:** pre-MVP, no external users. 8 KB of leaked
indirection per fully-empty pool page is negligible against the
data pages themselves (multi-MB per deleted file). Will revisit
if/when pool-page counts become a meaningful fraction of total
pages.

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
