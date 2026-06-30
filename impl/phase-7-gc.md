# Phase 7: Garbage Collection

## Goal
Implement shadow-compaction garbage collection that physically removes
soft-deleted epochs, collapses committed epochs, and reclaims unreachable
pages. GC never mutates the live tree in-place — it builds a new tree and
atomically swaps the root. A deferred-free queue prevents races between GC
and the allocator.

## Non-Negotiable Constraints

- **Must not corrupt data.** The old tree must remain fully intact and
  readable until the new superblock is atomically swapped. A `kill -9` during
  GC must leave the old tree recoverable.
- **Must not deadlock with readers.** The tree lock allows existing readers
  to finish before GC acquires the exclusive lock, and blocks new readers
  while GC holds it.
- **Deferred-free must prevent ABA.** Pages freed by GC must not be returned
  by `Allocate` until GC confirms no active traversal references them.
- **Pool pages rebuilt sequentially.** Surviving entries are packed densely
  into new pool pages. Dead entries are dropped. Fragmentation eliminated.
- **Indirection table refreshed.** All entries for unreachable pages set to 0.
  No physical compaction of data pages.

## File Organization

| File | Purpose |
|------|---------|
| `src/gc.c` | Tree lock, shadow compaction, pool page rebuild |
| `src/deferred_free.c` | Deferred-free queue with allocator integration |
| `test/test_gc.c` | GC correctness, crash recovery, deferred-free isolation |

## Dependencies
- Phase 2 (StorageBackend) for `Allocate`, `Free`, `Read`, `Write`, `Flush`
- Phase 3 (Pool) for slot allocation
- Phase 4 (Node Types) for all layout helpers
- Phase 5 (Tree Operations) for tree walking
- Phase 6 (Epoch System) for mapper access

---

## Workload 7.1 — Tree Lock

### What
A global read-write lock stored in the superblock's `treeLockState` field.
Normal operations acquire a shared read lock. GC acquires an exclusive write
lock. The lock state persists in the superblock so crash recovery can detect
interrupted GC.

### Bit Layout (treeLockState is int64_t)
```
bit 63:        exclusive write lock (1 = GC active)
bits 62–32:    reader count (31 bits, max ~2 billion readers)
bits 31–0:     reserved (unused, always 0)
```

### `tree_lock_acquire_shared(superblock)`

```
loop:
    state = atomic_load_i64(&superblock->treeLockState)
    if state & (1ULL << 63):  // bit 63 set → GC active
        sched_yield() or usleep(10)
        continue
    reader_count = (state >> 32) & 0x7FFFFFFF
    new_state = ((reader_count + 1) << 32) | (state & 0xFFFFFFFF)
    if CAS(&superblock->treeLockState, state, new_state) == state:
        return  // acquired
    // CAS failed → state changed, retry
```

### `tree_lock_release_shared(superblock)`

```
atomic_add_i64(&superblock->treeLockState, -(1LL << 32))
// Decrements reader count by 1. Ignore the reserved bits.
```

### `tree_lock_acquire_exclusive(superblock)`

```
// Set bit 63
loop:
    state = atomic_load_i64(&superblock->treeLockState)
    if state & (1ULL << 63):
        // Already locked — should not happen (only GC calls this)
        return ERROR_ALREADY_LOCKED
    if CAS(&superblock->treeLockState, state, state | (1ULL << 63)) == state:
        break

// Wait for all readers to drain
loop:
    state = atomic_load_i64(&superblock->treeLockState)
    reader_count = (state >> 32) & 0x7FFFFFFF
    if reader_count == 0:
        return  // exclusive lock held, no readers
    sched_yield() or usleep(10)
```

### `tree_lock_release_exclusive(superblock)`

```
atomic_store_i64(&superblock->treeLockState, 0)
// Clears both bit 63 and the reader count. Reader count is 0 at this point.
```

### Crash Recovery (Mount-time, Phase 5 bootstrap)

```
state = superblock->treeLockState
if state & (1ULL << 63):
    // GC was interrupted. The NEW superblock (being built) may be
    // partially written. The OLD superblock (lower generation in the
    // lazy mirror pair) is intact. Discard this superblock, use the
    // alternate half.
    use_alternate_superblock()
else:
    // Normal shutdown or crash without GC. Reader count is stale.
    // Zero it unconditionally (all pre-crash readers are gone).
    superblock->treeLockState = 0
```

### Acceptance
- [ ] Reader acquires when bit 63 clear
- [ ] Reader blocks (spins) when bit 63 set
- [ ] Writer (GC) acquires after all readers drain
- [ ] `kill -9` during GC (bit 63 set) → remount discards new superblock,
  uses old one → tree intact
- [ ] After normal close: treeLockState = 0

---

## Workload 7.2 — Shadow Compaction

### What
Walk the entire tree, copy surviving entries into new pool pages, rebuild the
indirection table, write a new superblock, and atomically swap.

### `vfs_gc(vfs) → VFS_OK`

This is a blocking, manual operation. No other operations may proceed while
it runs.

```
1. tree_lock_acquire_exclusive(superblock)    // Workload 7.1
2. Allocate a new list head for pool pages (gc_pool_list_head = 0 initially)
3. Allocate a new superblock buffer (8KB, zero-filled)
4. Walk the tree from rootNodeOffset:
   a. For each DirNode → for each DirContent (walk chain, apply survival rules)
   b. For each FileNode → for each FileContent → for each PageNode → for each VersionPage
   c. For each surviving entry: copy it into a new pool page
      - When current new pool page fills (255 slots): allocate a new one,
        link into gc_pool_list_head, and continue
5. Build new indirection table:
   - All reachable logical pages (data pages, pool pages, superblock, header)
     keep their physical offsets
   - All unreachable logical pages: entries set to 0
6. Build new epoch mapper chain:
   - Drop entries for soft-deleted epochs
   - Drop entries for committed epochs (their version nodes were relabeled in step 4)
   - Rebuild chain from surviving entries
7. Write new superblock buffer:
   - rootNodeOffset = VirtualPtr of root DirNode (copied in step 4)
   - currentEpoch = current value (unchanged)
   - epochMapperPtr = VirtualPtr of new mapper chain head
   - poolListHead = gc_pool_list_head
   - treeLockState = 0  ← critical: lock released in the new superblock
   - nextNodeId = current value
   - touchedFilesPtr = 0 (rebuilt from scratch for active epochs only)
8. Write new superblock to its logical page:
   storage_write(sb, SUPERBLOCK_PAGE, new_superblock_buffer)
   storage_flush(sb, -1)    // flush all dirty pages, fsync
9. Atomically swap to new superblock:
   - The lazy mirror mechanism handles this: writing the superblock
     increments its generation. The old superblock has lower generation.
     No explicit "swap" needed — it's just a Write.
10. tree_lock_release_exclusive(superblock)
11. Place old tree pages into deferred-free queue (Workload 7.3)
12. Return VFS_OK
```

### Survival Rules

For each entry encountered during the tree walk, decide whether to keep or drop:

| Entry Type | Condition | Action |
|---|---|---|
| VersionPage | epoch belongs to soft-deleted epoch (mapper has entry with traversalApply=false) | DROP |
| VersionPage | epoch equals committed snapshot S | REWRITE epoch to S+1, KEEP |
| VersionPage | any other epoch | KEEP unchanged |
| DirContent | epoch belongs to deleted epoch AND no surviving entry for same childNodeId at higher epoch ≤ live head | DROP |
| DirContent | namePtr=0 AND epoch belongs to deleted epoch | DROP (tombstone no longer needed) |
| DirContent | any other | KEEP |
| FileSize | epoch belongs to soft-deleted epoch | DROP (falls back to baseline) |
| FileSize | epoch belongs to committed epoch | REWRITE epoch, KEEP |
| FileContent | segment beyond highest surviving FileSize bound | DROP |
| TouchedFile | all entries | DROP all (rebuilt for active epochs) |
| MapperEntry | fromEpoch belongs to committed or soft-deleted epoch | DROP |

### Acceptance
- [ ] Create file, snapshot, write more, soft-delete snapshot → GC → file
  reverts to pre-snapshot size, dead data pages freed
- [ ] Commit snapshot → GC → committed version nodes relabeled to live head
  epoch, mapper entry removed
- [ ] `kill -9` during GC (before step 9) → remount → old tree intact
- [ ] `kill -9` after step 9 before deferred-free → new tree active, old pages
  in queue
- [ ] GC reduces pool page count when dead entries exceed a page worth

---

## Workload 7.3 — Deferred-Free Queue

### What
A queue of old-tree page indices that GC marks for deletion but doesn't
immediately return to the allocator. Prevents ABA: the allocator must not
reuse a page while GC still references it.

### Data Structure

```c
typedef struct {
    int64_t* pages;      // dynamic array of logical page indices
    int      count;
    int      capacity;
    bool     confirmed;  // true when no active traversals remain
} DeferredFreeQueue;
```

### `deferred_free_enqueue(logical_page)`

```
1. Lock the queue (simple mutex — this is called only during GC, not hot path).
2. Append logical_page to pages[].
3. If the page has mirrorPage != -1: also enqueue the sibling.
4. Unlock.
```

### `deferred_free_is_queued(logical_page) → bool`

Called by `Allocate` and `Acquire` before returning a page.

```
1. Lock queue.
2. Linear scan for logical_page. (Queue is typically small — hundreds, not millions.)
3. Unlock, return found.
```

### `deferred_free_confirm_and_release()`

Called after GC confirms no active traversals. This can happen at the next GC
invocation or after the current GC's tree lock release and a grace period.

```
1. Lock queue.
2. For each page in pages[]: Free(page) via StorageBackend.
3. Clear pages[], reset count, set confirmed = true.
4. Unlock.
```

### Acceptance
- [ ] After GC: `Allocate` never returns a page that is in the deferred-free queue
- [ ] After deferred-free is confirmed and released: pages are available for
  allocation again
- [ ] Queue is empty after complete GC cycle with no concurrent readers

---

## Workload 7.4 — Pool Page Rebuild

### What
During GC step 4, pool pages are rebuilt from scratch. Surviving entries are
copied sequentially into new pages. Dead entries are simply not copied.
Result: dense packing, zero fragmentation.

### Algorithm

```
// Part of GC step 4
current_new_page = pool_alloc_new_page()  // allocate + init free list + write header
slots_used = 0
gc_pool_list_head = current_new_page

for each surviving entry E in tree walk:
    slot = pool_get_slot(current_new_page, slots_used)
    memcpy(slot, E->data, 32)  // copy the 32-byte pool entry
    // Adjust any VirtualPtrs in E that point to old pool pages?
    // NO — VirtualPtrs reference LOGICAL pool pages. The GC builds
    // new logical pages but the old logical pages still exist
    // (in deferred-free queue). VirtualPtrs remain valid because
    // they reference logical pages that are still allocated.
    // Only after deferred-free confirm are the old pages freed,
    // and by then no VirtualPtrs reference them.
    slots_used++
    if slots_used == 255:
        new_page = pool_alloc_new_page()
        new_page->nextPoolPage = current_new_page  // LIFO
        current_new_page = new_page
        gc_pool_list_head = current_new_page
        slots_used = 0

// After all entries copied:
// The last new page has free slots (255 - slots_used_free).
// Its poolState is set to (free_slots << 16) | slots_used (first free = after last used).
// Remaining free slots' next-pointer chain is initialized normally.
```

### Wait — VirtualPtr Adjustment

VirtualPtrs stored in pool entries encode `(logical_page_index << 8) | slot_index`.
During GC, the ENTRY MOVES to a new logical page and new slot. So VirtualPtrs
inside pool entries that point to OTHER pool entries must be UPDATED.

**This is the key complexity of GC.** For every entry copied:
1. Scan the 32-byte entry for any VirtualPtr fields (they are at known offsets
   depending on the entry type).
2. If a VirtualPtr references a pool entry that also moved: update it to the
   new (logical_page, slot) location.
3. Maintain a mapping: `old_vptr → new_vptr` during the copy phase. A hash
   table. Before writing a copied entry, rewrite all its VirtualPtrs using
   this mapping.

This mapping is the GC's primary data structure. It is built on-the-fly:
when an entry is copied from old location `old_vp` to new location `new_vp`,
add `{old_vp → new_vp}` to the mapping. When scanning VirtualPtrs in another
entry, if the target `old_vp` is in the mapping, replace it with `new_vp`.

Entries that are DROPPED (not copied) are NOT added to the mapping. Any
VirtualPtr that references a dropped entry becomes 0 (null). This is correct:
dropped VersionPages disappear from chains; the chain terminates earlier.

### Acceptance
- [ ] After GC: pool pages contain only surviving entries, packed sequentially
- [ ] VirtualPtrs in copied entries correctly point to relocated entries
- [ ] Dropped entries cause chain termination (nextPtr becomes 0)
- [ ] `poolListHead` after GC points to first new page
- [ ] All old pool pages in deferred-free queue

---

## Final Phase 7 Checklist

- [ ] GC reclaims dead pages after soft-delete: pool slots freed, data pages freed
- [ ] GC correctly relabels committed version nodes
- [ ] `kill -9` before swap: old tree intact on remount
- [ ] `kill -9` after swap, before deferred-free: new tree active
- [ ] Deferred-free prevents allocator from returning queued pages
- [ ] VirtualPtr remapping works correctly for all entry types
- [ ] No double-frees, no leaked pages (Valgrind/ASan clean after GC cycle)
