#include "storage.h"
#include "page_buf.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------
 * Lazy mirror implementation
 *
 * First write:  generation=1, mirror_page=-1 (single copy)
 * Second write: allocate sibling, link both, generation=2
 * Subsequent:   alternate between the two pages
 * --------------------------------------------------------------------------- */

/* Read a PageHeader from disk at the given physical offset. */
static int read_page_header(StorageBackend* sb, int64_t phys_offset, PageHeader* out) {
    ssize_t n = pread(sb->fd, out, PAGE_HEADER_SIZE, phys_offset);
    return (n == PAGE_HEADER_SIZE) ? 0 : -1;
}

/* Sync in-memory generation + mirror_page from the on-disk PageHeader.
   Called lazily when generations[logical_page] is 0 but the page may have
   been written before a remount. */
static void sync_generation_from_disk(StorageBackend* sb, int64_t logical_page) {
    if (logical_page >= sb->mirror_cap) return;
    if (sb->generations[logical_page] != 0) return;  /* already synced */

    int64_t offset = indir_lookup(sb, logical_page);
    if (offset == 0) return;  /* not allocated */

    PageHeader ph;
    if (read_page_header(sb, offset, &ph) == 0) {
        while (__sync_lock_test_and_set(&sb->mirror_lock, 1)) {}
        sb->generations[logical_page]  = ph.generation;
        sb->mirror_pages[logical_page] = ph.mirror_page;
        __sync_lock_release(&sb->mirror_lock);
    }
}

/* Write a PageHeader + payload to disk at the given physical offset. */
static int write_page_record(StorageBackend* sb, int64_t phys_offset,
                             const PageHeader* ph, const uint8_t* payload) {
    ssize_t n = pwrite(sb->fd, ph, PAGE_HEADER_SIZE, phys_offset);
    if (n != PAGE_HEADER_SIZE) return -1;
    n = pwrite(sb->fd, payload, (size_t)sb->page_size, phys_offset + PAGE_HEADER_SIZE);
    if (n != sb->page_size) return -1;
    return 0;
}

/* ---------------------------------------------------------------------------
 * mirror_read
 *
 * 1. Look up indirection entry → physical offset
 * 2. Read PageHeader
 * 3. If mirror_page == -1: validate CRC, return payload
 * 4. If mirror_page != -1: read sibling, pick higher generation, validate
 * --------------------------------------------------------------------------- */

/* Compute physical offset for a logical page.
   Pages 0 and 1 have fixed offsets (header and superblock).
   Pages 2+ use the indirection table. */
static int64_t physical_offset(StorageBackend* sb, int64_t logical_page) {
    if (logical_page == 0) return 0;
    if (logical_page == 1) return sb->page_size + PAGE_HEADER_SIZE;
    return indir_lookup(sb, logical_page);
}

int mirror_read(StorageBackend* sb, int64_t logical_page, uint8_t* out_payload) {
    int64_t offset = physical_offset(sb, logical_page);
    if (offset < 0) return -1;

    PageHeader ph;
    if (read_page_header(sb, offset, &ph) != 0) return -1;

    if (ph.mirror_page == -1) {
        /* Single copy — validate and return */
        ssize_t n = pread(sb->fd, out_payload, (size_t)sb->page_size,
                          offset + PAGE_HEADER_SIZE);
        if (n != sb->page_size) return -1;

        uint32_t crc = vfs_crc32c(out_payload, (size_t)sb->page_size);
        if (crc != ph.checksum) return -1;

        /* Update in-memory tracking */
        if (logical_page < sb->mirror_cap) {
            while (__sync_lock_test_and_set(&sb->mirror_lock, 1)) {}
            sb->generations[logical_page] = ph.generation;
            sb->mirror_pages[logical_page] = ph.mirror_page;
            __sync_lock_release(&sb->mirror_lock);
        }
        return 0;
    }

    /* Mirror exists — read both headers, pick higher generation */
    int64_t sibling_offset = indir_lookup(sb, ph.mirror_page);
    if (sibling_offset == 0) {
        /* Sibling has no indirection entry — use original */
        ssize_t n = pread(sb->fd, out_payload, (size_t)sb->page_size,
                          offset + PAGE_HEADER_SIZE);
        if (n != sb->page_size) return -1;
        uint32_t crc = vfs_crc32c(out_payload, (size_t)sb->page_size);
        return (crc == ph.checksum) ? 0 : -1;
    }

    PageHeader ph_sib;
    if (read_page_header(sb, sibling_offset, &ph_sib) != 0) {
        /* Can't read sibling — use original */
        ssize_t n = pread(sb->fd, out_payload, (size_t)sb->page_size,
                          offset + PAGE_HEADER_SIZE);
        if (n != sb->page_size) return -1;
        uint32_t crc = vfs_crc32c(out_payload, (size_t)sb->page_size);
        return (crc == ph.checksum) ? 0 : -1;
    }

    /* Pick the page with higher generation */
    int64_t  active_offset;
    PageHeader active_ph;

    if (ph_sib.generation > ph.generation) {
        active_offset = sibling_offset;
        active_ph     = ph_sib;
    } else {
        active_offset = offset;
        active_ph     = ph;
    }

    /* Try the active half */
    ssize_t n = pread(sb->fd, out_payload, (size_t)sb->page_size,
                      active_offset + PAGE_HEADER_SIZE);
    if (n == sb->page_size) {
        uint32_t crc = vfs_crc32c(out_payload, (size_t)sb->page_size);
        if (crc == active_ph.checksum) {
            /* Update tracking */
            if (logical_page < sb->mirror_cap) {
                while (__sync_lock_test_and_set(&sb->mirror_lock, 1)) {}
                sb->generations[logical_page] = active_ph.generation;
                sb->mirror_pages[logical_page] = active_ph.mirror_page;
                __sync_lock_release(&sb->mirror_lock);
            }
            return 0;
        }
    }

    /* Active half failed CRC — try the other */
    int64_t  other_offset = (active_offset == offset) ? sibling_offset : offset;
    PageHeader other_ph   = (active_offset == offset) ? ph_sib : ph;

    n = pread(sb->fd, out_payload, (size_t)sb->page_size,
              other_offset + PAGE_HEADER_SIZE);
    if (n == sb->page_size) {
        uint32_t crc = vfs_crc32c(out_payload, (size_t)sb->page_size);
        if (crc == other_ph.checksum) {
            if (logical_page < sb->mirror_cap) {
                while (__sync_lock_test_and_set(&sb->mirror_lock, 1)) {}
                sb->generations[logical_page] = other_ph.generation;
                sb->mirror_pages[logical_page] = other_ph.mirror_page;
                __sync_lock_release(&sb->mirror_lock);
            }
            return 0;
        }
    }

    return -1;  /* both halves corrupt */
}

/* ---------------------------------------------------------------------------
 * mirror_write
 *
 * 1. Never written (gen=0): first write, single copy
 * 2. Written once (gen>=1, mirror=-1): allocate sibling, link
 * 3. Mirror exists: alternate write to inactive half
 * --------------------------------------------------------------------------- */

int mirror_write(StorageBackend* sb, int64_t logical_page, const uint8_t* payload,
                 uint32_t flags) {
    int64_t offset = physical_offset(sb, logical_page);
    if (offset < 0) return -1;

    /* Ensure mirror tracking */
    ensure_mirror_arrays(sb, (int)(logical_page + 1));

    /* Sync generation from disk if in-memory is stale (after remount) */
    sync_generation_from_disk(sb, logical_page);

    uint32_t gen = sb->generations[logical_page];
    int32_t  mp  = sb->mirror_pages[logical_page];

    uint32_t crc = vfs_crc32c(payload, (size_t)sb->page_size);

    if (gen == 0) {
        /* --- First write: single copy --- */
        PageHeader ph;
        memset(&ph, 0, sizeof(ph));
        ph.flags       = flags;
        ph.checksum    = crc;
        ph.generation  = 1;
        ph.mirror_page = -1;

        if (write_page_record(sb, offset, &ph, payload) != 0) return -1;

        while (__sync_lock_test_and_set(&sb->mirror_lock, 1)) {}
        sb->generations[logical_page]  = 1;
        sb->mirror_pages[logical_page] = -1;
        __sync_lock_release(&sb->mirror_lock);
        return 0;
    }

    if (mp == -1) {
        /* --- Second write: allocate mirror sibling --- */
        int64_t sibling = storage_allocate(sb, 1);
        if (sibling < 0) return -1;

        int64_t sibling_offset = indir_lookup(sb, sibling);
        if (sibling_offset == 0) return -1;

        /* Write to sibling */
        PageHeader ph_sib;
        memset(&ph_sib, 0, sizeof(ph_sib));
        ph_sib.flags       = flags;
        ph_sib.checksum    = crc;
        ph_sib.generation  = gen + 1;
        ph_sib.mirror_page = (int32_t)logical_page;

        if (write_page_record(sb, sibling_offset, &ph_sib, payload) != 0) return -1;

        /* Link original → sibling (atomic store of mirror_page) */
        PageHeader ph_orig;
        if (read_page_header(sb, offset, &ph_orig) != 0) return -1;
        ph_orig.mirror_page = (int32_t)sibling;
        /* Atomically update only the mirror_page field on disk */
        ssize_t n = pwrite(sb->fd, &ph_orig.mirror_page, 4,
                           offset + offsetof(PageHeader, mirror_page));
        if (n != 4) return -1;

        /* Update in-memory tracking */
        while (__sync_lock_test_and_set(&sb->mirror_lock, 1)) {}
        sb->mirror_pages[logical_page] = (int32_t)sibling;
        sb->generations[logical_page]  = gen + 1;
        __sync_lock_release(&sb->mirror_lock);

        /* Also update sibling tracking */
        ensure_mirror_arrays(sb, (int)(sibling + 1));
        while (__sync_lock_test_and_set(&sb->mirror_lock, 1)) {}
        sb->mirror_pages[sibling] = (int32_t)logical_page;
        sb->generations[sibling]  = 0;
        __sync_lock_release(&sb->mirror_lock);

        return 0;
    }

    /* --- Subsequent write: alternate between two pages --- */
    /* Find the page with LOWER generation (inactive half) */
    int64_t sib_offset = indir_lookup(sb, mp);
    if (sib_offset == 0) return -1;

    PageHeader ph_orig, ph_sib;
    if (read_page_header(sb, offset, &ph_orig) != 0) return -1;
    if (read_page_header(sb, sib_offset, &ph_sib) != 0) return -1;

    /* Write to the inactive half (lower generation) */
    int64_t  target_offset;
    PageHeader target_ph;

    if (ph_orig.generation <= ph_sib.generation) {
        target_offset = offset;
        target_ph     = ph_orig;
    } else {
        target_offset = sib_offset;
        target_ph     = ph_sib;
    }

    uint32_t new_gen = (ph_orig.generation > ph_sib.generation
                        ? ph_orig.generation : ph_sib.generation) + 1;

    target_ph.flags      = flags;
    target_ph.checksum   = crc;
    target_ph.generation = new_gen;

    if (write_page_record(sb, target_offset, &target_ph, payload) != 0) return -1;

    while (__sync_lock_test_and_set(&sb->mirror_lock, 1)) {}
    sb->generations[logical_page] = new_gen;
    __sync_lock_release(&sb->mirror_lock);
    return 0;
}
