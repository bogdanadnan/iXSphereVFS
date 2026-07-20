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
    pool->sb         = sb;
    pool->list_head  = list_head;
    pool->alloc_count = 0;
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
        /* Read the pool page payload.  Phase 27 C5: distinguish
           "not allocated" (end of list — stop walking) from
           "I/O error" or "CRC error" (data corruption — do NOT
           use garbage bytes, treat as end of list). */
        StorageReadStatus pst;
        uint8_t* payload = storage_read_with_status(pool->sb, page, &pst);
        if (payload == NULL) break;  /* not allocated OR corrupt — stop walking */

        /* Read poolState and check freeCount */
        uint32_t state = (uint32_t)vfs_rd4_s(payload, POOL_OFF_STATE, pool->sb->page_size);
        uint16_t free_count = pool_state_free_count(state);

        if (free_count > 0) {
            return page;  /* found a page with free slots */
        }

        /* Follow to next pool page */
        page = vfs_rd8_s(payload, POOL_OFF_NEXT, pool->sb->page_size);
    }

    return 0;  /* all pages full or no pages (or list head was 0) */
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

        /* 3. Read the pool page from cache.  Phase 27 C5: a NULL
           with status IO/CRC means data corruption — log it and
           fail the allocation rather than silently using zeros. */
        StorageReadStatus pst;
        uint8_t* payload = storage_read_with_status(pool->sb, page_index, &pst);
        if (payload == NULL) {
            if (pst == STORAGE_IO_ERROR || pst == STORAGE_CRC_ERROR) {
                fprintf(stderr, "vfs: pool_alloc: pool page %lld corrupted (status=%d)\n",
                        (long long)page_index, (int)pst);
            }
            return VFS_VPTR_NULL;
        }

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
               This eliminates the pinning race (finding #B).  Phase 27 C5:
               on CRC/IO error, log and retry — the next iteration will
               re-find a free page (or fail the whole pool_alloc). */
            StorageReadStatus rst;
            payload = storage_read_with_status(pool->sb, page_index, &rst);
            if (payload == NULL) {
                if (rst == STORAGE_IO_ERROR || rst == STORAGE_CRC_ERROR) {
                    fprintf(stderr, "vfs: pool_alloc: pool page %lld corrupted in CAS retry (status=%d)\n",
                            (long long)page_index, (int)rst);
                }
                continue;
            }
            continue;
        }

        /* CAS succeeded — mark the pool page dirty */
        cache_mark_dirty(&pool->sb->cache, page_index, FLUSH_PRIO_POOL);

        /* 11. Return VirtualPtr */
        int64_t vptr = VFS_VPTR_MAKE(page_index, first_free);
        pool->alloc_count++;
        return vptr;
    }
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

    /* Phase 27 C5: a NULL with status IO/CRC means the page backing
       this vptr is corrupted.  We can't copy garbage into the slot;
       leaving it zeroed is the safest fallback, but log it so the
       corruption is visible (and the caller's type/field checks
       will likely fail downstream). */
    StorageReadStatus pst;
    uint8_t* payload = storage_read_with_status(pool->sb, page_index, &pst);
    if (payload == NULL) {
        if (pst == STORAGE_IO_ERROR || pst == STORAGE_CRC_ERROR) {
            fprintf(stderr, "vfs: pool_acquire: pool page %lld corrupted (vptr=%lld, status=%d)\n",
                    (long long)page_index, (long long)vptr, (int)pst);
        }
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

    /* Phase 27 C5: a NULL with status IO/CRC means writing back to
       a corrupt page would propagate the corruption to disk.  Skip
       the write and log — the pin stays (the page stays dirty) so
       the caller's bytes are preserved in the pin path. */
    StorageReadStatus pst;
    uint8_t* payload = storage_read_with_status(pool->sb, page_index, &pst);
    if (payload == NULL) {
        if (pst == STORAGE_IO_ERROR || pst == STORAGE_CRC_ERROR) {
            fprintf(stderr, "vfs: pool_release: pool page %lld corrupted, skipping write-back (vptr=%lld, status=%d)\n",
                    (long long)page_index, (long long)slot->vptr, (int)pst);
        }
    } else {
        memcpy(payload + slot_offset, slot->bytes, VFS_POOL_SLOT_SIZE);
    }
    /* Mark dirty regardless — keeps the page resident and ensures the
       bytes are flushed on the next Flush(-1). Idempotent. */
    cache_mark_dirty(&pool->sb->cache, page_index, FLUSH_PRIO_POOL);

    /* Clear pin state so a stray second release is a safe no-op. */
    slot->pinnedPage = 0;
}

/* ---------------------------------------------------------------------------
 * pool_free (Phase 28 W6 — closes TODO-12 slot leak)
 *
 * Free one 32-byte slot back to the pool.  Symmetric to pool_alloc:
 * O(1) per free (one CAS on the page's poolState, one write to the
 * slot's bytes 0-1 to thread it onto the per-page free list).
 *
 * Mirrors pool_alloc's per-page free-list protocol:
 *   1. Read the page's poolState: (freeCount, firstFreeSlot).
 *   2. Write the slot's bytes 0-1 = firstFreeSlot
 *      (push the slot to the head of the free list).
 *   3. CAS poolState: (freeCount, firstFreeSlot) →
 *      (freeCount + 1, slot_we_are_freeing).
 *   4. On CAS loss, re-read state and retry (bounded).
 *
 * Not idempotent: a second free on the same slot double-decrements
 * the free count and the slot appears in the free list twice.  The
 * caller is responsible for ensuring the slot is currently in use
 * (i.e., not already in the free list).  The bin job's drop
 * guarantees this: the chain entry is CAS-removed BEFORE the free
 * runs, and on a re-run the chain is already empty (the drop is a
 * no-op) and the free is skipped.
 *
 * The slot's content is NOT cleared.  The next pool_alloc will
 * overwrite the 32 bytes.  Holding a stale VP after free would see
 * the freed slot's content until re-allocation; the bin job
 * guarantees no stale VPs (the chain no longer references the slot).
 *
 * The pool page itself is NOT freed even if all its slots become
 * free.  Pool-page-level reclamation is a separate optimization
 * (TODO-12 follow-up).
 * --------------------------------------------------------------------------- */

int pool_free(Pool* pool, int64_t slot_vp) {
    if (!pool || slot_vp == VFS_VPTR_NULL) return VFS_ERR_IO;

    int64_t page_index = VFS_VPTR_PAGE(slot_vp);
    int     slot_index = VFS_VPTR_SLOT(slot_vp);
    if (page_index < 2) return VFS_ERR_IO;  /* header + superblock never freed */

    /* Bounded retry loop for the per-page CAS on poolState.
       Same pattern as pool_alloc and bin_push. */
    for (int retry = 0; retry < POOL_PUSH_MAX_RETRIES; retry++) {
        /* Pin the page so the in-cache write is durable.  pool_acquire
           copies 32 bytes; we just need the bytes 0-1 read. */
        PoolSlot slot_probe = {0};
        pool_acquire(pool, slot_vp, false, &slot_probe);
        if (slot_probe.vptr == VFS_VPTR_NULL) return VFS_ERR_IO;

        /* Read the slot's bytes 0-1 (= old "next free" pointer after
           the slot was already allocated).  We re-read these after
           CAS loss since another thread might have re-allocated
           (and overwritten) the slot. */
        uint16_t next_free_at_alloc = (uint16_t)vfs_rd2_s(
            slot_probe.bytes, 0, pool->sb->page_size);
        (void)next_free_at_alloc;  /* not used for free; we just touch the slot */

        /* Re-fetch the page payload to read poolState (the pool
           may have been evicted from cache between calls). */
        StorageReadStatus pst;
        uint8_t* payload = storage_read_with_status(pool->sb, page_index, &pst);
        if (payload == NULL) {
            if (pst == STORAGE_IO_ERROR || pst == STORAGE_CRC_ERROR) {
                fprintf(stderr, "vfs: pool_free: pool page %lld corrupted (status=%d)\n",
                        (long long)page_index, (int)pst);
            }
            return VFS_ERR_IO;
        }

        /* Read poolState */
        uint32_t state = (uint32_t)vfs_atomic_load_i32(
            (const int32_t*)(payload + POOL_OFF_STATE));
        uint16_t free_count = pool_state_free_count(state);
        uint16_t first_free = pool_state_first_free(state);

        /* Push the slot to the head of the free list.  Step 1:
           write the slot's bytes 0-1 = firstFreeSlot.  This must
           happen BEFORE the CAS, but the order doesn't matter for
           correctness — a concurrent pool_alloc could read the old
           bytes 0-1 (= our "next_free_at_alloc" above) and pop
           the slot, but the CAS in step 2 ensures the slot is
           pushed exactly once. */
        int slot_offset = VFS_POOL_ENTRIES_OFFSET + slot_index * VFS_POOL_SLOT_SIZE;
        vfs_wr2_s(payload, slot_offset, (int16_t)first_free, pool->sb->page_size);

        /* Step 2: CAS poolState.  If we lose, the next iteration
           re-reads both poolState and the slot's bytes 0-1 (the
           push of another slot may have changed firstFreeSlot). */
        uint32_t new_state = pool_state_pack((uint16_t)(free_count + 1),
                                            (uint16_t)slot_index);
        if (vfs_cas_i32((int32_t*)(payload + POOL_OFF_STATE),
                         (int32_t)state, (int32_t)new_state) == (int32_t)state) {
            /* CAS succeeded — mark the page dirty so the change
               is flushed.  Decrement the alloc counter so
               pool_alloc_count() reflects the net allocation. */
            cache_mark_dirty(&pool->sb->cache, page_index, FLUSH_PRIO_POOL);
            pool->alloc_count--;
            return VFS_OK;
        }
        /* CAS lost — retry.  The next iteration re-reads poolState
           and re-writes the slot's next_free. */
    }
    /* Retries exhausted.  The page is under heavy contention.  The
       slot's bytes 0-1 have been written N times (one per retry)
       with a stale next_free value, but the CAS never succeeded,
       so the free list is not corrupted — just the slot's
       bytes 0-1 may be inconsistent.  Return an error; the caller
       (bin job) can retry on a future pass. */
    return VFS_ERR_IO;
}
