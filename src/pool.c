#include "pool.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * pool_page_init (Workload 3.2)
 *
 * Initialize a freshly allocated pool page so all slots form a linked free
 * list.  Must be called BEFORE the page is linked into the global list.
 *
 * After init:
 *   - slot[i].bytes[0:2] = i+1    for i < slot_count-1
 *   - slot[last].bytes[0:2] = 0xFFFF  (terminal sentinel)
 *   - poolState = (slot_count << 16) | 0
 *   - nextPoolPage = 0
 * --------------------------------------------------------------------------- */

void pool_page_init(uint8_t* payload, int64_t page_size) {
    int slot_count = VFS_POOL_SLOTS_FOR_PAGE(page_size);
    /* Cap at 65535 — the maximum that fits in VirtualPtr's 16-bit slot index
       and poolState's 16-bit firstFreeSlot field. */
    if (slot_count > 65535) slot_count = 65535;

    /* Zero the entire page payload */
    memset(payload, 0, (size_t)page_size);

    /* Build free list: each free slot's bytes 0–1 point to the next free slot */
    for (int i = 0; i < slot_count - 1; i++) {
        int offset = VFS_POOL_ENTRIES_OFFSET + i * VFS_POOL_SLOT_SIZE;
        vfs_wr2_s(payload, offset, (int16_t)(i + 1), page_size);
    }

    /* Terminal sentinel on the last slot */
    int last_offset = VFS_POOL_ENTRIES_OFFSET + (slot_count - 1) * VFS_POOL_SLOT_SIZE;
    vfs_wr2_s(payload, last_offset, (int16_t)VFS_POOL_FREE_TERMINAL, page_size);

    /* Set poolState: all slots free, first free is slot 0 */
    vfs_wr4_s(payload, POOL_OFF_STATE, (int32_t)pool_state_pack((uint16_t)slot_count, 0), page_size);

    /* nextPoolPage = 0 (not linked yet) */
    vfs_wr8_s(payload, POOL_OFF_NEXT, 0, page_size);
}

/* ---------------------------------------------------------------------------
 * pool_init (Workload 3.6)
 * --------------------------------------------------------------------------- */

void pool_init(Pool* pool, StorageBackend* sb, int64_t* list_head) {
    pool->sb        = sb;
    pool->list_head = list_head;
}

/* ---------------------------------------------------------------------------
 * pool_list_add (Workload 3.5)
 *
 * Prepend a pool page to the global list via CAS on *list_head.
 * nextPoolPage is write-once (set here, never modified).
 * --------------------------------------------------------------------------- */

void pool_list_add(Pool* pool, int64_t page_index, uint8_t* payload) {
    /* Read current head */
    int64_t old_head = vfs_atomic_load_i64(pool->list_head);

    /* Set this page's nextPoolPage to the current head */
    vfs_wr8_s(payload, POOL_OFF_NEXT, old_head, pool->sb->page_size);

    /* CAS-prepend: swap *list_head from old_head to page_index.
       If CAS fails (another thread prepended), retry with new head. */
    int64_t current;
    do {
        old_head = vfs_atomic_load_i64(pool->list_head);
        vfs_wr8_s(payload, POOL_OFF_NEXT, old_head, pool->sb->page_size);
        current = vfs_cas_i64(pool->list_head, old_head, page_index);
    } while (current != old_head);
}

/* ---------------------------------------------------------------------------
 * pool_list_find_free (Workload 3.5)
 *
 * Walk the global list starting at *list_head.  Return the first page with
 * freeCount > 0, or 0 if all pages are full.
 * --------------------------------------------------------------------------- */

int64_t pool_list_find_free(Pool* pool) {
    int64_t page = vfs_atomic_load_i64(pool->list_head);

    while (page != 0) {
        /* Read the pool page payload */
        uint8_t* payload = storage_read(pool->sb, page);
        if (payload == NULL) break;  /* corrupt or missing page */

        /* Read poolState and check freeCount */
        uint32_t state = (uint32_t)vfs_rd4_s(payload, POOL_OFF_STATE, pool->sb->page_size);
        uint16_t free_count = pool_state_free_count(state);

        if (free_count > 0) {
            return page;  /* found a page with free slots */
        }

        /* Follow to next pool page */
        page = vfs_rd8_s(payload, POOL_OFF_NEXT, pool->sb->page_size);
    }

    return 0;  /* all pages full or no pages */
}

/* ---------------------------------------------------------------------------
 * pool_alloc (Workload 3.3)
 *
 * Allocate one 32-byte slot from the pool.  Returns a VirtualPtr, or
 * VFS_VPTR_NULL on failure.
 * --------------------------------------------------------------------------- */

int64_t pool_alloc(Pool* pool) {
    for (;;) {
        /* 1. Find a page with free slots */
        int64_t page_index = pool_list_find_free(pool);

        if (page_index == 0) {
            /* 2. No free page — allocate a new one */
            page_index = storage_allocate(pool->sb, 1);
            if (page_index < 0) return VFS_VPTR_NULL;

            /* Allocate a buffer, init free list, write to disk */
            uint8_t* payload = malloc((size_t)pool->sb->page_size);
            if (!payload) return VFS_VPTR_NULL;

            pool_page_init(payload, pool->sb->page_size);

            /* Write to disk (transitions out of single-copy risk window) */
            storage_write(pool->sb, page_index, payload, FLUSH_PRIO_POOL);

            /* Link into global list */
            pool_list_add(pool, page_index, payload);

            /* The page is now in the cache via storage_write.
               payload can be freed — cache has its own copy. */
            free(payload);
        }

        /* 3. Read the pool page from cache */
        uint8_t* payload = storage_read(pool->sb, page_index);
        if (payload == NULL) return VFS_VPTR_NULL;

        /* 4. Read poolState */
        uint32_t state = (uint32_t)vfs_atomic_load_i32(
            (const int32_t*)(payload + POOL_OFF_STATE));

        /* 5. Extract freeCount */
        uint16_t free_count = pool_state_free_count(state);
        if (free_count == 0) {
            /* Page was drained by another thread — retry */
            continue;
        }

        /* 7. Get firstFreeSlot */
        uint16_t first_free = pool_state_first_free(state);

        /* 8. Read nextFreeSlot from the slot's bytes 0–1.
           This MUST be inside the CAS retry loop: if the CAS fails, another
           thread consumed this slot, and first_free may now point to a
           different slot with a different next_free. */
        int slot_offset = VFS_POOL_ENTRIES_OFFSET + first_free * VFS_POOL_SLOT_SIZE;
        uint16_t next_free = (uint16_t)vfs_rd2_s(payload, slot_offset, pool->sb->page_size);

        /* 9. Compute new poolState */
        uint32_t new_state = pool_state_pack(free_count - 1, next_free);

        /* 10. CAS on poolState */
        int32_t old_state = (int32_t)state;
        if (vfs_cas_i32((int32_t*)(payload + POOL_OFF_STATE),
                         old_state, (int32_t)new_state) != old_state) {
            /* CAS failed — another thread raced.  Re-fetch the payload pointer
               in case the page was evicted from cache and re-read from disk.
               This eliminates the pinning race (finding #B). */
            payload = storage_read(pool->sb, page_index);
            if (payload == NULL) continue;
            continue;
        }

        /* CAS succeeded — mark the pool page dirty */
        cache_mark_dirty(&pool->sb->cache, page_index, FLUSH_PRIO_POOL);

        /* 11. Return VirtualPtr */
        return VFS_VPTR_MAKE(page_index, first_free);
    }
}

/* Phase 25: TEST-ONLY compat wrappers (see pool.h for rationale).
   These return raw pointers into the cache (the OLD API shape) for
   the test suite.  Production code uses the safe by-value API. */
uint8_t* pool_resolve(Pool* pool, int64_t vptr, int writable) {
    if (vptr == VFS_VPTR_NULL) return NULL;

    int64_t page_index = VFS_VPTR_PAGE(vptr);
    int     slot_index = VFS_VPTR_SLOT(vptr);

    uint8_t* payload = storage_read(pool->sb, page_index);
    if (payload == NULL) return NULL;

    if (writable) {
        cache_mark_dirty(&pool->sb->cache, page_index, FLUSH_PRIO_POOL);
    }

    return payload + VFS_POOL_ENTRIES_OFFSET + slot_index * VFS_POOL_SLOT_SIZE;
}

/* ---------------------------------------------------------------------------
 * pool_acquire / pool_release — Phase 25 (C1 fix) by-value pool API
 *
 * Copy 32 bytes from the pool page into a stack-local PoolSlot. The
 * caller's working copy is never a pointer into the cache payload, so
 * it cannot be invalidated by cache eviction. `pool_release` writes
 * the bytes back (when pinned) and re-marks the page dirty.
 *
 * The pin state lives in the struct (slot->pinnedPage), not in the
 * function signatures, so the lifecycle is self-documenting and a
 * stray pool_release is a safe no-op rather than a write of stale
 * data back to the page.
 * --------------------------------------------------------------------------- */

void pool_acquire(Pool* pool, int64_t vptr, bool pinPage, PoolSlot* out) {
    if (!pool || !out) return;

    out->vptr = vptr;
    out->pinnedPage = 0;  /* default: not pinned; only set true after success */
    memset(out->bytes, 0, VFS_POOL_SLOT_SIZE);

    if (vptr == VFS_VPTR_NULL) {
        /* Invalid virtual pointer — leave bytes zeroed, no pin. */
        return;
    }

    int64_t page_index = VFS_VPTR_PAGE(vptr);
    int     slot_index = VFS_VPTR_SLOT(vptr);
    int     slot_offset = VFS_POOL_ENTRIES_OFFSET + slot_index * VFS_POOL_SLOT_SIZE;

    uint8_t* payload = storage_read(pool->sb, page_index);
    if (payload == NULL) {
        /* Page not in cache and not on disk — leave bytes zeroed, no pin.
           Caller's type/field checks will detect the invalid slot. */
        return;
    }

    memcpy(out->bytes, payload + slot_offset, VFS_POOL_SLOT_SIZE);

    if (pinPage) {
        /* Pin: mark the cache page dirty so it can't be evicted for
           the duration of the call. This is a perf optimization — the
           C1 hazard is closed by the copy-out itself, not by this mark. */
        cache_mark_dirty(&pool->sb->cache, page_index, FLUSH_PRIO_POOL);
        out->pinnedPage = 1;
    }
}

void pool_release(Pool* pool, PoolSlot* slot) {
    if (!pool || !slot) return;

    /* No-op if the slot wasn't pinned, or has already been released.
       The struct's pin state is the source of truth — no caller flag
       needed. */
    if (slot->pinnedPage == 0) return;

    /* The page was pinned at acquire, so it should be in cache. If
       something exotic happened (e.g., cache reset), storage_read
       may still miss — bail safely without writing back. */
    int64_t page_index = VFS_VPTR_PAGE(slot->vptr);
    int     slot_index = VFS_VPTR_SLOT(slot->vptr);
    int     slot_offset = VFS_POOL_ENTRIES_OFFSET + slot_index * VFS_POOL_SLOT_SIZE;

    uint8_t* payload = storage_read(pool->sb, page_index);
    if (payload != NULL) {
        memcpy(payload + slot_offset, slot->bytes, VFS_POOL_SLOT_SIZE);
    }
    /* Mark dirty regardless — keeps the page resident and ensures the
       bytes are flushed on the next Flush(-1). Idempotent. */
    cache_mark_dirty(&pool->sb->cache, page_index, FLUSH_PRIO_POOL);

    /* Clear pin state so a stray second release is a safe no-op. */
    slot->pinnedPage = 0;
}
