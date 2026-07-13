#ifndef VFS_POOL_H
#define VFS_POOL_H

#include "ixsphere/vfs.h"
#include "platform.h"
#include "page_buf.h"
#include "storage.h"

/* ---------------------------------------------------------------------------
 * Pool page layout constants (Workload 3.1)
 *
 * Every pool page in the system has this layout:
 *
 *   Offset  Size  Description
 *   ──────  ────  ───────────
 *     0       8   nextPoolPage  (int64_t — logical page of next pool page; 0 = end)
 *     8       4   poolState     (uint32 — packed: bits 16-31 = freeCount, bits 0-15 = firstFreeSlot)
 *    12       4   reserved      (zero-filled)
 *    16    8160   slots[255]    (255 × 32 bytes)
 *  8176      16   padding       (zero-filled)
 *
 * Total: 8192 bytes (at default page_size).
 * --------------------------------------------------------------------------- */

#define VFS_POOL_SLOTS           255
#define VFS_POOL_SLOT_SIZE        32
#define VFS_POOL_HEADER_SIZE      16   /* nextPoolPage + poolState + reserved */
#define VFS_POOL_ENTRIES_OFFSET   16   /* slots start at byte 16 */
#define VFS_POOL_FREE_TERMINAL  0xFFFF /* sentinel in free slot's bytes 0–1 */

/* Offsets within the pool page payload */
#define POOL_OFF_NEXT       0   /* int64_t: next pool page in global list */
#define POOL_OFF_STATE      8   /* uint32: packed freeCount|firstFreeSlot */
#define POOL_OFF_RESERVED  12   /* uint32: reserved */

/* Compute slots_per_page dynamically for non-default page sizes:
   (page_size - VFS_POOL_HEADER_SIZE) / VFS_POOL_SLOT_SIZE */
#define VFS_POOL_SLOTS_FOR_PAGE(ps) \
    (int)(((ps) - VFS_POOL_HEADER_SIZE) / VFS_POOL_SLOT_SIZE)

/* Verify layout at compile time for default page size (8192).
   For non-default page sizes, use VFS_POOL_SLOTS_FOR_PAGE(ps) instead. */
#define VFS_POOL_TOTAL_BYTES \
    (VFS_POOL_HEADER_SIZE + VFS_POOL_SLOTS * VFS_POOL_SLOT_SIZE + 16 /* padding */)
_Static_assert(VFS_POOL_TOTAL_BYTES == 8192,
               "default pool page layout must be exactly 8192 bytes");

/* ---------------------------------------------------------------------------
 * poolState packing/unpacking
 *
 *   bits 0–15:  firstFreeSlot (0–65535)
 *   bits 16–31: freeCount     (0–65535)
 * --------------------------------------------------------------------------- */

VFS_INLINE uint32_t pool_state_pack(uint16_t free_count, uint16_t first_free) {
#ifndef NDEBUG
    /* first_free must be a valid slot index (0–65535) or the terminal sentinel */
    assert(first_free <= VFS_POOL_FREE_TERMINAL);
#endif
    return ((uint32_t)free_count << 16) | (uint32_t)first_free;
}

VFS_INLINE uint16_t pool_state_free_count(uint32_t state) {
    return (uint16_t)(state >> 16);
}

VFS_INLINE uint16_t pool_state_first_free(uint32_t state) {
    return (uint16_t)(state & 0xFFFF);
}

/* ---------------------------------------------------------------------------
 * VirtualPtr (Workload 3.4)
 *
 * An 8-byte packed reference: page_index in the upper 48 bits, slot_index
 * in the lower 16 bits.  VirtualPtr 0 is null (page 0 is the StorageBackend
 * header — never a pool page).
 *
 *   VirtualPtr = (page_index << 16) | (slot_index & 0xFFFF)
 * --------------------------------------------------------------------------- */

#define VFS_VPTR_NULL           ((int64_t)0)
#define VFS_VPTR_MAKE(pg, sl)   (((int64_t)(pg) << 16) | ((int64_t)(sl) & 0xFFFF))
#define VFS_VPTR_PAGE(vp)       ((int64_t)((uint64_t)(vp) >> 16))
#define VFS_VPTR_SLOT(vp)       ((int)((uint64_t)(vp) & 0xFFFF))

/* ---------------------------------------------------------------------------
 * Pool handle (Workload 3.6 — forward declaration)
 * --------------------------------------------------------------------------- */

typedef struct {
    StorageBackend* sb;          /* for storage_allocate/read/write */
    int64_t*        list_head;   /* pointer to superblock's poolListHead field */
} Pool;

/* ---------------------------------------------------------------------------
 * Pool page operations (pool.c)
 * --------------------------------------------------------------------------- */

/* Initialize a freshly allocated pool page's free list.
   Must be called BEFORE linking into the global list. */
void pool_page_init(uint8_t* payload, int64_t page_size);

/* Initialize the pool handle. */
void pool_init(Pool* pool, StorageBackend* sb, int64_t* list_head);

/* Allocate one 32-byte slot.  Returns VFS_VPTR_NULL on failure. */
int64_t pool_alloc(Pool* pool);

/* ---------------------------------------------------------------------------
 * PoolSlot — by-value 32-byte slot payload (Phase 25, C1 fix)
 *
 * A PoolSlot is a typed 32-byte buffer that the caller uses as a working
 * copy of a pool entry. The pointer never escapes the pool API: `pool_acquire`
 * copies 32 bytes from the cache page into `slot->bytes`; `pool_release`
 * copies them back (if pinned) and marks the page dirty. Between acquire
 * and release, the caller's working copy is a stack-local — it cannot be
 * invalidated by cache eviction.
 *
 * `pinnedPage` carries the pin state set by `pool_acquire` (0 or 1).
 * `pool_release` checks this field; if 0, it no-ops. This makes the
 * lifecycle self-documenting and `pool_release` safe to call on any slot.
 * --------------------------------------------------------------------------- */

typedef struct {
    int64_t  vptr;          /* which slot this came from (set by acquire) */
    int      pinnedPage;    /* 0 or 1; set by acquire, cleared by release */
    uint8_t  bytes[VFS_POOL_SLOT_SIZE];  /* the slot payload */
} PoolSlot;

/* Copy 32 bytes from the pool page backing `vptr` into `*out`.
   If `pinPage` is true, the underlying cache page is marked dirty
   (pinned against eviction) for the duration of the call — this is
   a perf optimization: it keeps the page resident in cache so the
   later `pool_release` is a guaranteed in-cache write, not a
   possible disk re-read. The pin state is recorded in
   `out->pinnedPage` so `pool_release` can act on it.

   On failure (vptr == 0 or storage_read returns NULL), `*out->bytes`
   is zero-filled and `out->pinnedPage` is set to 0. The caller's
   subsequent type/field checks detect the invalid slot. */
void pool_acquire(Pool* pool, int64_t vptr, bool pinPage, PoolSlot* out);

/* If `slot->pinnedPage` is set: copy 32 bytes from `slot->bytes`
   back to the cache page (guaranteed in cache because acquire pinned
   it), then mark the page dirty. After a successful write-back,
   `slot->pinnedPage` is cleared to 0, so a stray second call is
   a safe no-op. If `slot->pinnedPage` is 0 (slot wasn't pinned, or
   has already been released): no-op. */
void pool_release(Pool* pool, PoolSlot* slot);

/* Add a pool page to the global list (CAS on poolListHead). */
void pool_list_add(Pool* pool, int64_t page_index, uint8_t* payload);

/* Find a pool page with free slots.  Returns logical page index, or 0. */
int64_t pool_list_find_free(Pool* pool);

#endif /* VFS_POOL_H */
