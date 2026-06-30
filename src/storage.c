/*
 * src/storage.c — StorageBackend Implementation (Phase 2.1-2.2)
 *
 * File layout, XVFS magic, and header management.
 */
#include "ixsphere_vfs.h"
#include "vfs_internal.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

/* Physical page: PageHeader (16 bytes) + payload (page_size bytes) */
#define PHYSICAL_PAGE_HEADER sizeof(PageHeader)

/* Offsets within the payload (not including PageHeader) */
#define OFFSET_TOTAL_PAGES   0
#define OFFSET_PAGE_SIZE     8
#define OFFSET_SEGMENT_SIZE  16
#define OFFSET_BITMAP_DIR    32

/* Max bitmap_dir entries: (page_size - 32) / 8 entries per header page, 2 pages total */
#define MAX_BITMAP_DIR_ENTRIES_PER_PAGE(page_size) ((page_size - 32) / 8)
#define MAX_BITMAP_DIR_ENTRIES(page_size) (2 * ((page_size) - 32) / 8)

/* Helper to compute bits per page from runtime page_size */
static inline int bits_per_page(uint64_t page_size) {
    return (int)(page_size * 8);
}

/*
 * vfs_open - Open an existing VFS file (fails if not found)
 */
vfs_t* vfs_open(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return NULL; /* File doesn't exist */
    }
    
    vfs_t* vfs = calloc(1, sizeof(vfs_t));
    if (!vfs) return NULL;
    
    StorageBackend* sb = &vfs->backend;
    strncpy(sb->path, path, sizeof(sb->path) - 1);
    sb->fd = -1;
    sb->zone_hint_cursor = 4;
    pthread_mutex_init(&sb->bitmap_lock, NULL);
    
    sb->fd = open(path, O_RDWR);
    if (sb->fd < 0) goto fail;
    
    /* First, read just enough to extract the page_size field (at offset 8) */
    uint8_t header_partial[PHYSICAL_PAGE_HEADER + 24]; /* header + total_pages + page_size + segment_size */
    if (pread(sb->fd, header_partial, sizeof(header_partial), 0) != sizeof(header_partial)) {
        close(sb->fd);
        sb->fd = -1;
        goto fail;
    }
    
    PageHeader* header = (PageHeader*)header_partial;
    
    /* Validate XVFS magic before trusting page_size */
    if (header->pageType != XVFS_MAGIC_TYPE || header->flags != XVFS_MAGIC_FLAGS) {
        close(sb->fd);
        sb->fd = -1;
        goto fail;
    }
    
    /* Get page_size BEFORE allocating buffers */
    uint64_t* fields = (uint64_t*)(header_partial + PHYSICAL_PAGE_HEADER);
    sb->page_size = fields[OFFSET_PAGE_SIZE / 8];
    if (sb->page_size == 0) sb->page_size = VFS_PAGE_SIZE;
    
    /* Read segment_size if present */
    uint32_t* seg_ptr = (uint32_t*)(header_partial + PHYSICAL_PAGE_HEADER + OFFSET_SEGMENT_SIZE);
    sb->segment_size = (seg_ptr[0] == 0) ? 1024 : seg_ptr[0];
    
    /* Now read the full header page with correct page size */
    uint8_t* phys_buffer = malloc(PHYSICAL_PAGE_HEADER + sb->page_size);
    if (!phys_buffer) {
        close(sb->fd);
        sb->fd = -1;
        goto fail;
    }
    memcpy(phys_buffer, header_partial, sizeof(header_partial));
    
    /* Read the rest of the header page (from offset 32 to page_size) */
    ssize_t remaining = (ssize_t)(sb->page_size - 32);
    uint8_t* rest = phys_buffer + PHYSICAL_PAGE_HEADER + OFFSET_BITMAP_DIR;
    if (pread(sb->fd, rest, remaining, PHYSICAL_PAGE_HEADER + OFFSET_BITMAP_DIR) != remaining) {
        free(phys_buffer);
        close(sb->fd);
        sb->fd = -1;
        goto fail;
    }
    
    header = (PageHeader*)phys_buffer;
    uint8_t* payload = phys_buffer + PHYSICAL_PAGE_HEADER;
    
    /* Validate CRC32C on full payload */
    uint32_t expected_crc = vfs_crc32c(payload, sb->page_size);
    if (expected_crc != header->checksum) {
        free(phys_buffer);
        close(sb->fd);
        sb->fd = -1;
        goto fail;
    }
    
    /* Read header fields */
    sb->total_pages = ((uint64_t*)payload)[OFFSET_TOTAL_PAGES / 8];
    
    /* Load bitmap_dir from page 0 and page 1 */
    /* Each header page holds (page_size - 32) / 8 entries */
    int entries_per_page = (int)((sb->page_size - 32) / 8);
    int i;
    int64_t* bitmap_dir = (int64_t*)(payload + OFFSET_BITMAP_DIR);
    for (i = 0; i < entries_per_page && bitmap_dir[i] != 0; i++) {
        sb->bitmap_dir[i] = bitmap_dir[i];
    }
    
    /* If we exhausted page 0, load from page 1 */
    if (i == entries_per_page) {
        /* Read header page 1 */
        uint8_t* page1_buffer = malloc(PHYSICAL_PAGE_HEADER + sb->page_size);
        if (page1_buffer) {
            off_t page1_offset = PHYSICAL_PAGE_HEADER + sb->page_size;
            if (pread(sb->fd, page1_buffer, PHYSICAL_PAGE_HEADER + sb->page_size, page1_offset) == 
                (ssize_t)(PHYSICAL_PAGE_HEADER + sb->page_size)) {
                int64_t* page1_dir = (int64_t*)(page1_buffer + PHYSICAL_PAGE_HEADER);
                int max_entries = MAX_BITMAP_DIR_ENTRIES(sb->page_size);
                for (int j = 0; i < max_entries && page1_dir[j] != 0; j++, i++) {
                    sb->bitmap_dir[i] = page1_dir[j];
                }
            }
            free(page1_buffer);
        }
    }
    sb->bitmap_count = i;
    
    sb->initialized = 1;
    
    /* Allocate working buffer */
    sb->buffer = malloc(sb->page_size);
    if (!sb->buffer) {
        free(phys_buffer);
        close(sb->fd);
        sb->fd = -1;
        goto fail;
    }
    
    free(phys_buffer);
    return vfs;
    
fail:
    free(vfs);
    return NULL;
}

/*
 * vfs_create - Create a new VFS file with specified page size
 */
vfs_t* vfs_create(const char* path, uint64_t page_size) {
    if (page_size == 0) page_size = VFS_PAGE_SIZE;
    
    vfs_t* vfs = calloc(1, sizeof(vfs_t));
    if (!vfs) return NULL;
    
    StorageBackend* sb = &vfs->backend;
    strncpy(sb->path, path, sizeof(sb->path) - 1);
    sb->page_size = page_size;
    sb->zone_hint_cursor = 4;
    pthread_mutex_init(&sb->bitmap_lock, NULL);
    
    sb->fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (sb->fd < 0) goto fail;
    
    /* Allocate buffers based on actual page size */
    uint8_t* phys_buffer = malloc(PHYSICAL_PAGE_HEADER + page_size);
    uint8_t* payload = phys_buffer + PHYSICAL_PAGE_HEADER;
    if (!phys_buffer) goto fail_close;
    
    /* Page 0: StorageBackend header with XVFS magic */
    memset(phys_buffer, 0, PHYSICAL_PAGE_HEADER + page_size);
    PageHeader* header = (PageHeader*)phys_buffer;
    header->pageType = XVFS_MAGIC_TYPE;
    header->flags = XVFS_MAGIC_FLAGS;
    header->generation = 1;
    header->mirrorPage = -1;
    
    /* Set config fields */
    ((uint64_t*)payload)[OFFSET_TOTAL_PAGES / 8] = 4;
    ((uint64_t*)payload)[OFFSET_PAGE_SIZE / 8] = page_size;
    
    /* Set segment_size at offset 16 (default 1024) */
    uint32_t* seg_ptr = (uint32_t*)(payload + OFFSET_SEGMENT_SIZE);
    seg_ptr[0] = 1024;
    
    /* bitmap_dir[0] = 2 (first bitmap page is logical page 2) */
    ((int64_t*)payload)[OFFSET_BITMAP_DIR / 8] = 2;
    
    header->checksum = vfs_crc32c(payload, page_size);
    
    if (pwrite(sb->fd, phys_buffer, PHYSICAL_PAGE_HEADER + page_size, 0) != 
        (ssize_t)(PHYSICAL_PAGE_HEADER + page_size)) goto fail_free;
    
    /* Page 1: Header continuation (zeros) - uses PoolPage type per spec */
    memset(phys_buffer, 0, PHYSICAL_PAGE_HEADER + page_size);
    header = (PageHeader*)phys_buffer;
    header->pageType = PAGE_TYPE_POOL;
    header->flags = 0;
    header->generation = 1;
    header->mirrorPage = -1;
    header->checksum = vfs_crc32c(payload, page_size);
    
    if (pwrite(sb->fd, phys_buffer, PHYSICAL_PAGE_HEADER + page_size, 
               PHYSICAL_PAGE_HEADER + page_size) != 
        (ssize_t)(PHYSICAL_PAGE_HEADER + page_size)) goto fail_free;
    
    /* Page 2: First bitmap page - all bits set to 1 (free), except bits 0-3 (allocated) */
    memset(phys_buffer, 0, PHYSICAL_PAGE_HEADER + page_size);
    header = (PageHeader*)phys_buffer;
    header->pageType = PAGE_TYPE_BITMAP;
    header->flags = FLUSH_PRIORITY_BITMAP;
    header->generation = 1;
    header->mirrorPage = -1;
    
    /* All bits free (0xFF in payload) */
    memset(payload, 0xFF, page_size);
    /* Mark pages 0-3 as allocated (bits 0-3 = 0) */
    payload[0] = 0xF0;
    
    header->checksum = vfs_crc32c(payload, page_size);
    
    if (pwrite(sb->fd, phys_buffer, PHYSICAL_PAGE_HEADER + page_size, 
               2 * (PHYSICAL_PAGE_HEADER + page_size)) != 
        (ssize_t)(PHYSICAL_PAGE_HEADER + page_size)) goto fail_free;
    
    /* Page 3: Reserved for superblock */
    memset(phys_buffer, 0, PHYSICAL_PAGE_HEADER + page_size);
    header = (PageHeader*)phys_buffer;
    header->pageType = PAGE_TYPE_SUPERBLOCK;
    header->flags = FLUSH_PRIORITY_SUPERBLOCK;
    header->checksum = vfs_crc32c(payload, page_size);
    header->generation = 1;
    header->mirrorPage = -1;
    
    if (pwrite(sb->fd, phys_buffer, PHYSICAL_PAGE_HEADER + page_size, 
               3 * (PHYSICAL_PAGE_HEADER + page_size)) != 
        (ssize_t)(PHYSICAL_PAGE_HEADER + page_size)) goto fail_free;
    
    free(phys_buffer);
    
    sb->total_pages = 4;
    sb->segment_size = 1024;
    sb->bitmap_dir[0] = 2;
    sb->bitmap_count = 1;
    sb->initialized = 1;
    
    /* Reopen for read/write */
    close(sb->fd);
    sb->fd = open(path, O_RDWR);
    if (sb->fd < 0) goto fail;
    
    /* Allocate working buffer */
    sb->buffer = malloc(page_size);
    if (!sb->buffer) {
        close(sb->fd);
        sb->fd = -1;
        goto fail;
    }
    
    return vfs;
    
fail_free:
    free(phys_buffer);
fail_close:
    close(sb->fd);
    sb->fd = -1;
fail:
    free(vfs);
    return NULL;
}

/*
 * vfs_page_size - Get the configured page size
 */
uint64_t vfs_page_size(vfs_t* vfs) {
    if (!vfs) return 0;
    return vfs->backend.page_size;
}

/*
 * vfs_close - Close a VFS file
 */
void vfs_close(vfs_t* vfs) {
    if (!vfs) return;
    
    StorageBackend* sb = &vfs->backend;
    if (sb->fd >= 0) {
        close(sb->fd);
        sb->fd = -1;
    }
    if (sb->buffer) {
        free(sb->buffer);
        sb->buffer = NULL;
    }
    pthread_mutex_destroy(&sb->bitmap_lock);
    sb->initialized = 0;
    
    free(vfs);
}

/*
 * bitmap bit operations - find the bit for a logical page
 */
static inline int get_bitmap_index(int64_t page, int bits_per) {
    return (int)(page / bits_per);
}

static inline int get_bit_offset(int64_t page, int bits_per) {
    return (int)(page % bits_per);
}

/*
 * Read a bitmap page into memory
 */
static int read_bitmap_page(StorageBackend* sb, int bitmap_idx) {
    int64_t logical_page = sb->bitmap_dir[bitmap_idx];
    if (logical_page == 0) return -1;
    
    off_t offset = logical_page * (PHYSICAL_PAGE_HEADER + sb->page_size);
    ssize_t result = pread(sb->fd, sb->buffer, sb->page_size, offset + PHYSICAL_PAGE_HEADER);
    return (result == (ssize_t)sb->page_size) ? 0 : -1;
}

/*
 * Write a bitmap page to disk
 */
static int write_bitmap_page(StorageBackend* sb, int bitmap_idx) {
    int64_t logical_page = sb->bitmap_dir[bitmap_idx];
    if (logical_page == 0) return -1;
    
    /* Build physical page with header */
    uint8_t* phys = malloc(PHYSICAL_PAGE_HEADER + sb->page_size);
    if (!phys) return -1;
    
    off_t offset = logical_page * (PHYSICAL_PAGE_HEADER + sb->page_size);
    memcpy(phys + PHYSICAL_PAGE_HEADER, sb->buffer, sb->page_size);
    
    PageHeader* header = (PageHeader*)phys;
    header->pageType = PAGE_TYPE_BITMAP;
    header->flags = FLUSH_PRIORITY_BITMAP;
    header->checksum = vfs_crc32c(sb->buffer, sb->page_size);
    header->generation = 1;
    header->mirrorPage = -1;
    
    int result = pwrite(sb->fd, phys, PHYSICAL_PAGE_HEADER + sb->page_size, offset);
    free(phys);
    return (result == (ssize_t)(PHYSICAL_PAGE_HEADER + sb->page_size)) ? 0 : -1;
}

/*
 * storage_allocate - Allocate count contiguous free pages
 * Uses per-instance lock for bitmap page updates
 */
int64_t storage_allocate(StorageBackend* sb, uint64_t count) {
    if (count == 0 || count > ZONE_SIZE_PAGES) return -1;
    
    int bits_per = bits_per_page(sb->page_size);
    
    /* Zone-based allocation: divide pages into 1M-page zones */
    int64_t cursor = vfs_atomic_load_i64(&sb->zone_hint_cursor);
    int zone_count = (int)((sb->total_pages + ZONE_SIZE_PAGES - 1) / ZONE_SIZE_PAGES) + 1;
    
    /* Scan zones starting from cursor (round-robin) */
    int64_t start_page = -1;
    
    pthread_mutex_lock(&sb->bitmap_lock);
    
    for (int zone = 0; zone < zone_count * 2; zone++) {
        int64_t zone_idx = (cursor / ZONE_SIZE_PAGES + zone) % zone_count;
        int64_t zone_start = zone_idx * ZONE_SIZE_PAGES;
        int64_t zone_end = zone_start + ZONE_SIZE_PAGES;
        if (zone_end > (int64_t)sb->total_pages) zone_end = sb->total_pages;
        
        /* Get bitmap pages for this zone */
        int bmp_start = get_bitmap_index(zone_start, bits_per);
        int bmp_end = get_bitmap_index(zone_end, bits_per);
        
        for (int bmp = bmp_start; bmp <= bmp_end && bmp < sb->bitmap_count; bmp++) {
            if (read_bitmap_page(sb, bmp) < 0) continue;
            
            int scan_start = (bmp == bmp_start) ? 
                (int)(zone_start % bits_per) : 4;
            int scan_end = bits_per;
            
            int consecutive = 0;
            start_page = -1;
            
            for (int bit = scan_start; bit < scan_end; bit++) {
                if (vfs_atomic_bit_test(sb->buffer, bit)) {
                    if (start_page < 0) start_page = (int64_t)(bmp * (int64_t)bits_per + bit);
                    consecutive++;
                } else {
                    start_page = -1;
                    consecutive = 0;
                }
                
                if (consecutive >= (int)count) {
                    /* Mark pages as allocated */
                    for (size_t j = 0; j < count; j++) {
                        int64_t p = start_page + j;
                        int set_bit = get_bit_offset(p, bits_per);
                        sb->buffer[set_bit / 8] &= ~(1 << (set_bit % 8));
                    }
                    
                    write_bitmap_page(sb, bmp);
                    
                    /* Update zone hint cursor atomically */
                    vfs_atomic_store_i64(&sb->zone_hint_cursor, start_page + count);
                    
                    pthread_mutex_unlock(&sb->bitmap_lock);
                    return start_page;
                }
            }
        }
    }
    
    pthread_mutex_unlock(&sb->bitmap_lock);
    return -1;
}

/*
 * storage_acquire - Atomically check and set a specific bit
 * Returns 1 if the page was free, 0 if already allocated
 */
int storage_acquire(StorageBackend* sb, int64_t page) {
    if (page < 0) return 0;
    
    int bits_per = bits_per_page(sb->page_size);
    int bitmap_idx = get_bitmap_index(page, bits_per);
    int bit_offset = get_bit_offset(page, bits_per);
    
    if (bitmap_idx >= sb->bitmap_count) return 0;
    
    pthread_mutex_lock(&sb->bitmap_lock);
    
    /* Read the bitmap page */
    if (read_bitmap_page(sb, bitmap_idx) < 0) {
        pthread_mutex_unlock(&sb->bitmap_lock);
        return 0;
    }
    
    /* Atomically test and reset the bit */
    int was_free = vfs_atomic_bit_test_and_reset(sb->buffer, bit_offset);
    
    if (was_free) {
        write_bitmap_page(sb, bitmap_idx);
    }
    
    pthread_mutex_unlock(&sb->bitmap_lock);
    return was_free;
}

/*
 * storage_free - Clear the bit for a page (mark as free)
 */
void storage_free(StorageBackend* sb, int64_t page) {
    int bits_per = bits_per_page(sb->page_size);
    int bitmap_idx = get_bitmap_index(page, bits_per);
    int bit_offset = get_bit_offset(page, bits_per);
    
    if (bitmap_idx >= sb->bitmap_count) return;
    
    pthread_mutex_lock(&sb->bitmap_lock);
    
    if (read_bitmap_page(sb, bitmap_idx) < 0) {
        pthread_mutex_unlock(&sb->bitmap_lock);
        return;
    }
    
    /* Set the bit (mark as free) - atomic operation */
    vfs_atomic_bit_test_and_set(sb->buffer, bit_offset);
    write_bitmap_page(sb, bitmap_idx);
    
    pthread_mutex_unlock(&sb->bitmap_lock);
}

/*
 * vfs_allocate - Public wrapper for storage_allocate
 */
int64_t vfs_allocate(vfs_t* vfs, uint64_t count) {
    if (!vfs || !vfs->backend.initialized) return -1;
    return storage_allocate(&vfs->backend, count);
}

/*
 * vfs_acquire - Public wrapper for storage_acquire
 */
int vfs_acquire(vfs_t* vfs, int64_t page) {
    if (!vfs || !vfs->backend.initialized) return 0;
    return storage_acquire(&vfs->backend, page);
}

/*
 * vfs_free - Public wrapper for storage_free
 */
void vfs_free(vfs_t* vfs, int64_t page) {
    if (!vfs || !vfs->backend.initialized) return;
    storage_free(&vfs->backend, page);
}

