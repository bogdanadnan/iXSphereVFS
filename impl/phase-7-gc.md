# Phase 7: Garbage Collection

## Goal
Implement shadow-compaction garbage collection that physically removes
soft-deleted epochs, collapses committed epochs, and frees unreachable
pages. GC never mutates the live tree in-place — it builds a new tree
and atomically swaps the root. A deferred-free queue prevents races
between GC and the allocator.

---

## Workload 7.1 — Tree Lock

**What:** A global read-write lock stored in the superblock's `treeLockState`
field. Normal operations acquire a shared read lock; GC acquires an exclusive
write lock.

**Why:** GC must prevent concurrent modifications while it copies the tree.
A simple CAS-based lock with a reader count and a writer bit provides this
without a heavyweight OS mutex. The lock state persists in the superblock
so crash recovery can detect interrupted GC.

**How:**
- Lock layout in `treeLockState` (int64_t): bit 63 is the exclusive write
  lock (1 = GC active). Bits 32–62 are the reader count. Bits 0–31 are
  reserved.
- Reader acquisition: check bit 63. If set, GC is active — block (spin or
  yield) until it clears. If clear, CAS-increment the reader count in bits
  32–62. The check-and-increment MUST be atomic — use a CAS loop that
  re-checks bit 63 on each attempt.
- Reader release: atomically decrement the reader count.
- Writer acquisition (GC): CAS-set bit 63. Then spin until the reader count
  (bits 32–62) reaches zero — all pre-existing readers have exited. New
  readers are blocked because they check bit 63 before incrementing.
- Writer release: atomically clear bit 63.
- Crash recovery: on mount, if bit 63 is set, GC was interrupted. Read the
  alternate superblock (the one with the lower generation in the ping-pong
  pair) — it still points to the intact pre-GC tree. Discard the new
  superblock that was being built. If bit 63 is clear, unconditionally zero
  the reader count (any pre-crash readers have exited).
- The lock is stored in the superblock, so its state persists across crashes.
  The rule that GC writes the new superblock with `treeLockState = 0` ensures
  that bit 63 is only ever set in the OLD superblock — the one being replaced.

**Acceptance:**
  - Reader can acquire when bit 63 is clear.
  - Reader blocks when bit 63 is set.
  - Writer acquires after all readers drain; new readers block during write.
  - After simulated crash during GC (bit 63 set on the old superblock):
    mount discards the new superblock and uses the old one.
  - After normal shutdown: `treeLockState` is 0.

---

## Workload 7.2 — Shadow Compaction

**What:** Walk the entire tree from the root, copy all surviving entries into
new pool pages, build new bitmap state, and atomically swap the superblock
to the new tree.

**Why:** Soft-deleted epochs accumulate dead entries (VersionPages, DirContents,
TouchedFiles) that waste pool slots and data pages. GC reclaims this space
by copying only live entries into fresh pages. Doing this as a copy-then-swap
operation means a crash during GC leaves the old tree perfectly intact.

**How:**
- `vfs_gc(vfs)`:
  1. Acquire the tree write lock (Workload 7.1).
  2. Walk the tree from the root DirNode. Traverse all DirContent chains,
     follow `childPtr` links to child nodes, walk FileContent chains, PageNode
     chains, VersionPage chains.
  3. For each entry type, determine whether it survives:
     - **VersionPage**: drop if its epoch belongs to a soft-deleted epoch (per
       the epoch mapper, `traversalApply = false`). If its epoch equals a
       committed snapshot epoch S, rewrite its epoch to S+1 (the live head
       at commit time) and keep it. Otherwise keep it unchanged. All kept
       VersionPages are written sequentially into new pool pages.
     - **DirContent**: drop if its epoch belongs to a deleted epoch AND
       no surviving entry for the same `childNodeId` exists at a higher
       epoch ≤ the live head. For deletion entries (`namePtr = 0`) in
       deleted epochs, drop them — the baseline entry at the previous
       epoch provides the correct state.
     - **FileSize**: drop entries from soft-deleted epochs (file falls back
       to baseline size). For committed epochs, rewrite epoch to the live
       head and keep. Drop FileContent segments beyond the highest surviving
       FileSize bound.
     - **TouchedFile**: drop all entries for soft-deleted and committed
       epochs — the chains are rebuilt from scratch for active epochs only.
     - **Mapper entries**: drop entries for soft-deleted and committed
       epochs. Collapse committed mappings (remove the mapping — version
       nodes have already been relabeled).
  4. Build new pool pages: as entries survive the filter, allocate new pool
     pages and write them sequentially. This packs surviving entries densely
     and eliminates fragmentation.
  5. Build new bitmap state: the new tree references a known set of data pages
     and pool pages. Build a new bitmap from scratch marking all reachable
     pages as allocated. All other pages become free.
  6. Write new superblock: include the new `rootNodeOffset`, `currentEpoch`,
     `epochMapperPtr`, `poolListHead`, and `treeLockState = 0`. fsync the
     new superblock.
  7. Atomically swap to the new superblock (write to the inactive half of the
     ping-pong superblock page, increment generation).
  8. Release the tree lock.
  9. Place all pages from the old tree into the deferred-free queue
     (Workload 7.3).

**Acceptance:**
  - Create a file, take snapshot, write more data, soft-delete the snapshot,
    run GC. Verify that the soft-deleted data pages are freed and the file
    reverts to its pre-snapshot size.
  - Commit a snapshot, run GC. Verify that committed version nodes are
    relabeled to the live head epoch and the mapper entry is removed.
  - Kill the process during GC (before the swap): on remount, the old tree
    is intact and no corruption has occurred.
  - Kill the process after the swap but before deferred-free completes: the
    new tree is active and consistent.
  - GC reduces the number of pool pages when dead entries are removed.

---

## Workload 7.3 — Deferred-Free Queue

**What:** A queue of pages from the old tree that GC has marked for deletion
but not yet returned to the allocator. The allocator skips pages in this
queue. Pages are freed only after GC confirms no active traversals reference
them.

**Why:** Without deferred-free, a page freed by GC could be immediately
reallocated and reused by a concurrent writer while GC is still walking old
tree branches that reference that page. This would cause the GC thread to
read mutated data. The deferred-free queue decouples "marked for deletion"
from "available for reuse."

**How:**
- The queue is a linked list of page indices (not pool-allocated — a simple
  in-memory structure protected by the tree lock during population, then
  lock-free for reads).
- During GC step 9: for every page in the old tree (data pages, pool pages,
  bitmap pages), if it is not reachable from the new tree, append its logical
  page index to the deferred-free queue. Data pages are freed as pairs (mirror
  sibling included).
- The allocator (`Allocate`, `Acquire`): before returning a page, check
  whether it is in the deferred-free queue. If yes, skip it — do not allocate
  it. The check is O(1) via a hash set or bitmap overlay of the queue.
- After GC confirms no active traversals (all pre-GC readers have exited,
  verified by the tree lock's reader count reaching zero AND the new superblock
  swap having completed), the pages in the deferred-free queue are actually
  freed via `Free(page)`. The queue is then cleared.
- This confirmation can happen at the next GC invocation, or lazily after the
  current GC's tree lock release. The key invariant: pages enter the queue
  before the allocator can return them, and leave the queue after all potential
  readers of the old tree have finished.

**Acceptance:**
  - After GC, allocate pages: none of the returned pages were in the old tree.
  - A page marked for deferred-free is not returned by `Allocate` or `Acquire`
    until it is actually freed.
  - The deferred-free queue is empty after a complete GC cycle where no
    concurrent readers were active.

---

## Workload 7.4 — Pool Page Rebuild

**What:** During GC, pool pages are rebuilt from scratch by copying surviving
entries sequentially into new pages. This eliminates fragmentation and returns
full pages to the free pool.

**Why:** Pool pages accumulate dead entries (freed DirContents from deleted
files, VersionPages from soft-deleted epochs, etc.). Without compaction,
the allocator would eventually run out of free slots even though many slots
are logically dead. Rebuilding packs surviving entries densely.

**How:**
- During the tree walk in Workload 7.2, as each surviving entry is identified,
  it is copied into a new pool page's next available slot. When the page
  fills (255 slots), allocate a new pool page and continue.
- The new pool pages are linked via `nextPoolPage` in the order they are
  filled. The new `poolListHead` in the superblock points to the first new
  page.
- After the swap, the old pool pages are placed in the deferred-free queue.
- This is the only time pool slots are freed — individual slots are never
  freed during normal operation. The pool is a monotonic allocator between
  GC cycles.
- The pool free list (`poolState`) of each new page is initialized normally
  (Workload 3.2). After all surviving entries are copied, remaining free slots
  in the last page are available for allocation.

**Acceptance:**
  - After GC, pool pages contain only surviving entries, packed sequentially.
  - A pool page that was 50% full before GC with dead entries is rebuilt
    and becomes 100% full of live entries (or the live entries are spread
    across fewer pages).
  - The `poolListHead` after GC points to the first new pool page.

---

## Deliverables

| File | Purpose |
|------|---------|
| `src/gc.c` | Tree lock, shadow compaction, pool page rebuild |
| `src/deferred_free.c` | Deferred-free queue with allocator integration |
| `test/test_gc.c` | GC correctness, crash recovery, deferred-free isolation |

## Success Criteria
- GC reclaims dead pages after soft-delete: pool slot count and data page
  count decrease.
- GC correctly handles committed epochs: version nodes relabeled, mapper
  entry removed.
- Crash during GC (before swap): old tree intact on remount.
- Crash during GC (after swap, before deferred-free): new tree active, old
  pages in deferred-free queue.
- Deferred-free queue prevents allocator from returning pages that GC has
  not yet confirmed safe.
- GC runs to completion without corrupting data under a read-only workload
  (concurrent readers hold the tree lock in shared mode, GC waits for them
  to finish).
