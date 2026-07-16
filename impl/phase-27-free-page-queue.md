# Phase 27 (Follow-up): Free-Page Queue (H2 + H3 Fix)

**Revision:** 3 (addresses the review in `PHASE27_FREEPAGE_REVIEW_V2.md`).
Changes from revision 2: Q1 (remove the stale "tagged pointer"
ABA text in the Concurrency section), Q2 (ABA detection now uses
`try_claim_entry` inside `dequeue_from_free_list`, not caller-side
check), Q3 (document the overflow-page dirty issue as best-effort).

Changes from revision 1 are summarized at the bottom of this spec
for the curious reader; the V2 review is the canonical iteration
artifact.

## Goal

Make freed pages reusable. Replace today's `storage_free` (which sets
`indir[page] = 0` and is only called by GC) with a **persisted,
lock-free, FIFO free-page queue** that:

- Is populated by `storage_free` (the source of free pages is now GC
  AND future callers — including the H2 fix in `mirror_write`).
- Is consumed by `storage_allocate(1)` (the hot path). Pages from the
  queue are preferred over the tail-advance when available, so a
  free-and-allocate pair costs zero net pages.
- Survives `vfs_unmount` / `vfs_mount`: a page freed before unmount
  is reused on the next mount without growing `total_pages`.
- Adds ≤ 1 atomic load to the `storage_allocate(1)` hot path when
  the queue is empty (the common case).
- Closes **H2** (`mirror_write` second-write torn sibling — orphan
  becomes a free-page-queue entry instead of a permanent leak) and
  **H3** (`storage_allocate(1)` ignores the deferred-free queue
  — the queue is now consulted on the hot path, so H3's complaint
  no longer applies).

The migration is split into **4 workloads** within this spec. Each is
self-contained, testable, benched.

## Terminology clarification

The design uses a **page-level FIFO with page-local LIFO** queue:

- **Across pages**: the head free-list page is the oldest (FIFO).
  Pages are appended at the tail (newest end). This gives the
  cleanest crash recovery (the head is stable; the tail is the
  write target).
- **Within a page**: entries are popped from the "top" (last
  appended) for cache locality — the most recently appended entry
  is hot in the cache.

Operations:
- `enqueue` (storage_free) — appends a (logical, physical) entry
  to the **tail** page's slot.
- `dequeue` (storage_allocate(1)) — pops the **last** entry from
  the **head** page's slot.

(If a true pure-LIFO design is preferred for any reason, flip the
in-page convention in W1. The on-disk layout and concurrency
model are the same either way.)

## Background

### Current `storage_allocate(1)` (the hot path)

`src/storage.c::storage_allocate`, the tail-advance comment:
> **Tail-advance fast path.** Assumption: every newly allocated page
> gets the next available index at the tail of the indirection table,
> so the first free slot is exactly `sb->total_pages`.  This holds as
> long as `storage_free` is never called during normal operation —
> and currently it isn't (only GC calls it).
>
> TODO: when GC reclaims mid-table slots, switch to a free-list
> allocation policy.

The hot path is:
```c
int64_t old_total = sb->total_pages;
int64_t new_total = old_total + 1;
while (vfs_cas_i64(&sb->total_pages, old_total, new_total) != old_total) {
    old_total = sb->total_pages;
    new_total = old_total + 1;
}
return old_total;  // the newly-claimed logical page
```

This is correct, lock-free, and very fast (3 atomic loads + 1 CAS).
The bug is: `total_pages` only ever grows. A freed page is invisible
to the allocator because:
1. The free list is not consulted.
2. The indirection entry is set to 0, but the tail-advance just
   bumps past it.

### Current `storage_free`

`src/storage.c:625`:
```c
void storage_free(StorageBackend* sb, int64_t logical_page) {
    if (logical_page < 2) return;  /* don't free header or superblock */
    indir_set(sb, logical_page, 0);
}
```

One atomic store. The freed slot is now "logically free" (indir = 0,
so reads return NOT_FOUND) but is **never reused** by the hot path.
The only way it can be reused today is via `storage_allocate_count_scan`
(count > 1 fallback), which scans the indirection table for `indir == 0`.
That path is not exercised by the hot path (only `pool_alloc` and
`mirror_write` call `storage_allocate(1)`).

### Why now

Two bugs trace to the absence of a real free list:

- **H2** (`mirror_write` second-write torn sibling): on link failure,
  the orphan sibling is in `indir` (so the next allocator doesn't
  see it as free) but is unreferenced (so it sits on disk forever).
  With a free list, the orphan can be `storage_free`'d, and the
  next `storage_allocate(1)` reuses the orphan's slot instead of
  bumping `total_pages` past it.

- **H3** (`storage_allocate` count==1 fast path ignores
  deferred-free queue): the deferred-free queue is consulted by
  `storage_allocate_count_scan` (count > 1 fallback) but not by
  the hot path. With a real free list consulted by the hot path,
  H3 is structurally closed — the deferred-free queue and the
  free list are the same structure.

## Current state

`StorageBackend` (`src/storage.h:90-104`):
```c
typedef struct StorageBackend {
    int              fd;
    int64_t          total_pages;
    int64_t          page_size;
    uint32_t         segment_size;
    int64_t          physical_tail;
    int64_t          indirection_head;
    IndirectionTable indir;
    PageCache        cache;
    uint8_t*         header_buf;
} StorageBackend;
```

`indir_set` is one atomic store; `indir_lookup` is one non-atomic
read (aligned int64_t, atomic on x86/ARMv8). The header page payload
holds the indirection inline entries at offset 40.

The hot path is the tail-advance (~10 ns per call). H2 + H3 are open
in ISSUES.md.

## Proposed state

### Data layout

**In the header page**: 3 new fields (24 bytes total) are placed at
**the start of the inline indirection area** (offsets 40, 48, 56).
The inline indirection table shifts from `header_buf + 40` to
`header_buf + 64`, and `inline_count` is reduced by 3 (from
`(page_size - 40) / 8` to `(page_size - 64) / 8`):

```
Offset  Size  Field
0       8     total_pages                    (unchanged)
8       8     page_size                      (unchanged)
16      4     segment_size                   (unchanged)
20      4     (padding)                      (unchanged — was 4 unused bytes)
24      8     physical_tail                  (unchanged)
32      8     indirection_head               (unchanged)
40      8     free_list_head       (NEW — logical page of first free-list page, 0 = empty)
48      8     free_list_tail       (NEW — logical page of last free-list page,  0 = empty)
56      8     free_list_count      (NEW — total free pages across all free-list pages)
64      8*N   indirection inline entries     (SHIFTED from offset 40; N = inline_count - 3)
```

For an 8 KB page, `inline_count` goes from 1022 to 1019 (3 fewer
inline entries). For 4 KB: 510 → 507. Negligible — the overflow
chain takes over beyond `inline_count`, so this only delays overflow
allocation by 3 page writes.

**The indirection code change is mechanical**: `indir_init` (or
wherever `inline_entries` is set) now does
`it->inline_entries = sb->header_buf + 64` instead of
`sb->header_buf + 40`, and `inline_count` is computed as
`(page_size - 64) / 8`. The lookup/set functions don't need to
change — they access `inline_entries[logical_page]`, which now
correctly maps logical page 0 to the entry at offset 64 (not 40).

`free_list_count` enables the hot path to do a single atomic load and
decide "queue is empty, fall through to tail-advance" without touching
any other memory.

**A free-list page** (regular storage page, allocated like any other
data page — uses `storage_allocate(sb, 1)`):
```
Offset  Size  Field
0       8     next_page      (logical page of next free-list page, 0 = end)
8       4     count          (number of free pages in this page's array)
12      4     (padding — 8-byte alignment for the array)
16      16*N  entries[N]     (each entry: 8-byte logical VP + 8-byte physical offset)
```

For an 8 KB page: `N = (8192 - 16) / 16 = 510` free pages per page.
For a 4 KB page: `N = (4096 - 16) / 16 = 255`.

So the queue holds up to 510 free pages per page of metadata.
Beyond that, the chain extends via `next_page`. 100k free pages
needs ~197 free-list pages = ~1.6 MB of metadata. Well under any
realistic worst case.

### Operation: `enqueue` (storage_free)

**Input**: `logical_page` to be freed (assumed not currently in the
queue).

**Algorithm**:
```c
void storage_free(StorageBackend* sb, int64_t logical_page) {
    if (logical_page < 2) return;

    // 1. Read the current tail and tail_count atomically.
    int64_t tail = vfs_atomic_load_i64(HDR_OFF_FREE_LIST_TAIL);
    int     tail_count = (tail != 0)
        ? read_tail_count(sb, tail)  // CAS-loop read
        : 0;

    // 2. If tail is null OR tail_count == MAX_PER_PAGE, allocate a
    //    new free-list page. This is a rare path (only when the
    //    queue is empty, or every 510 frees). Use the internal
    //    tail-advance helper to avoid the producer-consumer cycle
    //    (see R6 in the review; storage_allocate(1) consults the
    //    free list, which we're trying to extend).
    int64_t new_tail;
    if (tail == 0 || tail_count >= MAX_PER_PAGE) {
        new_tail = storage_allocate_tail_advance(sb, 1);
        // ... link as new tail, update head if queue was empty ...
    } else {
        new_tail = tail;
    }

    // 3. Append logical_page to the new tail's free_pages array at
    //    position [tail_count]. CAS the count from tail_count to
    //    tail_count+1, retry if another thread won the race.

    // 4. CAS the global count from N to N+1. (N is the value
    //    observed in step 1, or re-read under contention.)

    // 5. Mark the free-list page dirty (so the change is flushed).

    // 6. Set indir[logical_page] = 0.
}
```

The CAS on `count` (step 3) is the per-page append race resolution.
The CAS on the global count (step 4) is optional — we can also just
defer the global count update to the next `dequeue` (read from
the head page's count directly). This is faster but introduces a
slight accounting lag. The spec uses the simpler approach (always
update the global count).

**Concurrency**: `storage_free` can be called concurrently from
multiple threads (e.g., GC + mirror_write after H2 fix). The CAS
on the count resolves the append race. The CAS on `free_list_tail`
or the linking of a new free-list page uses a per-page CAS — only
one thread succeeds in linking the new tail, others see the new
tail and append to it.

**Failure modes**: if `storage_allocate` fails (step 2, e.g., disk
full), the free is dropped. The page is still marked `indir=0`
(step 6), so the logical free happens, but the page isn't in the
queue. The next `storage_allocate(1)` would tail-advance past it.
The on-disk space is "lost" in this rare case (disk full + free
race), but no data corruption.

### Operation: `dequeue` (storage_allocate(1) fast path addition)

**Current hot path** is the tail-advance (3 atomic loads + 1 CAS).
**New addition** is one atomic load at the top:

```c
int64_t storage_allocate(StorageBackend* sb, int count) {
    if (count == 1) {
        // Try the free-list first.
        int64_t count_global = vfs_atomic_load_i64(HDR_OFF_FREE_LIST_COUNT);
        if (count_global > 0) {
            int64_t page = dequeue_from_free_list(sb);  // ~5 atomic ops
            if (page != 0) return page;
            // dequeue lost a race (queue drained between the load
            // and the dequeue). Fall through to tail-advance.
        }
        // Original tail-advance fast path.
        ...
    }
    // ... count > 1 fallback (unchanged) ...
}
```

The `dequeue_from_free_list` operation:
```c
static int64_t dequeue_from_free_list(StorageBackend* sb) {
    // 1. Read head.
    int64_t head = vfs_atomic_load_i64(HDR_OFF_FREE_LIST_HEAD);
    if (head == 0) return 0;  // queue is empty (race with another dequeue)

    // 2. Read the head page's count, with CAS-retry.
    int head_count = read_head_count(sb, head);
    if (head_count == 0) {
        // Head page is empty (just drained by another thread).
        // Advance head to head->next. The drained head page's
        // physical slot is no longer needed — call internal_free
        // to set indir=0 without enqueuing. The drained page is
        // effectively "leaked" from the queue's perspective; the
        // next storage_free on that VP will re-enqueue it. This
        // leak is bounded: 1 free-list page per 510 dequeues.
        int64_t next = read_next(sb, head);
        if (vfs_cas_i64(HDR_OFF_FREE_LIST_HEAD, head, next) == head) {
            // We won the advance. The drained head's slot is now
            // free; indir_set(head, 0) marks it reusable. Note:
            // this happens at most once per 510 dequeues, so the
            // cost is negligible.
            //
            // Q3 caveat: if `head`'s indir entry lives in an
            // overflow page, indir_set does not mark the overflow
            // page dirty (the overflow path in indirection.c
            // does an atomic store without cache_mark_dirty — a
            // pre-existing M12 residual). The indir=0 write is
            // best-effort in that case; the drained page won't
            // be reused until the next storage_free re-enqueues
            // it. Acceptable: the next re-enqueue triggers an
            // indir_set on the same slot (still 0), and the
            // eventual flush catches the change.
            indir_set(sb, head, 0);
        }
        return 0;  // caller falls through to tail-advance
    }

    // 3. Pop the LAST entry from the head page (page-local LIFO
    //    for cache locality — the most recently appended entry is
    //    hot in the cache).
    int     pop_idx  = head_count - 1;
    int64_t page     = read_entry_logical(sb, head, pop_idx);
    int64_t phys     = read_entry_physical(sb, head, pop_idx);
    // CAS the count from head_count to head_count - 1.
    if (vfs_cas_i32(HEAD_COUNT_OFFSET, head_count, head_count - 1) != head_count) {
        return 0;  // lost the race; caller falls through
    }

    // 4. Decrement the global count.
    vfs_atomic_add_i64(HDR_OFF_FREE_LIST_COUNT, -1);

    // 5. If head_count was 1, the page is now empty. Advance head
    //    to head->next (same as step 2). Mark the drained head as
    //    free via internal_free.

    // 6. Invalidate the cache entry for the dequeued page. The
    //    old data in the cache is stale (the page is logically
    //    free; the new indir entry points to a new physical
    //    offset that the caller will write to). The cache
    //    invalidation prevents serving stale data on a subsequent
    //    read before the caller's write completes.

    // 7. CLAIM the page by CASing indir[page] from 0 to phys.
    //    This is the ABA detection: if another thread already
    //    claimed this page (indir is non-zero), the CAS fails.
    //    We return 0 to the caller, who retries storage_allocate(1);
    //    the retry will tail-advance past this VP (it's still in
    //    indir from the other thread's claim) and get a fresh VP.
    //
    //    try_claim_entry is the existing helper used by
    //    storage_allocate(1)'s tail-advance (storage.c:442). It
    //    does a CAS of indir[vp] from 0 to physical_offset. The
    //    same primitive, same atomicity guarantees.
    if (!try_claim_entry(sb, page, phys)) {
        // ABA: another thread already claimed this page. The
        // entry was already removed from the queue (count
        // decremented). The caller will retry and tail-advance.
        return 0;
    }

    return page;
}
```

**Each entry stores BOTH the logical VP and the physical offset**:
on enqueue, the physical offset is recorded alongside the logical
VP. On dequeue, the same physical offset is restored. This avoids
the "old physical slot is leaked" problem that would occur if the
dequeue allocated a new physical offset — every dequeue would leak
one physical slot, and the leak rate would match the allocation
rate (1:1). With the offset preserved, no physical slot is leaked
on the dequeue path. The only physical-slot leaks in the design are
the drained free-list head pages (1 per 510 dequeues, as noted in
step 2).

**Enqueue** records both. Storage_free does:
```c
int64_t phys = indir_lookup(sb, logical_page);
indir_set(sb, logical_page, 0);
enqueue(sb, logical_page, phys);
```

And the dequeue reverses the indirection by writing the recorded
phys back:
```c
indir_set(sb, page, phys);  // re-establish indirection on dequeue
```

**ABA on the head pointer**: the spec uses a single 8-byte head
pointer (no tagged/generation). The chain is forward-only (new
free-list pages are appended at the tail; the head only advances
toward newer pages). The head pointer is monotonic, so classic
ABA (H1 → H2 → H1) requires H1 to be completely drained, removed
from the chain, recycled as a data page or a new free-list page,
and then become the head again — a very narrow window. The cost
of a successful ABA is "duplicate VP allocation" (two callers get
the same logical page from the queue).

**Mitigation**: ABA detection inside `dequeue_from_free_list` via
the existing `try_claim_entry` helper. After popping the entry
from the queue, the dequeue CASes `indir[page]` from 0 to the
recorded physical offset. If another thread already claimed this
page (the ABA case), the CAS fails — `indir` is non-zero because
the other thread's claim set it. The dequeue returns 0; the
caller's `storage_allocate(1)` retry will tail-advance past this
VP (it's still in indir from the other thread's claim) and get a
fresh VP.

This is the same CAS-based claim that the existing tail-advance
path uses (`try_claim_entry` at `storage.c:442`), so it's
consistent with the rest of the allocator. No 16-byte CAS, no
caller-side check race.

**Recursion guard (R2)**: the dequeue's "advance head when drained"
step calls `indir_set(head, 0)` (the "internal_free" — sets
indirection to zero without enqueuing). This is NOT a recursive
`storage_free`; it's a one-line atomic store. No cycle.

**storage_free internal allocation (R6)**: when `storage_free` needs
to allocate a new free-list page (because the tail is full or the
queue is empty), it calls `storage_allocate_tail_advance(sb, 1)` —
a new internal helper that does ONLY the tail-advance, bypassing
the free-list check. This avoids the producer-consumer cycle
between `storage_free` (producer) and `storage_allocate(1)`
(consumer) within the same data structure.

```c
// New internal helper — tail-advance only, no free-list check.
static int64_t storage_allocate_tail_advance(StorageBackend* sb, int count) {
    // ... original tail-advance code from storage_allocate(1) ...
}

// Public storage_allocate now has the free-list check at the top.
int64_t storage_allocate(StorageBackend* sb, int count) {
    if (count == 1) {
        int64_t count_global = vfs_atomic_load_i64(HDR_OFF_FREE_LIST_COUNT);
        if (count_global > 0) {
            int64_t page = dequeue_from_free_list(sb);
            if (page != 0) return page;
        }
    }
    return storage_allocate_tail_advance(sb, count);
}
```

**Head==tail case (R7)**: when head == tail (only one free-list
page), enqueue and dequeue target the same page. The CAS on the
page's `count` field serializes them correctly:
- Enqueue reads count, CAS-increments to N+1.
- Dequeue reads count, CAS-decrements to N-1.
- If both attempt the CAS simultaneously, one wins, the other
  retries. The winner's value is the new state; the loser retries
  with the new state.

When enqueue needs to allocate a new free-list page (tail is full
at MAX_PER_PAGE = 510), it calls `storage_allocate_tail_advance`
to get a new VP, writes the new tail's metadata, and CAS-sets
`free_list_tail` to the new VP. The head is NOT advanced at this
point — the old head (which is the old tail) continues to be
drained by dequeues. The new tail receives new enqueues. This is
correct because the old page's count is at MAX_PER_PAGE, meaning
it's full; no more dequeues can be served from it until the head
advances past it.

When the old head's count reaches 0, the head advances to the
old head's `next_page` (which is the new tail). Now the head and
tail are again on the same page (or different pages, depending on
the chain length). The transition is straightforward.

**No physical offset is leaked on the dequeue path**. The dequeued
page's physical offset is preserved by the enqueue's record
(Step 3 in `dequeue_from_free_list`: `phys = read_entry_physical`),
and re-applied via `indir_set(page, phys)`. The page is reused
in place.

**Trade-off**: ~2x the metadata per free page (16 bytes vs 8 bytes),
and 2x the disk I/O for the enqueue (write both logical and
physical). This is acceptable — enqueue is the rare path.

**Pick: revised design with physical offsets in the queue.** The
metadata cost is negligible (510 entries per page at 16 bytes
each = 8 KB per 1000 frees). The correctness win is the no-leak
guarantee.

### Operation: mount / unmount

**mount_existing** (`src/storage.c:bootstrap_new`-equivalent):
- Reads the header page, validates XVFS magic.
- Reads the new fields `free_list_head`, `free_list_tail`,
  `free_list_count`. The count must equal the sum of `count` fields
  across all free-list pages (validated via a walk; on mismatch,
  log a warning and rebuild the count from the walk).
- No in-memory data structure needed beyond the three fields —
  the free-list pages are on disk, and the hot path reads them
  via `indir_lookup` and `storage_read` as needed.

**bootstrap_new**:
- Sets the new fields to 0 (no free pages).

**storage_close** / `vfs_unmount`:
- Flushes the header page and any dirty free-list pages.
- The free-list pages stay on disk (they're regular storage pages).
- No special cleanup needed.

### In-memory caching

The header fields are in `header_buf` (already in memory). The
free-list pages are NOT pre-cached — they're read on demand via
`storage_read_with_status`, which goes through the page cache. After
the first dequeue, the free-list head page is hot in the cache, so
subsequent dequeues are cache hits.

The page cache is keyed by logical page; the free-list pages
(regular storage pages) are cacheable like any other. Their access
pattern is "read many times, write rarely" — fits the cache well.

### Cache invalidation on dequeue

When a page is dequeued, the cache might have a stale entry for
that page (from a previous read). The cache entry's data is the
OLD content of the page (before the free). After the dequeue, the
page has a NEW physical offset (potentially the same as the old
one if the revised design is used, but with cleared data).

With the revised design (physical offset preserved), the cache
entry for the dequeued page is still valid (same physical offset,
same data). But the dequeued page is supposed to be "fresh" for
the caller — the caller is going to overwrite it via `storage_write`.
The cache entry for the old data should be invalidated to avoid
serving stale data if someone reads the page before the caller's
write.

**Action**: `cache_invalidate(&sb->cache, page)` after dequeue.
This is the same invalidation `storage_free` does today... wait,
it doesn't. `storage_free` today doesn't invalidate the cache.
This is a pre-existing minor bug (stale cache entries survive a
free). The spec fixes it as a side effect.

## Design

### Concurrency

| Operation | Threads involved | Atomicity mechanism |
|---|---|---|
| `enqueue` (storage_free) | GC + future callers | Per-page CAS on `count`; global CAS on `free_list_count`; CAS on `free_list_tail` for new-page link |
| `dequeue` (storage_allocate(1) hot path addition) | All allocator threads | Per-page CAS on `count`; global CAS-add on `free_list_count`; CAS on `free_list_head` when advancing past an empty head |
| `mount_existing` | Single-threaded at mount | No concurrency |

**Lock-free guarantees**:
- No mutex on the hot path (storage_allocate(1) doesn't take any lock
  it doesn't already take).
- Per-page CAS is uncontended in the common case (each free-list
  page is touched by at most one thread at a time for append/pop).
- Global counters (`free_list_count`, `free_list_head`,
  `free_list_tail`) are CAS-modified.

**ABA on `free_list_head`**: the spec uses a single 8-byte head
pointer (no tagged/generation). The chain is forward-only (new
free-list pages are appended at the tail; the head only advances
toward newer pages). The head pointer is monotonic, so classic
ABA (H1 → H2 → H1) requires H1 to be completely drained, removed
from the chain, recycled as a data page or a new free-list page,
and then become the head again — a very narrow window. The cost
of a successful ABA is "duplicate VP allocation" (two callers get
the same logical page from the queue). The dequeue detects this
via `try_claim_entry` (see the dequeue function above) — if the
CAS fails, the dequeue returns 0 and the caller retries
storage_allocate(1), which tail-advances past the already-claimed
VP. No 16-byte CAS, no caller-side check race.

**`free_list_count`** is monotonic-ish (only enqueue/dequeue modify
it; both are +1/-1). ABA is not a concern for a simple counter.

### Hot-path performance

**Current**: `storage_allocate(1)` is ~10 ns per call (3 atomic
loads + 1 CAS on `total_pages`).

**New**: 1 additional atomic load on `free_list_count` at the top.
If `count == 0` (the common case for a fresh VFS or one that
hasn't seen many frees), fall through to the existing path.
Net cost: ~1-2 ns (the load is in L1 cache for the common case
where the queue has been empty for a while).

If `count > 0` (the dequeue path is exercised):
- 1 load for `free_list_count` (the initial check)
- 1 load for `free_list_head` (8 bytes; might need tagged load)
- 1 load for head page's `count` (via indir_lookup + storage_read)
- 1 CAS on head page's `count` (to pop the top entry)
- 1 atomic add on `free_list_count` (to decrement)
- 1 cache_invalidate (lock-free, no atomic)

If the head page is in the page cache, the `storage_read` is a
cache hit (no disk I/O). After the first dequeue, the head page
stays in the cache (it's accessed repeatedly until drained).

**Verdict vs current bench** (post-Phase-27 / pre-this-phase):
- Cold (queue empty): ~12 ns (10 + 2). In the noise (5% of
  200-300 ns total per `vfs_create`).
- Warm (queue non-empty, head page cached): ~30-40 ns. In the noise
  for `vfs_create` (a few % of the total per-op cost). Faster than
  tail-advance for the cache-hit case.

**GC impact**: GC calls `storage_free` for every reclaimed page.
With the new design, those pages go into the free list and are
immediately reusable. GC itself is currently non-functional (returns
VFS_ERR_FULL), so the impact is moot. When GC works, the free list
is the natural place for the deferred-free queue to land.

**Contention under load (R8)**: the atomic load on `free_list_count`
is in the hot path of every `storage_allocate(1)` call. The cache
line holding this counter is shared with `storage_free` writers.
Under low write frequency (current state — only GC calls free,
and GC is non-functional), the line stays in L1 across reads
and the cost is one uncontended atomic load (~1-2 ns). Under high
write frequency (when GC works and is reclaiming many pages per
second), the line bounces between cores on every read, adding
~5-20 ns per load under contention. For the current bench
workloads, GC is not active, so this is in noise. If profiling
shows contention after GC is fixed, the fix is to pad
`free_list_count` to a separate cache line (64 bytes on x86,
128 on Apple Silicon) — the header page has unused space. The
spec documents this as a known post-GC concern, not addressed now.

### Memory and disk overhead

**Per free page**: 16 bytes (8-byte logical + 8-byte physical).
**Per free-list page**: 8 KB (the page itself). With 510 free pages
per 8 KB free-list page, the overhead is `8192 / 510 = 16 bytes per
free page` (metadata) + `16 bytes per free page` (the entry) =
**32 bytes per free page of overhead**.

For 1000 free pages: 32 KB. Negligible.

**Header page growth**: +24 bytes (3 fields × 8 bytes). Negligible.

### Crash safety

The free-list pages are regular storage pages. The header fields
`free_list_head` / `_tail` / `_count` are part of the header page.
On crash:
- Header page is written last (atomic commit point). If a crash
  happens before the header is written, the previous `free_list_*`
  values are intact.
- Free-list page writes are `pwrite` + `cache_mark_dirty`. On crash,
  the page cache is lost (in-memory state), but the on-disk data
  is what was last flushed. The hot path reads via `storage_read`
  which uses the cache, so a cache miss triggers a disk read.

**Edge case**: a crash after a `storage_free` writes the free-list
page but before the header is updated. On remount, the header
shows the old state, but the free-list page exists. The free-list
page's count is whatever was last written. This is fine — the count
is consistent with the page's data.

**Worse edge case**: a crash mid-update of the free-list page (e.g.,
the count was CAS'd to N+1, the entry written, but the dirty mark
not yet set). On remount, the count says N+1, but entry N might be
garbage. The hot path would dequeue a garbage logical page. This is
a real (if rare) bug.

**Mitigation**: order the writes carefully. The spec's enqueue
sequence is:
1. Write the entry at position [count] (the new entry).
2. CAS the count from `count` to `count+1` (this is the commit
   point — if we crash after this, the entry is "visible").
3. Mark the page dirty (so the change is flushed on next
   storage_flush).

The CAS in step 2 is the commit. If we crash before step 2, the
count is still `count` and the entry at [count] is garbage but
won't be read. If we crash after step 2 but before step 3, the
count is `count+1` and the entry is correct in cache but the page
isn't dirty. On remount, the page cache is empty, so the next
`storage_read` reads from disk — which has the OLD data (no flush
happened). The count says N+1 but the disk has only N entries.
On the next dequeue, we'd read the Nth entry, which is garbage.

**Mitigation for this**: use `cache_mark_dirty` (which is in-memory)
followed by an explicit `storage_flush_cache_only(sb)` (or just
rely on the periodic flush). The spec uses the latter — the
free-list page is marked dirty, and the next periodic flush (or
explicit `storage_flush(-1)` at unmount) writes it. If a crash
happens between CAS and the next flush, the count is +1 ahead of
the disk. On remount, the rebuild-count walk validates each
entry's **stored physical offset** (the 8-byte value recorded
when the page was freed): it must be non-zero and point to a
location within the file's size. The `indir[entry]` lookup is
NOT used for validation — a freed page has `indir = 0` by
construction, so checking `indir != 0` would reject every
valid entry (see review R4).

If a physical offset fails validation, the entry is discarded
and the count decremented. The discard is conservative: the entry
might have been valid, but the validation can't tell. This
means a crash in the CAS-to-flush window can "lose" the most
recently-freed pages. The loss is bounded by the dirty-flush
interval (typically `storage_flush(-1)` at unmount, so the window
is the time between the last `storage_free` and the unmount).

The spec documents this as a known race with very low probability
(must crash in the microsecond window between CAS and flush). The
mitigation is the rebuild-count walk on remount that validates
each entry's stored physical offset.

## Performance analysis

**Baseline (post-Phase-27, pre-this-phase)**:
- create 1t: 13,150 ops/sec (76 µs/op)
- create 4t: 13,048 ops/sec
- small_create 1t: 12,400 ops/sec
- write 1t: 12,481 ops/sec

**Expected delta from this phase**:
- Cold (queue empty): +1-2 ns per `storage_allocate(1)`. Total per-op
  impact: +0.01 µs. **In noise** (< 0.1% of total per-op cost).
- Warm (queue non-empty): the hot path becomes cache-resident,
  faster than tail-advance by ~5-10 ns per dequeue. **Out of noise
  but small** (5-10% of the 10 ns tail-advance). Only relevant if
  the free list is heavily exercised, which is rare in the current
  workload (no GC = no frees).

**Bench workload** (created as part of the migration, in W4):
- A new bench mode `bench_freelist` that pre-fills the free list
  with 1000 entries, then alternates `storage_free` and
  `storage_allocate(1)` in a tight loop. Measures the warm-path
  cost.

**Overall verdict**: this phase is "in noise" for the current
benchmarks. The wins are for GC (which is non-functional) and for
the H2 fix in `mirror_write` (which only fires on I/O failure).
Both are correctness wins, not perf wins.

## Migration plan

4 workloads, in dependency order. Each is one self-contained,
testable, benched change.

### Workload 1: data structure + on-disk layout + mount

**Scope**:
- Add the 3 new fields to the header page (`free_list_head`,
  `free_list_tail`, `free_list_count`) at fixed offsets 48, 56, 64.
- Add a `FreeListPage` struct (or just in-line offsets) for the
  free-list page format.
- Update `bootstrap_new` (set new fields to 0) and `mount_existing`
  (read new fields, validate count via a walk).
- Add the mount-time validation walk (rebuild count from on-disk
  free-list pages if the header count doesn't match the sum).
- NO callsite changes in this workload — `storage_free` and
  `storage_allocate` are unchanged. The new fields are zero-initialized
  and ignored.

**Test gate**:
- All 13 ctest suites pass (no behavioral change yet).
- A new test `test_free_list_mount_validation` that:
  - Creates a VFS, allocates 100 pages, unmounts.
  - Manually corrupts the header's `free_list_count` to a wrong
    value, remounts, verifies the count is rebuilt correctly.
- Bench delta: 0 ns (no callsite changes).

**Files touched**:
- `src/storage.h` — add 3 new fields to `StorageBackend`? No,
  they're in the header page, not in the struct. The struct gets
  no new fields.
- `src/storage.c` — `bootstrap_new` and `mount_existing`.
- `test/test_storage.c` — new test.

**Commit**: `phase-27-w1: free-page queue on-disk layout (storage.h/c) + mount validation`

### Workload 2: enqueue (storage_free) — adds to the queue, no consumer yet

**Scope**:
- Update `storage_free` to:
  - Get the current physical offset of the page (via
    `indir_lookup`).
  - Set `indir[page] = 0`.
  - Append `(page, phys_offset)` to the free list.
  - On rare paths (queue empty, page full), allocate a new
    free-list page via `storage_allocate(1)` (using the
    existing tail-advance).
- Storage_free is still called only by GC, but the free list
  is now populated.

**Test gate**:
- All 13 ctest suites pass.
- A new test `test_free_list_enqueue` that:
  - Creates a VFS, allocates 100 pages.
  - Triggers GC's `deferred_free_confirm_and_release` (or
    directly calls `storage_free` for a page).
  - Verifies the free list has the expected count and the
    expected logical page at the head.
- Bench delta: storage_free goes from 1 atomic store to ~10-20
  ns (multiple atomic ops + possibly a `storage_allocate(1)` for
  the rare new-page case). **In noise** (storage_free is called
  only by GC, which is not exercised in the bench).

**Files touched**:
- `src/storage.c` — `storage_free`.
- `src/indirection.c` — possibly a new helper to find the next
  free-list page's last entry (for appending without scanning).
- `test/test_storage.c` — new test.

**Commit**: `phase-27-w2: free-list enqueue (storage_free updated)`

### Workload 3: dequeue (storage_allocate(1) addition) — consume from the queue

**Scope**:
- Add the queue-check at the top of `storage_allocate(1)`:
  - Atomic load of `free_list_count`.
  - If 0, fall through to tail-advance (existing path).
  - If > 0, call `dequeue_from_free_list` and return the page.
- `dequeue_from_free_list` does the per-page CAS pop, advances
  the head if the page drains, and invalidates the cache.

**Test gate**:
- All 13 ctest suites pass.
- A new test `test_free_list_dequeue` that:
  - Creates a VFS, allocates 100 pages.
  - Frees 10 pages via `storage_free`.
  - Verifies `storage_allocate(1)` returns the 10 freed pages
    in FIFO order (oldest first).
  - Verifies `total_pages` is unchanged after the 10 dequeues
    (no tail-advance needed).
- A new test `test_free_list_persistence` that:
  - Creates a VFS, allocates 100 pages, frees 5.
  - Unmounts and remounts.
  - Verifies the 5 freed pages are reused on the next allocate.
- Bench delta: +1-2 ns per `storage_allocate(1)` (the atomic
  load + check). **In noise** for the current workloads (the
  queue is empty in the bench, so the fall-through is the
  common path).

**Files touched**:
- `src/storage.c` — `storage_allocate`, new `dequeue_from_free_list`,
  new `storage_allocate_tail_advance` (extracted from current
  `storage_allocate`'s tail-advance code).
- `src/page_cache.c` — new `cache_invalidate(sb, page)` function.
  The spec's W3 dequeue invalidates the cache entry for the
  dequeued page (so the old data in cache doesn't shadow the
  caller's new write). The current `cache_*` API has
  `cache_mark_dirty`, `cache_evict_all`, `cache_flush_page` —
  no per-page invalidation. Add a new helper that removes a
  specific entry from the cache hash table (lock-free per-bucket,
  reuses the existing `bucket_locks` spinlock).
- `test/test_storage.c` — new tests.

**Commit**: `phase-27-w3: free-list dequeue (storage_allocate(1) consults queue)`

### Workload 4: H2 + H3 closure (mirror_write + storage_allocate)

**Scope**:
- Update `mirror_write` second-write branch to call
  `storage_free(sibling)` on **all four failure paths**, not just
  the one mentioned in the original spec (see review R5):

  | # | Failure point | Code line | `sibling` state | Action |
  |---|---|---|---|---|
  | 1 | `storage_allocate(sb, 1)` returns < 0 | (allocate failed) | not allocated | nothing to free |
  | 2 | `indir_lookup(sb, sibling)` returns 0 | (lookup failed) | allocated, indir set, no data | `storage_free(sibling)` |
  | 3 | `write_page_record(sb, sib_off, ...)` fails | (sibling write failed) | allocated, indir set, partial data on disk | `storage_free(sibling)` |
  | 4 | Final `pwrite` of `original.mirror_page` fails | (link failed) | allocated, indir set, full data on disk, NOT linked | `storage_free(sibling)` |

  The original spec only addressed path 3. Paths 2 and 4 are
  equally important: path 2 leaks an allocated-but-unused VP,
  and path 4 leaks a fully-written-but-unlinked sibling. Both
  are now fixed by calling `storage_free` in the failure branch.

- Update ISSUES.md: H2 marked RESOLVED, H3 marked RESOLVED.
- The deferred-free queue (used by GC) now goes through the new
  free list (consulted by the hot path). The old
  `deferred_free_is_queued` check in `storage_allocate_count_scan`
  can be removed (the free list IS the deferred-free mechanism
  for the hot path).
- Add a regression test that simulates the H2 path: a mirror
  write where the link pwrite fails (mock by writing to a
  full disk or by patching the function pointer).

**Test gate**:
- All 13 ctest suites pass.
- New tests in test_storage.c for the H2 regression.
- Bench: full vfs_bench runs, no regression (this workload
  is correctness-only, no hot-path changes).

**Files touched**:
- `src/lazy_mirror.c` — `mirror_write` failure paths (2, 3, 4).
- `src/storage.c` — possibly simplify `storage_allocate_count_scan`
  (no more dfq check).
- `ISSUES.md` — close H2 and H3.
- `test/test_storage.c` — new tests.

**Commit**: `phase-27-w4: H2 + H3 closure (mirror_write frees sibling on all 4 failure paths, ISSUES.md updated)`

## Open questions for reviewer

All five open questions from the previous version have been
resolved (per the review):

1. **FIFO vs LIFO within a free-list page** — resolved: LIFO
   within a page (pop the last entry; the most recently appended
   entry is hot in the cache). FIFO across pages (head advances
   toward the tail). The spec calls this "page-level FIFO with
   page-local LIFO."

2. **Tag bits for ABA on the head pointer** — resolved: single
   8-byte head pointer (no tagged/generation). The chain is
   forward-only, so classic ABA is rare. ABA detection inside
   `dequeue_from_free_list` via `try_claim_entry` (CAS indir
   from 0 to phys on the dequeued page). If the CAS fails, the
   dequeue returns 0 and the caller retries. Avoids 16-byte CAS
   portability issues and the caller-side check race (Q2).

3. **Crash-safety mitigation** — resolved: rebuild-count walk
   on mount, validating the **stored physical offset** (not
   `indir != 0`, which would reject every valid entry). Spec
   text updated (R4).

4. **Cache invalidation on dequeue** — resolved: yes, invalidate.
   A new `cache_invalidate` function is added in W3 (R11).

5. **`free_list_count` as a separate field vs derived** — resolved:
   separate atomic field. The hot path's atomic load on
   `free_list_count` is the cheapest possible "is the queue
   empty?" check; deriving it would require multiple loads +
   walks on every dequeue.

## Risk assessment

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| ABA on `free_list_head` causes duplicate VP allocation | Very low (forward-only chain) | Caller-visible: two `vfs_create` get the same VP | In-dequeue `try_claim_entry` CAS detects the ABA; caller retries and tail-advances past the already-claimed VP |
| Crash leaves `free_list_count` ahead of disk | Very low (microsecond window between CAS and flush) | Mount-time recovery: re-walk and rebuild | Rebuild-count walk in W1; validates stored physical offset, not `indir` |
| Hot path slower than baseline | Negligible (+1-2 ns when queue is empty) | Bench "in noise" | Workload-gated commit; rollback if regressed > 5% |
| Header layout collisions (overlap with inline indirection entries) | None (R1 fixed: 3 entries reserved for free-list header, inline_count reduced by 3) | N/A | Verified: offsets 40-56 are now the free-list header; entries shift to offset 64+ |
| Concurrent GC + mirror_write + dequeue races | Low (GC is single-threaded today) | Page allocated twice | Documented as "GC only" assumption; revisit when GC becomes concurrent |
| `storage_free` cost on GC slow-path | Low (GC is non-functional) | GC slower when it lands | Worst-case ~30 ns per free (1 atomic load + 1 CAS + 1 atomic add), GC is the slow path anyway |
| Recursive free in dequeue (drained head page) | Resolved (R2 fixed) | N/A | One-line `indir_set(head, 0)` instead of recursive `storage_free` |
| Producer-consumer cycle in enqueue (storage_free → storage_allocate) | Resolved (R6 fixed) | N/A | New `storage_allocate_tail_advance` internal helper, bypasses the free-list check |
| `free_list_count` cache-line contention under load | Low (currently only GC writes; when GC works, ~1 cache-line bounce per free) | Hot-path "in noise" if GC is slow | Document; pad to a separate cache line if profiling shows contention |

The spec's "max concurrency" goal is met by the lock-free design.
The "performance" goal is met for the cold case (1-2 ns delta) and
the warm case (faster than tail-advance). The "persisted across
mounts" goal is met by storing the queue on disk as regular
storage pages.

## Commit plan

4 commits, one per workload.

```
phase-27-w1: free-page queue on-disk layout + mount validation
phase-27-w2: free-list enqueue (storage_free updated)
phase-27-w3: free-list dequeue (storage_allocate(1) consults queue)
phase-27-w4: H2 + H3 closure (mirror_write frees sibling, ISSUES.md updated)
```

## Source

- `ISSUES.md` H2 — `mirror_write` second-write torn sibling.
- `ISSUES.md` H3 — `storage_allocate` count==1 fast path ignores
  deferred-free queue.
- `src/storage.c::storage_allocate` — current `storage_allocate(1)`
  tail-advance fast path. The TODO comment in that function
  names this work.
- `src/storage.c::storage_free` — current `storage_free`
  (one-liner).
- `src/gc.h::DeferredFreeQueue`, `src/gc.c::deferred_free_*` —
  the existing in-memory deferred-free queue; this phase
  subsumes it.
- `src/indirection.c::indir_set` / `indir_lookup` — the atomic
  primitives the new code uses.
- `src/page_cache.c` — for the new `cache_invalidate` (R11).
- `impl/phase-25-pool-by-value-migration.md` — the spec template
  this one follows.
- `PHASE27_FREEPAGE_REVIEW.md` — the review this revision
  addresses.
- `/tmp/vfs_baseline/BASELINE.md` — pre-Phase-27 bench numbers
  for the performance comparison.
