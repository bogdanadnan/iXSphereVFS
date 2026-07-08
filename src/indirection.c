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
        int64_t offset = 0;
        /* Look up the physical offset for this overflow page */
        if (chain_page < it->inline_count) {
            offset = it->inline_entries[chain_page];
        } else {
            /* It's in a previous overflow page — but we load sequentially,
               so this should have been set up by a prior iteration */
            break;  /* shouldn't happen in a well-formed file */
        }
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

    int64_t total_entries = (int64_t)it->inline_count +
                            (int64_t)it->overflow_count * it->entries_per_overflow;

    /* Check if we already have enough entries */
    /* We need entries up to at least total_pages + needed */
    int64_t required = sb->total_pages + needed;
    if (required < total_entries) return 0;

    /* Need to allocate overflow pages */
    while (required >= total_entries) {
        /* Allocate a new overflow page by advancing physical_tail */
        int64_t old_tail = sb->physical_tail;
        int64_t new_tail = old_tail + phys_record_size(sb);
        while (vfs_cas_i64(&sb->physical_tail, old_tail, new_tail) != old_tail) {
            old_tail = sb->physical_tail;
            new_tail = old_tail + phys_record_size(sb);
        }
        int64_t new_page_phys = old_tail;

        /* Allocate a buffer for the overflow page */
        uint8_t* buf = calloc(1, (size_t)sb->page_size);
        if (!buf) return -1;

        /* Initialize: next = 0, all entries = 0 */
        vfs_wr8_s(buf, 0, 0, sb->page_size);  /* next pointer */

        /* Register the new overflow page in the indirection table.
           Its logical page is the next one after current total_pages. */
        int64_t new_logical = sb->total_pages;

        /* Ensure mirror arrays */

        /* Set the indirection entry for this new overflow page */
        if (new_logical < it->inline_count) {
            it->inline_entries[new_logical] = new_page_phys;
        } else {
            int64_t rem = new_logical - it->inline_count;
            int64_t oidx = rem / it->entries_per_overflow;
            int64_t eidx = rem % it->entries_per_overflow;
            if (oidx < it->overflow_count) {
                /* Entry goes into a previous overflow page */
                it->overflow_pages[oidx][1 + eidx] = new_page_phys;
            } else {
                /* oidx == overflow_count — the new page covers its own entry.
                   Write into the freshly allocated buffer before appending. */
                ((int64_t*)buf)[1 + eidx] = new_page_phys;
            }
        }

        /* Update total_pages */
        int64_t old_tp = sb->total_pages;
        while (vfs_cas_i64(&sb->total_pages, old_tp, new_logical + 1) != old_tp) {
            old_tp = sb->total_pages;
        }

        /* Append to overflow chain — grow arrays under lock */
        if (it->overflow_count >= it->overflow_cap) {
            while (__sync_lock_test_and_set(&it->overflow_lock, 1)) { /* spin */ }

            /* Double-check after acquiring lock */
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

            __sync_lock_release(&it->overflow_lock);
        }

        /* Link previous overflow page's 'next' to this new page's logical index.
           Use CAS to ensure atomic chain append — another thread may be appending
           simultaneously. */
        if (it->overflow_count > 0) {
            int64_t* prev = it->overflow_pages[it->overflow_count - 1];
            int64_t expected_next = 0;
            if (vfs_cas_i64(&prev[0], expected_next, new_logical) != expected_next) {
                /* Another thread appended — retry the entire capacity check */
                free(buf);
                return indir_ensure_capacity(sb, needed);
            }
        } else {
            /* First overflow page — CAS-update indirection_head */
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

    return 0;
}
