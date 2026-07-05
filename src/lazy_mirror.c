#include "storage.h"
#include "page_buf.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Read a PageHeader from disk at the given physical offset. */
static int read_page_header(StorageBackend* sb, int64_t phys_offset, PageHeader* out) {
    ssize_t n = pread(sb->fd, out, PAGE_HEADER_SIZE, phys_offset);
    return (n == PAGE_HEADER_SIZE) ? 0 : -1;
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

/* Compute physical offset for a logical page.
   Pages 0 and 1 have fixed offsets (header and superblock).
   Pages 2+ use the indirection table. */
static int64_t physical_offset(StorageBackend* sb, int64_t logical_page) {
    if (logical_page == 0) return 0;
    if (logical_page == 1) return sb->page_size + PAGE_HEADER_SIZE;
    return indir_lookup(sb, logical_page);
}

/* ---------------------------------------------------------------------------
 * mirror_read
 *
 * 1. Look up indirection entry → physical offset
 * 2. Read PageHeader from disk
 * 3. If mirror_page == -1: validate CRC, return payload
 * 4. If mirror_page != -1: read sibling, pick higher generation, validate
 * --------------------------------------------------------------------------- */

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
        if (crc == active_ph.checksum) return 0;
    }

    /* Active half failed CRC — try the other */
    int64_t  other_offset = (active_offset == offset) ? sibling_offset : offset;
    PageHeader other_ph   = (active_offset == offset) ? ph_sib : ph;

    n = pread(sb->fd, out_payload, (size_t)sb->page_size,
              other_offset + PAGE_HEADER_SIZE);
    if (n == sb->page_size) {
        uint32_t crc = vfs_crc32c(out_payload, (size_t)sb->page_size);
        if (crc == other_ph.checksum) return 0;
    }

    return -1;  /* both halves corrupt */
}

/* ---------------------------------------------------------------------------
 * mirror_write
 *
 * All state is read from the on-disk PageHeader.  No in-memory arrays.
 * 1. Never written (gen=0 on disk): first write, single copy
 * 2. Written once (gen>=1, mirror=-1): allocate sibling, link
 * 3. Mirror exists: alternate write to inactive half
 * --------------------------------------------------------------------------- */

int mirror_write(StorageBackend* sb, int64_t logical_page, const uint8_t* payload,
                 uint32_t flags) {
    int64_t offset = physical_offset(sb, logical_page);
    if (offset < 0) return -1;

    uint32_t crc = vfs_crc32c(payload, (size_t)sb->page_size);

    /* Read current PageHeader to determine state */
    PageHeader ph;
    if (read_page_header(sb, offset, &ph) != 0) {
        /* Page has no valid header — first write */
        memset(&ph, 0, sizeof(ph));
        ph.generation  = 0;
        ph.mirror_page = -1;
    }

    if (ph.generation == 0) {
        /* --- First write: single copy --- */
        ph.flags       = flags;
        ph.checksum    = crc;
        ph.generation  = 1;
        ph.mirror_page = -1;
        return write_page_record(sb, offset, &ph, payload) == 0 ? 0 : -1;
    }

    if (ph.mirror_page == -1) {
        /* --- Second write: allocate mirror sibling --- */
        int64_t sibling = storage_allocate(sb, 1);
        if (sibling < 0) return -1;

        int64_t sib_off = indir_lookup(sb, sibling);
        if (sib_off == 0) return -1;

        PageHeader ph_sib;
        memset(&ph_sib, 0, sizeof(ph_sib));
        ph_sib.flags       = flags;
        ph_sib.checksum    = crc;
        ph_sib.generation  = ph.generation + 1;
        ph_sib.mirror_page = (int32_t)logical_page;

        if (write_page_record(sb, sib_off, &ph_sib, payload) != 0) return -1;

        /* Link original → sibling */
        ph.mirror_page = (int32_t)sibling;
        ssize_t n = pwrite(sb->fd, &ph.mirror_page, 4,
                           offset + offsetof(PageHeader, mirror_page));
        return (n == 4) ? 0 : -1;
    }

    /* --- Subsequent write: alternate between the two pages --- */
    int64_t sib_offset = indir_lookup(sb, ph.mirror_page);
    if (sib_offset == 0) return -1;

    PageHeader ph_sib;
    if (read_page_header(sb, sib_offset, &ph_sib) != 0) return -1;

    /* Write to the inactive half (lower generation) */
    int64_t  target_offset;
    PageHeader target_ph;

    if (ph.generation <= ph_sib.generation) {
        target_offset = offset;
        target_ph     = ph;
    } else {
        target_offset = sib_offset;
        target_ph     = ph_sib;
    }

    uint32_t new_gen = (ph.generation > ph_sib.generation
                        ? ph.generation : ph_sib.generation) + 1;

    target_ph.flags      = flags;
    target_ph.checksum   = crc;
    target_ph.generation = new_gen;

    return write_page_record(sb, target_offset, &target_ph, payload) == 0 ? 0 : -1;
}
