/*
 * iXSphereVFS — Internal structures and helpers.
 * Not part of the public API.
 */
#ifndef VFS_INTERNAL_H
#define VFS_INTERNAL_H

#include "ixsphere_vfs.h"
#include <stdatomic.h>

/* ── Page header ────────────────────────────────────────── */

typedef struct {
    uint16_t page_type;
    uint16_t flags;
    uint32_t checksum;
    uint32_t generation;
} vfs_page_header_t;

/* ── Superblock ─────────────────────────────────────────── */

#define VFS_SUPERBLOCK_ROOT_OFFSET    12
#define VFS_SUPERBLOCK_EPOCH_OFFSET   20
#define VFS_SUPERBLOCK_MAPPER_OFFSET  28
#define VFS_SUPERBLOCK_POOL_OFFSET    36
#define VFS_SUPERBLOCK_LOCK_OFFSET    44

/* ── Section page ───────────────────────────────────────── */

#define VFS_SECTION_NODE_OFFSET   12
#define VFS_SECTION_RESERVED      20
#define VFS_SECTION_SLOTS_OFFSET  24

/* ── Pool page ──────────────────────────────────────────── */

#define VFS_POOL_NEXT_OFFSET    12
#define VFS_POOL_STATE_OFFSET   20
#define VFS_POOL_RESERVED       24
#define VFS_POOL_SLOTS_OFFSET   28

/* ── Mapper page ────────────────────────────────────────── */

#define VFS_MAPPER_NEXT_OFFSET  12
#define VFS_MAPPER_COUNT_OFFSET 20
#define VFS_MAPPER_ENTRIES_OFF  24

#define VFS_MAPPER_ENTRY_SIZE   12

/* ── Directory/File node ────────────────────────────────── */

#define VFS_NODE_HEAD_OFFSET   12
#define VFS_NODE_COUNT_OFFSET  20
#define VFS_NODE_ENTRIES_OFF   28

/* ── Overflow page ──────────────────────────────────────── */

#define VFS_OVERFLOW_NEXT_OFFSET  12
#define VFS_OVERFLOW_COUNT_OFFSET 20
#define VFS_OVERFLOW_ENTRIES_OFF  24

/* ── Entry (directory/file node) ────────────────────────── */

#define VFS_ENTRY_EPOCH_OFF     0
#define VFS_ENTRY_TYPE_OFF      4
#define VFS_ENTRY_NAMELEN_OFF   6
#define VFS_ENTRY_CHILD_OFF     8
#define VFS_ENTRY_NEXT_OFF      16
#define VFS_ENTRY_NAME_OFF      24

/* ── CRC32C ─────────────────────────────────────────────── */

uint32_t vfs_crc32c(const uint8_t* data, size_t len);

/* ── Internal structures ────────────────────────────────── */

typedef struct {
    uint8_t*  pages;          /* dirty page cache */
    int       fd;             /* backing file descriptor */
    int64_t   total_pages;    /* pages in backing file */
    int       page_size;
} vfs_storage_t;

typedef struct {
    uint8_t*  bits;           /* bitmap data */
    int64_t   total_pages;
    int64_t   free_count;
    int       zone_count;
    /* zones omitted for initial scaffold */
} vfs_bitmap_t;

typedef struct {
    vfs_page_header_t header;
    int64_t  root_node_offset;
    int64_t  current_epoch;
    int64_t  epoch_mapper_page;
    _Atomic(int64_t) pool_list_head;
    _Atomic(int64_t) tree_lock_state;
} vfs_superblock_t;

struct vfs_t {
    char               path[1024];
    vfs_storage_t      storage;
    vfs_bitmap_t       bitmap;
    vfs_superblock_t   superblock;
    vfs_error_t        last_error;
};

#endif /* VFS_INTERNAL_H */
