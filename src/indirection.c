#include "storage.h"
#include "page_buf.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------
 * Indirection table implementation
 *
 * Maps logical_page → physical_byte_offset.
 * Inline entries are in the header page payload at offset 40+.
 * Overflow pages are chained from indirection_head.
 * --------------------------------------------------------------------------- */

void indir_init(StorageBackend* sb) {
    IndirectionTable* it = &sb->indir;

    /* Inline entries point into the header buffer */
    it->inline_entries = (int64_t*)(sb->header_buf + HDR_OFF_ENTRIES);
    it->inline_count   = inline_entry_count(sb->page_size);
    it->entries_per_overflow = (sb->page_size / 8) - 1;

    /* Load overflow chain from disk */
    it->overflow_pages  = NULL;
    it->overflow_logical = NULL;
    it->overflow_count  = 0;
    it->overflow_cap    = 0;

    int64_t chain_page = sb->indirection_head;
    while (chain_page != 0) {
        /* Look up the physical offset for this overflow page.  After
           each iteration the in-RAM indirection table is partially
           built (overflow_pages[0..i-1] registered), so indir_lookup
           can resolve subsequent chain links.  indir_lookup also
           handles inline entries (chain_page < inline_count) via the
           header buffer. */
        int64_t offset = indir_lookup(sb, chain_page);
        if (offset == 0) break;

        /* Read the overflow page */
        uint8_t* buf = calloc(1, (size_t)sb->page_size);
        if (!buf) break;

        /* Read PageHeader + payload */
        PageHeader ph;
        if (pread(sb->fd, &ph, PAGE_HEADER_SIZE, offset) != PAGE_HEADER_SIZE) {
            free(buf);
            break;
        }
        if (pread(sb->fd, buf, (size_t)sb->page_size,
                  offset + PAGE_HEADER_SIZE) != sb->page_size) {
            free(buf);
            break;
        }

        /* Grow overflow_pages array */
        if (it->overflow_count >= it->overflow_cap) {
            int new_cap = it->overflow_cap ? it->overflow_cap * 2 : 8;
            int64_t** np = realloc(it->overflow_pages, (size_t)new_cap * sizeof(int64_t*));
            int64_t*  nl = realloc(it->overflow_logical, (size_t)new_cap * sizeof(int64_t));
            if (!np || !nl) { free(buf); free(np); free(nl); break; }
            it->overflow_pages   = np;
            it->overflow_logical = nl;
            it->overflow_cap     = new_cap;
        }

        it->overflow_pages[it->overflow_count]   = (int64_t*)buf;
        it->overflow_logical[it->overflow_count]  = chain_page;
        it->overflow_count++;

        /* Follow chain: next is at offset 0 of the overflow page */
        chain_page = vfs_rd8_s(buf, 0, sb->page_size);
    }
}

int64_t indir_lookup(StorageBackend* sb, int64_t logical_page) {
    IndirectionTable* it = &sb->indir;

    if (logical_page < it->inline_count) {
        return it->inline_entries[logical_page];
    }

    int64_t remaining = logical_page - it->inline_count;
    int64_t overflow_idx = remaining / it->entries_per_overflow;
    int64_t entry_idx    = remaining % it->entries_per_overflow;

    if (overflow_idx >= it->overflow_count) return 0;  /* beyond end of table */

    int64_t* entries = it->overflow_pages[overflow_idx];
    /* Entry starts at offset 8 (after the 'next' pointer) */
    return entries[1 + entry_idx];
}

void indir_set(StorageBackend* sb, int64_t logical_page, int64_t physical_offset) {
    IndirectionTable* it = &sb->indir;

    if (logical_page < it->inline_count) {
        vfs_atomic_store_i64(&it->inline_entries[logical_page], physical_offset);
        /* Mark header page dirty */
        cache_mark_dirty(&sb->cache, 0, FLUSH_PRIO_SUPERBLOCK);
        return;
    }

    int64_t remaining = logical_page - it->inline_count;
    int64_t overflow_idx = remaining / it->entries_per_overflow;
    int64_t entry_idx    = remaining % it->entries_per_overflow;

    if (overflow_idx < it->overflow_count) {
        int64_t* entries = it->overflow_pages[overflow_idx];
        vfs_atomic_store_i64(&entries[1 + entry_idx], physical_offset);
    }
}

/* ---------------------------------------------------------------------------
 * Ensure the indirection table has capacity for at least 'needed' more
 * entries beyond what currently exists.
 * --------------------------------------------------------------------------- */

int indir_ensure_capacity(StorageBackend* sb, int needed) {
    IndirectionTable* it = &sb->indir;
    int64_t required = sb->total_pages + needed;

    /* ---- Fast path: lock-free read-only check ----
       The early-out is strict < (NOT <=).  At
       required == total_entries, the next slot to claim is
       the self-ref of a not-yet-allocated overflow page
       (the self-ref at the end of the previous overflow,
       or inline[inline_count-1] for the first overflow).
       That slot's indirection entry is in the previous
       overflow's last entry (which we've just read) — so
       indir_lookup can return its value.  But the value is
       0 (overflow not allocated), and storage_allocate's
       do-while would then CAS-set it to a DATA page's
       physical, corrupting the indirection table.

       Hence we must allocate a new overflow page BEFORE
       sb->total_pages reaches the self-ref slot.  The strict
       < ensures we always have an overflow ready when
       required crosses the self-ref boundary.

       Stale reads of overflow_count are tolerable here — at
       worst we acquire the lock unnecessarily, do the
       re-check, and return 0 from the slow path.  This is
       the standard double-checked-locking pattern. */
    int64_t total_entries = (int64_t)it->inline_count +
                            (int64_t)it->overflow_count * it->entries_per_overflow;
    if (required < total_entries) return 0;

    /* ---- Slow path: acquire the lock, re-check, allocate ---- */
    while (__sync_lock_test_and_set(&it->overflow_lock, 1)) { /* spin */ }

    /* Re-check under the lock — another thread may have
       allocated while we were waiting.  No lock-fast-path can
       race past the early-out; the lock is the single
       synchronization point for overflow-page allocation. */
    total_entries = (int64_t)it->inline_count +
                    (int64_t)it->overflow_count * it->entries_per_overflow;
    if (required < total_entries) {
        __sync_lock_release(&it->overflow_lock);
        return 0;
    }

    /* The lock is held for the entire allocation loop.  We're
       alone on the indirection table and the chain, so:
         - The chain-link CAS (step 6) always succeeds (no
           concurrent writer to prev[0]).
         - The overflow-array realloc / assignment (steps 5, 7)
           is safe without further synchronization.
         - The 'continue' retry path that the C6 iterative
           fix added is unnecessary — the chain-link CAS
           cannot fail.  We keep the loop structure but the
           iteration is guaranteed to complete.
       The loop test '>=' can over-allocate by 1 page in the
       boundary case (required exactly equals a freshly-grown
       total_entries — we exit with strictly more than needed);
       this is harmless and the cost of being conservative
       about the self-reference boundary. */
    while (required >= total_entries) {
        /* Allocate a new overflow page by advancing physical_tail.
           physical_tail is a public field, not protected by our
           lock — use CAS to reserve a unique offset. */
        int64_t old_tail = sb->physical_tail;
        int64_t new_tail = old_tail + phys_record_size(sb);
        while (vfs_cas_i64(&sb->physical_tail, old_tail, new_tail) != old_tail) {
            old_tail = sb->physical_tail;
            new_tail = old_tail + phys_record_size(sb);
        }
        int64_t new_page_phys = old_tail;

        /* Allocate a buffer for the overflow page */
        uint8_t* buf = calloc(1, (size_t)sb->page_size);
        if (!buf) {
            __sync_lock_release(&it->overflow_lock);
            return -1;
        }

        /* Initialize: next = 0, all entries = 0 */
        vfs_wr8_s(buf, 0, 0, sb->page_size);  /* next pointer */

        /* The new overflow page's logical index — the self-ref
           logical.  This is the indirection entry that points
           to the new overflow page itself, so indir_lookup can
           find it on subsequent access.

           Layout invariant: the self-ref of overflow[K] lives in
           the LAST indirection entry of overflow[K-1] (for K>=1),
           or in inline[inline_count-1] for K=0.  Concretely:

             inline[0..inline_count-2]  = data (logical 0..inline_count-2)
             inline[inline_count-1]     = self-ref of overflow[0]
                                           (logical inline_count-1)
             overflow[0][1..14]         = data (logicals inline_count+1..inline_count+14)
             overflow[0][15]            = self-ref of overflow[1]
                                           (logical inline_count-1 + entries_per_overflow)
             overflow[K][1..14]         = data (logicals inline_count+1+K*15..inline_count+14+K*15)
             overflow[K][15]            = self-ref of overflow[K+1]
                                           (logical inline_count-1 + (K+1)*entries_per_overflow)

           The chain links in buf[0] are LOGICALS (the next
           overflow's self-ref logical), not physicals.  GC needs
           to be able to move pages without breaking the chain,
           so the chain must use the indirection table to resolve
           physicals.  The indirection table is consistent: the
           entry for the next overflow's self-ref is in the
           CURRENT overflow's last entry (which we've just
           written), so indir_init can walk the chain on load
           using indir_lookup at each step — no chicken-and-egg.

           The OLD design used sb->total_pages here, which is the
           next DATA logical to allocate.  That worked for the
           first 9 overflows (self-refs landed at logicals 2..10
           in the inline area), but broke at the 10th overflow:
           sb->total_pages crossed into overflow[0]'s data range
           and the code wrote the new overflow's physical into a
           DATA slot of overflow[0] (logical 11), corrupting the
           indirection table.  Using the canonical self-ref
           logical avoids that entirely.

           sb->total_pages is NOT bumped here — storage_allocate
           owns that field.  The do-while retry in storage_allocate
           skips self-ref slots (try_claim_entry returns 0 because
           the entry is already set to the new overflow's physical,
           and the inner CAS bumps sb->total_pages past it). */
        int64_t new_logical = (int64_t)it->inline_count - 1 +
                              (int64_t)it->overflow_count * it->entries_per_overflow;

        /* Set the indirection entry for this new overflow page's
           self-ref.  Safe under the lock: no other thread is
           reading or writing this slot.

           K=0:  new_logical = inline_count - 1 < inline_count, so
                 the entry goes into the inline array at the LAST
                 slot.
           K>=1: new_logical = inline_count - 1 + K*15.  Subtract
                 inline_count to get rem = K*15 - 1.  oidx = (K*15-1)/15
                 = K-1, eidx = (K*15-1)%15 = 14.  So the entry
                 goes into overflow_pages[K-1][1+14] = overflow_pages[K-1][15]
                 = the LAST indirection entry of the previous
                 overflow.  This is the "self-ref in previous
                 overflow's last entry" invariant.

           The 'oidx == overflow_count' branch is unreachable in
           this design (the new overflow's own self-ref is never
           in itself) but kept defensively. */
        if (new_logical < it->inline_count) {
            it->inline_entries[new_logical] = new_page_phys;
        } else {
            int64_t rem = new_logical - it->inline_count;
            int64_t oidx = rem / it->entries_per_overflow;
            int64_t eidx = rem % it->entries_per_overflow;
            if (oidx < it->overflow_count) {
                /* Self-ref lands in the LAST indirection entry
                   of the previous overflow. */
                it->overflow_pages[oidx][1 + eidx] = new_page_phys;
            } else {
                /* Unreachable in this design — kept defensively. */
                ((int64_t*)buf)[1 + eidx] = new_page_phys;
            }
        }

        /* sb->total_pages is intentionally NOT bumped here.  See
           the comment above 'new_logical' for why storage_allocate
           owns the bump. */

        /* Grow overflow arrays if needed.  Lock is already held
           — no per-step spinlock needed. */
        if (it->overflow_count >= it->overflow_cap) {
            int new_cap = it->overflow_cap ? it->overflow_cap * 2 : 8;
            int64_t** np = realloc(it->overflow_pages, (size_t)new_cap * sizeof(int64_t*));
            int64_t*  nl = realloc(it->overflow_logical, (size_t)new_cap * sizeof(int64_t));
            if (!np || !nl) {
                __sync_lock_release(&it->overflow_lock);
                free(buf); free(np); free(nl);
                return -1;
            }
            it->overflow_pages   = np;
            it->overflow_logical = nl;
            __sync_synchronize();
            it->overflow_cap     = new_cap;
        }

        /* Link the chain.  The chain stores LOGICALS (the next
           overflow's self-ref logical), not physicals.  This
           lets GC move pages later — the indirection table
           resolves physicals via indir_lookup, and as long as
           the table is kept consistent, the chain works.

           indir_init walks the chain like this:
             chain_page = indirection_head (= self-ref logical of
                          the first overflow = inline_count - 1)
             indir_lookup(chain_page) → physical of overflow[0]
                                           (resolved via inline[chain_page]
                                            for K=0, or via
                                            overflow[K-1][15] for K>=1)
             read overflow[0] from disk
             chain_page = overflow[0][0] (= self-ref logical of
                          overflow[1] = inline_count - 1 + 1*15)
             indir_lookup(chain_page) → physical of overflow[1]
                                           (via overflow[0][15])
             ... (repeats)

           The indirection entry for chain_page (the next
           overflow's self-ref) is in the CURRENT overflow's
           last entry (or inline for K=0), which we just
           wrote.  No chicken-and-egg.

           With the lock held, prev[0] is guaranteed to be 0
           (no other writer to this slot) — no CAS retry needed. */
        if (it->overflow_count > 0) {
            int64_t* prev = it->overflow_pages[it->overflow_count - 1];
            vfs_atomic_store_i64(&prev[0], new_logical);
        } else {
            /* First overflow page — CAS-update indirection_head
               to the new page's self-ref logical. */
            int64_t expected_head = sb->indirection_head;
            while (vfs_cas_i64(&sb->indirection_head, expected_head, new_logical) != expected_head) {
                expected_head = sb->indirection_head;
            }
        }

        it->overflow_pages[it->overflow_count]   = (int64_t*)buf;
        it->overflow_logical[it->overflow_count]  = new_logical;
        it->overflow_count++;
        total_entries += it->entries_per_overflow;
    }

    __sync_lock_release(&it->overflow_lock);
    return 0;
}
