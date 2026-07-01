# Phase 3: Pool Allocator

## Goal
A fixed-size 32-byte slot allocator backed by 8KB pool pages. Every metadata
entry in the VFS — directory entries, file nodes, version chains, names,
mapper entries — comes from this single pool. The allocator must be lock-free
on the hot path and must never fragment.

## Non-Negotiable Constraints

- **No individual slot freeing.** Slots are never freed during normal operation.
  Only GC rebuilds pool pages from scratch. This simplifies concurrency.
- **CAS-based, no mutexes.** Slot allocation uses `vfs_cas_i32` on `poolState`.
  Page creation uses `vfs_cas_i64` on `poolListHead`. No pthread mutex anywhere.
- **VirtualPtr must be 8 bytes.** Must fit in pool entries alongside other fields.
- **Page 0 is never a pool page.** It is the StorageBackend header. This makes
  `VirtualPtr = 0` safely mean "null."
- **The pool page header must be written BEFORE linking into the list.** This
  closes the single-copy risk window before the page holds multi-file metadata.
- **Pool page size is derived from StorageBackend.** The number of slots per
  page is `(sb->page_size - 16) / 32` = 255 at the default page_size of 8192.
  Larger page_sizes increase slot count. The VirtualPtr encoding uses 10 bits
  for the slot index (0–1023), supporting page sizes up to 32,752 bytes.

## Dependencies

| Dependency | Phase | What's Used |
|------------|-------|-------------|
| `storage_allocate` | Phase 2 | Allocate new pool pages from the StorageBackend |
| `storage_write` | Phase 2 | Write pool page headers with `FLUSH_PRIO_POOL` (1) |
| `storage_read` | Phase 2 | Read pool page payloads into memory for slot access |
| `vfs_atomic_*` | Phase 1 | CAS on `poolState`, `poolListHead` |
| `vfs_rd2/wr2/wr4/wr8` | Phase 1 | Read/write pool page header fields and slot data |
| `vfs_crc32c` | Phase 1 | Compute CRC32C on pool page payloads |

## Staging Guidance

Phase 3 can be developed in isolation from Phase 5 (Tree Operations). The pool
allocator does NOT need a superblock. It needs only:

1. A pointer to `poolListHead` (int64_t). This starts as 0 (no pages). The
   pool allocator owns this value — it reads and CAS-updates it. Phase 5 will
   later wire this pointer to the superblock's `poolListHead` field.

2. Access to the StorageBackend (`storage_allocate`, `storage_read`,
   `storage_write`). A test harness can provide these directly.

Build order:
- 3.1 + 3.2 first (pool page layout + free list init)
- 3.4 next (VirtualPtr macros — needed by 3.3 and 3.5)
- 3.5 next (global list — `pool_list_add` and `pool_list_find_free`)
- 3.3 last (slot allocation — uses everything above)

## File Organization

| File | Purpose |
|------|---------|
| `src/pool.c` | Pool page init, slot allocation, VirtualPtr helpers |
| `src/pool.h` | Pool page layout constants, VirtualPtr macros |
| `test/test_pool.c` | Allocation, CAS retry, VirtualPtr round-trip, list walk |

---

## Workload 3.1 — Pool Page Layout

### What
Define the exact byte layout of a pool page payload (8,192 bytes). Every
pool page in the system follows this layout.

### Layout
```
Offset  Size  Type     Name           Description
──────  ────  ───────  ──────         ───────────
  0      8    int64_t  nextPoolPage   Logical page of next pool page in allocator list. Write-once.
  8      4    uint32_t poolState      Packed: bits 16-31 = freeCount, bits 0-15 = firstFreeSlot
 12      4    —        reserved       Zero-filled
 16   8160    —        slots[255]     255 entries, each exactly 32 bytes
8176     16    —        padding        Zero-filled, reserved
```

- Slots are indexed 0 through 254. Slot N occupies bytes `[16 + N*32, 16 + (N+1)*32)`.
- The trailing 16 bytes are unused padding. No link pointer there — the single
  `nextPoolPage` at offset 0 is the only chain pointer this page needs.
- The PageHeader (outside the payload) has `flags` bits 0-1 set to 1 (pool priority).

### Macros in pool.h
```c
#define VFS_POOL_SLOTS         255
#define VFS_POOL_SLOT_SIZE      32
#define VFS_POOL_HEADER_SIZE    16   // nextPoolPage + poolState + reserved
#define VFS_POOL_ENTRIES_OFFSET 16   // slots start here
#define VFS_POOL_FREE_TERMINAL  0xFFFF
```

### Acceptance
- [ ] Fresh page: `freeCount` = 255, `firstFreeSlot` = 0
- [ ] Total payload: 8 + 4 + 4 + 255×32 + 16 = 8192
- [ ] Slot 254 is at highest valid offset; slot 255 does not exist

---

## Workload 3.2 — Free List Initialization

### What
Initialize a freshly allocated pool page so all 255 slots form a linked free
list. Must be done BEFORE the page is linked into the global list.

### Step-by-Step

1. Zero the entire page payload first (`vfs_zero_page`).
2. For slot `i` from 0 to 253: write `(uint16_t)(i + 1)` into bytes 0–1 of the slot.
3. For slot 254: write `0xFFFF` into bytes 0–1 of the slot (terminal sentinel).
4. Set `poolState`: `vfs_wr4(payload, 8, (255 << 16) | 0)`.
5. Write the PageHeader with `generation = 1` and `mirrorPage = -1` via
   `storage_write(sb, page_index, payload, FLUSH_PRIO_POOL)`. This transitions
   the page out of the single-copy risk window BEFORE any operational slots
   are allocated.

### How the Free List Works

When a slot is free, its bytes 0–1 hold the index of the next free slot.
The remaining 30 bytes are garbage and will be overwritten when the slot is
allocated. `poolState` contains:
- `freeCount` in bits 16–31: how many slots are free (255 → 0)
- `firstFreeSlot` in bits 0–15: index of the first free slot to allocate

To allocate: read `firstFreeSlot`. Take that slot. Read its bytes 0–1 to get
the NEXT free slot index. CAS `poolState` to `((freeCount-1) << 16) | nextFreeSlot`.

### Acceptance
- [ ] After init: first allocation returns slot 0
- [ ] After allocating slot 0: next allocation returns slot 1
- [ ] After 255 allocations: `freeCount` is 0
- [ ] Pool page reopened after crash: free list is consistent (allocated slots
  remain allocated, free slots remain free)

---

## Workload 3.3 — Slot Allocation

### What
The actual allocation algorithm — the hot path for every VFS write.

### `pool_alloc(pool_state) → VirtualPtr`

```
1. page_index = pool_list_find_free(pool)
2. If page_index == 0:
   a. Allocate a new pool page via storage_allocate(sb, 1)
   b. Initialize its free list (Workload 3.2)
   c. Write its PageHeader (gen=1, mirror=-1) via
      storage_write(sb, new_page_index, payload, FLUSH_PRIO_POOL)
   d. Call pool_list_add(pool, new_page_index) to prepend to global list
   e. page_index = new_page_index
3. Read pool page payload via storage_read(sb, page_index)
   (This returns a pointer to the 8192-byte payload buffer.)
4. Read poolState = atomic_load_i32(payload + 8)
5. Extract freeCount = poolState >> 16
6. If freeCount == 0:
   // Page was full by the time we got here (race with other threads).
   // Skip this page in the list scan: advance past it and retry.
   // (pool_list_find_free should have returned a page with free slots,
   // but there's a tiny window where another thread drained it.)
   goto 1
7. firstFreeSlot = poolState & 0xFFFF
8. Read nextFreeSlot = vfs_rd2(payload + VFS_POOL_ENTRIES_OFFSET + firstFreeSlot * VFS_POOL_SLOT_SIZE, 0)
9. newPoolState = ((freeCount - 1) << 16) | nextFreeSlot
10. If CAS(payload + 8, poolState, newPoolState) != poolState:
      // Another thread raced — the poolState changed. Retry from step 4.
      goto 4
11. Return VirtualPtr = VFS_VPTR_MAKE(page_index, firstFreeSlot)
```

### `pool_resolve(VirtualPtr vp) → uint8_t* pointer to slot`

```
page_index = VFS_VPTR_PAGE(vp)
slot_index = VFS_VPTR_SLOT(vp)
payload = storage_read(sb, page_index)   // goes through page cache
if payload == NULL: return NULL          // page not yet written (shouldn't happen)
return payload + VFS_POOL_ENTRIES_OFFSET + slot_index * VFS_POOL_SLOT_SIZE
```

`pool_resolve` returns a pointer into the page cache buffer. The slot's 32
bytes can be read or written at this pointer. For writes, the caller must
call `storage_write` (or mark the cache entry dirty) after modifying the slot
to persist the changes. For reads, the pointer is valid as long as the cache
holds the page — the caller should NOT retain the pointer across operations
that may evict the cache.

### Arena Optimization (Optional, Deferred)
If CAS retry rate exceeds 10% at 8+ threads, implement arenas:
- Reserve one byte in the pool page header (currently `reserved` at offset 12)
  as `arena_id`. Default 0 means "any arena."
- Thread T picks `arena_id = T % arena_count`. On allocation, prefer pages
  with matching `arena_id`, falling back to any page with `freeCount > 0`.
- New pages are tagged with the allocating thread's arena ID.

### Acceptance
- [ ] Allocate 1 slot: freeCount decrements, firstFreeSlot advances
- [ ] Allocate 255 slots: all succeed, page is full
- [ ] 256th allocation creates a second pool page
- [ ] 4 threads × 100 allocations each: all VirtualPtrs unique, no double-allocations
- [ ] CAS retry: two threads racing on same poolState → exactly one wins, other retries
- [ ] VirtualPtr for slot 0 is `(page << 10) | 0`, for slot 254 is `(page << 10) | 254`

---

## Workload 3.4 — VirtualPtr

### What
An 8-byte packed reference used everywhere to link pool entries into chains.

### Encoding/Decoding (Macros in pool.h)
```c
#define VFS_VPTR_NULL         ((int64_t)0)
#define VFS_VPTR_MAKE(pg, sl) (((int64_t)(pg) << 10) | ((sl) & 0x3FF))
#define VFS_VPTR_PAGE(vp)     ((int64_t)((vp) >> 10))
#define VFS_VPTR_SLOT(vp)     ((int)((vp) & 0x3FF))
```

### Requirements
- The page index can be up to 2^54 - 1 (fits in 54 bits). Slot index is 0–1023
  (fits in 10 bits). This supports up to 1,024 slots per pool page — enough for
  page sizes up to `1024 * 32 + 16 = 32,784` bytes.
- `VFS_VPTR_NULL` is 0. Logical page 0 is the StorageBackend header — never a
  pool page — so `(0, 0)` is safely null.
- VirtualPtr is stored as `int64_t` in pool entries at 8-byte aligned offsets.
  This enables 64-bit CAS on chain heads.
- `pool_resolve(vp)` is the only way to turn a VirtualPtr into a usable pointer.
  No raw pointer arithmetic on VirtualPtr values.

### Acceptance
- [ ] `VFS_VPTR_MAKE(42, 7)` → PAGE=42, SLOT=7
- [ ] `VFS_VPTR_MAKE(1, 0)` round-trips through pool_resolve correctly
- [ ] Maximum page (2^54 - 1) and max slot (1023) encode/decode without
  overflow
- [ ] Slot index 1024 is rejected (assert in debug, error in release)
- [ ] `VFS_VPTR_NULL` → PAGE=0, SLOT=0

---

## Workload 3.5 — Global Pool List

### What
A singly-linked list of all pool pages, rooted at `poolListHead` in the
superblock. The allocator walks this list to find a page with free slots.

### How It Works

- `poolListHead` is an `int64_t` in the superblock. It holds the logical page
  index of the most recently added pool page.
- Each pool page's `nextPoolPage` points to the next older page. LIFO order.
- Set once when the page enters the list. Never modified. This is what makes
  the list lock-free — `nextPoolPage` writes are uncontended after insertion.
- To allocate: start at `poolListHead`, follow `nextPoolPage`. Read each
  page's `freeCount`. Use the first page with `freeCount > 0`.
- In steady state, the head page has free slots → O(1).
- Pages with `freeCount == 0` remain in the list. They are skipped. GC removes
  them when rebuilding.

### `pool_list_add(new_page_index)`
```
new_page->nextPoolPage = atomic_load_i64(&superblock->poolListHead)
CAS(&superblock->poolListHead, old_value, new_page_index)
If CAS fails: re-read poolListHead, update new_page->nextPoolPage, retry
```

### `pool_list_find_free() → page_index`
```
page = poolListHead
while page != 0:
    freeCount = atomic_load_i32(&page->poolState) >> 16
    if freeCount > 0:
        return page
    page = page->nextPoolPage
return 0  // all pages full — trigger new page allocation
```

### Lazy Rebuild on Mount
If `poolListHead` is stale after a crash, the list is rebuilt lazily:
- When `pool_list_find_free()` returns 0 and the list is non-empty, walk the
  tree from the root, collect all pool page indices from VirtualPtrs, build a
  new list. This is O(metadata pages). GC also does this during compaction.

### Acceptance
- [ ] New pool pages appear at head of list
- [ ] `pool_list_find_free()` returns a page with free slots
- [ ] After consuming all 255 slots of head page: next call returns the next
  page in the list (or allocates a new one)
- [ ] After GC: list contains only pages with free slots
- [ ] Simulated stale poolListHead → lazy rebuild produces correct list

---

## Workload 3.6 — Pool Initialization & Entry Point

### What
A `pool_init` function that wires the pool allocator to the `poolListHead`
pointer and the StorageBackend. This is the single entry point called during
VFS bootstrap (Phase 5) to initialize the pool subsystem.

### `pool_t` struct

```c
typedef struct {
    StorageBackend* sb;          // for storage_allocate/read/write
    int64_t*        list_head;   // pointer to superblock's poolListHead field
} Pool;
```

### `pool_init(Pool* pool, StorageBackend* sb, int64_t* list_head)`

```
1. pool->sb = sb
2. pool->list_head = list_head
3. pool->list_head starts at 0 (no pages). The first pool_alloc call will
   create the first page.
```

### On Mount (Existing File)
When `poolListHead` is read from the superblock on mount, it already points
to the most recently added pool page from the previous session. If the
previous session crashed before flushing, `poolListHead` may be stale. The
`pool_list_find_free` function handles this: if no pages are found with free
slots, the lazy rebuild walks the tree's VirtualPtrs to collect all pool
page indices and reconstructs the list.

### Acceptance
- [ ] `pool_init` on fresh system: `*list_head == 0`, first `pool_alloc` creates page
- [ ] `pool_init` on existing file: `*list_head` points to existing pages
- [ ] `pool_alloc` works without Phase 5 — only needs StorageBackend + list_head pointer

---

## Crash Recovery Detail

After a crash, the on-disk pool page is self-describing:
- `poolState` at offset 8 records `freeCount` and `firstFreeSlot`
- Each free slot's bytes 0–1 point to the next free slot
- Allocated slots contain their operational data (node types, chain pointers)

On remount, reading the pool page's payload gives the exact same free list
state as before the crash. No recovery pass is needed — the pool page IS the
free list. The only exception: slots allocated AND used to write data, but
whose `storage_write` was not flushed. Those slots appear as "free" on
remount because the in-memory free list was advanced but the on-disk version
was not. This is consistent with the VFS's crash model: unflushed writes are
lost. The slots were allocated, data was written to them, but the writes
weren't flushed → on remount the slots are free and the data is lost. GC
will eventually rebuild the pool and reclaim any zombies.

---

## Final Phase 3 Checklist

- [ ] 255 slots allocated from single page without error
- [ ] 256th allocation creates second page
- [ ] 4 threads × 100 slots: 400 unique VirtualPtrs, no double-allocations
- [ ] VirtualPtr encode/decode round-trips for edge values
- [ ] Pool page crash recovery: open after `kill -9`, free list still consistent
- [ ] Global list walk reaches all pages
- [ ] No mutexes in pool allocation path
