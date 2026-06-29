# Phase 3: Pool Allocator

## Goal
A fixed-size 32-byte slot allocator backed by 8KB pool pages. All metadata
in the VFS — directory entries, file nodes, version chains, names, mapper
entries — is stored in this single pool. The allocator must be lock-free on
the hot path and support thread-safe concurrent allocations.

---

## Workload 3.1 — Pool Page Layout

**What:** Define the on-disk layout of a pool page. Each page holds exactly
255 usable 32-byte slots plus a small header for allocation tracking.

**Why:** The pool is the only metadata page type in the VFS. A fixed slot size
simplifies the free list (no fragmentation, constant-time allocation) and
makes GC compaction trivial (just copy surviving slots sequentially).

**How:**
- Pool page payload (8192 bytes): starts with a header, followed by 255
  slots of exactly 32 bytes each, followed by trailing padding.
- Header fields within the payload: `nextPoolPage` (8 bytes, offset 0) is
  the logical page index of the next pool page in the global allocator list.
  Set once when the page enters the list, never modified. `poolState`
  (4 bytes, offset 8) packs two 16-bit values: `freeCount` in bits 16–31
  and `firstFreeSlot` in bits 0–15. `reserved` (4 bytes, offset 12).
- Slots start at offset 16. Slot N occupies bytes `[16 + N*32, 16 + (N+1)*32)`.
  Slot indices range from 0 to 254.
- Trailing 16 bytes at offset 8176 are reserved padding — no special link
  slot, no MetaPoolLink. The old v1 design had a second chain pointer here;
  v2 uses only `nextPoolPage` for chaining.
- The pool page itself is a logical page with a standard 16-byte PageHeader
  before the payload. The PageHeader's `pageType` is `0x02` (PoolPage) and
  `flags` is 0. The flush priority bits in `flags` are set by the VFS layer
  according to the page's role (priority 1 for pool pages).

**Acceptance:**
  - A freshly allocated pool page has `freeCount = 255`, `firstFreeSlot = 0`.
  - The total payload size (header + 255×32 + padding) equals exactly 8192.
  - Slot 254 is at the highest valid offset; reading beyond it is out of bounds.

---

## Workload 3.2 — Free List Initialization

**What:** Initialize the free list of a freshly allocated pool page so that
slots 0 through 254 form a singly-linked chain of available slots.

**Why:** The allocator needs to find free slots in O(1) time without scanning.
A linked list of free slots embedded within the free slots themselves provides
this — the allocator reads `firstFreeSlot` to get the next available slot,
and the slot itself stores the index of the next free slot.

**How:**
- When a slot is free, bytes 0–1 of that slot store the index of the next
  free slot as a `uint16_t`. The remaining 30 bytes are undefined and may
  contain garbage from a previous allocation — they will be overwritten
  when the slot is allocated.
- The terminal sentinel is `0xFFFF` (slot index 65535, which exceeds the
  maximum valid index of 254).
- On a freshly allocated pool page, initialize:
  - `slot[0].bytes[0:2] = 1`
  - `slot[1].bytes[0:2] = 2`
  - `...`
  - `slot[253].bytes[0:2] = 254`
  - `slot[254].bytes[0:2] = 0xFFFF`
  - `poolState = (255 << 16) | 0` — 255 free slots, first free is slot 0.
- This initialization must run before the page is linked into the global
  pool list, so no other thread can observe an uninitialized page.
- The pool page header (generation=1, mirrorPage=-1) must also be written
  before linking, so the page is crash-safe from its first use.

**Acceptance:**
  - After initialization, the first allocation returns slot 0.
  - After allocating slot 0, the next allocation returns slot 1.
  - After allocating all 255 slots, `freeCount` is 0 and the next allocation
    triggers creation of a new pool page.
  - A pool page that survived a crash can be reopened and its free list
    is still consistent (slots allocated before the crash remain allocated;
    slots that were free remain free).

---

## Workload 3.3 — Slot Allocation

**What:** A lock-free algorithm to allocate a single 32-byte slot from a pool
page, returning a VirtualPtr. When a page is exhausted, a new page is
allocated and prepended to the global list.

**Why:** Pool slot allocation is on the critical path for every VFS write —
creating a VersionPage, prepending a DirContent entry, adding a name slot.
Contention on the pool allocator directly limits write throughput.

**How:**
- Read `poolState` from the pool page. Extract `freeCount` (bits 16–31) and
  `firstFreeSlot` (bits 0–15).
- If `freeCount == 0`: this page is full. Allocate a new pool page from the
  StorageBackend via `Allocate(1)`. Initialize its free list (Workload 3.2).
  Write its PageHeader (gen=1, mirror=-1) before publishing. Set its
  `nextPoolPage` to the current value of `poolListHead` in the superblock.
  CAS `poolListHead` from the old value to the new page index. If the CAS
  fails, retry — another thread prepended a page first; re-read `poolListHead`
  and try again with your allocated page (never abandon it). Then restart
  allocation from the newly active page.
- If `freeCount > 0`: take the slot at `firstFreeSlot`. Read the next free
  slot index from that slot's bytes 0–1. CAS `poolState` to decrement
  `freeCount` and set `firstFreeSlot` to the next index. If the CAS fails,
  retry from the beginning (re-read `poolState` — another thread may have
  consumed a different slot, changing both fields).
- On successful CAS: the slot is allocated. Write the caller's data into
  the slot's 32 bytes. Return a `VirtualPtr` encoding the pool page index
  and the slot index.
- The CAS retry loop is uncontended in steady state because each successful
  allocation advances `firstFreeSlot` to a different value — threads rarely
  collide on the same slot.
- For a page with only a few free slots remaining, threads may converge on
  the same `poolState` value. This is expected and handled by the CAS retry.

**Acceptance:**
  - Allocate one slot: `freeCount` decrements by 1, `firstFreeSlot` advances.
  - Allocate 255 slots from one page: all succeed, page is now full.
  - Allocate 256 slots: triggers creation of a second pool page.
  - Four concurrent threads each allocate 100 slots: total allocated is 400,
    no double-allocations. All VirtualPtrs are unique.
  - CAS retry: if two threads race on the same `poolState`, exactly one wins
    and the other retries successfully on the updated state.

---

## Workload 3.4 — VirtualPtr

**What:** An 8-byte packed value that uniquely identifies a specific slot in
a specific pool page. Used throughout the VFS as the universal reference
for metadata entries.

**Why:** Metadata entries are linked together in chains (DirContent → next
DirContent, VersionPage → next VersionPage, etc.). These chain links must
fit within the 32-byte slot. An 8-byte VirtualPtr packs both the pool page
index and the slot index, leaving 24 bytes free for other fields. This is
the sole chain-linking mechanism — there are no raw pointers.

**How:**
- Encoding: `VirtualPtr = (poolPageIndex << 8) | (slotIndex & 0xFF)`. The
  pool page index occupies bits 8–63. The slot index occupies bits 0–7
  (range 0–254, with 255 being unused).
- Decoding: `poolPageIndex = vp >> 8`, `slotIndex = vp & 0xFF`.
- Null value: `VirtualPtr = 0` means null (end of chain or unallocated).
  Logical page 0 is the StorageBackend header and is never a pool page,
  so `(page=0, slot=0)` naturally encodes as 0 — safely colliding with null.
- Resolution: to access the data at a VirtualPtr, first resolve the pool
  page index to a cached page buffer via the unified page cache, then index
  into the slots array at `slots[slotIndex]`. This is typically done inline
  by a helper function.
- Helper macros or inline functions:
  - `VFS_VPTR_NULL` = 0
  - `VFS_VPTR_PAGE(vp)` — extract page index
  - `VFS_VPTR_SLOT(vp)` — extract slot index
  - `VFS_VPTR_MAKE(page, slot)` — encode
- The VirtualPtr is stored as an 8-byte aligned `int64_t` in pool entries.
  This enables native 64-bit CAS on chain heads.

**Acceptance:**
  - `VFS_VPTR_MAKE(42, 7)` → `VFS_VPTR_PAGE` = 42, `VFS_VPTR_SLOT` = 7.
  - `VFS_VPTR_NULL` → `VFS_VPTR_PAGE` = 0, `VFS_VPTR_SLOT` = 0.
  - A round-trip through encode → write to slot → read slot → decode
    preserves the original page and slot values.
  - Maximum page index (2^56 - 1) and maximum slot index (254) encode and
    decode correctly.
  - Attempting to encode a slot index > 254 is a programmer error (assert
    in debug builds).

---

## Workload 3.5 — Global Pool List

**What:** A singly-linked list of all pool pages, rooted at the superblock's
`poolListHead` field. Used by the allocator to find pages with free slots
and rebuilt by GC during shadow compaction.

**Why:** The allocator needs to find any page with `freeCount > 0`. Walking
a global list is simple and lock-free: following `nextPoolPage` pointers
requires no synchronization because the field is write-once. Pages are
never removed from the list except by GC, which operates under the tree lock.

**How:**
- The superblock field `poolListHead` (8 bytes, VirtualPtr-compatible page
  index) points to the most recently added pool page.
- Each pool page's `nextPoolPage` field points to the next older page.
  The list is in LIFO order (newest at head). This is a write-once field:
  set when the page enters the list, never modified afterward.
- To find a page with free slots: start at `poolListHead`. Walk via
  `nextPoolPage`. Read each page's `poolState.freeCount`. Use the first
  page with free slots. In steady state, the head page has free slots
  (it was just added or still has capacity), making this O(1).
- The scan may traverse multiple full pages if the head page has been
  exhausted by other threads. This is bounded by the number of pool pages,
  which is proportional to metadata volume. GC resets the list to compact
  form, removing full pages from the chain.
- On mount, if `poolListHead` is stale (from a crash before a GC completed),
  the list is rebuilt lazily: the first allocation that finds no free slots
  in the chain triggers a tree walk to collect all pool page indices from
  VirtualPtrs in the tree. This is O(metadata pages), not O(total pages).
- GC builds a new pool list from scratch and atomically swaps `poolListHead`
  in the superblock during the commit phase.

**Acceptance:**
  - New pool pages appear at the head of the list.
  - Walking the list reaches all pool pages that were added.
  - A page with `freeCount == 0` is skipped by the allocator scan.
  - After GC, the list contains only pages with free slots (all dead pages
    are removed).
  - After a simulated crash with a stale `poolListHead`, the lazy rebuild
    correctly reconstructs the list from tree VirtualPtrs.

---

## Deliverables

| File | Purpose |
|------|---------|
| `src/pool.c` | Pool page initialization, slot allocation, CAS retry loop |
| `src/pool.h` | `VirtualPtr` macros, pool state helpers |
| `test/test_pool.c` | Allocation, CAS retry, VirtualPtr round-trip, list walking |

## Success Criteria
- 255 slots can be allocated from a single pool page without error.
- The 256th allocation triggers creation of a second page.
- Four concurrent threads allocating 100 slots each produce unique VirtualPtrs.
- `VirtualPtr` encode/decode round-trips correctly for all valid inputs.
- After a simulated crash during allocation, the free list is consistent
  (no double-allocations, no leaked slots). This is verified by allocating
  a few slots, killing the process without flushing, reopening, and verifying
  all previously-allocated slots remain allocated.
