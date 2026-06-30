/*
 * include/vfs_internal.h — iXSphereVFS Internal Structures
 *
 * Declares structures shared across source files.
 */
#ifndef VFS_INTERNAL_H
#define VFS_INTERNAL_H

#include "ixsphere_vfs.h"
#include "platform.h"
#include <pthread.h>

/* ── Page Header ─────────────────────────────────────────── */
/* 16-byte header prepended to every logical page */

#pragma pack(push, 1)
typedef struct {
    uint16_t pageType;    /* Page type identifier */
    uint16_t flags;       /* Priority (0-3) in bits 0-1, other flags elsewhere */
    uint32_t checksum;    /* CRC32C of payload */
    uint32_t generation;  /* Generation number for mirroring */
    int32_t  mirrorPage;  /* Mirror sibling index, -1 if none */
} PageHeader;
#pragma pack(pop)

/* ── Page Types ──────────────────────────────────────────── */
/* Based on Phase 2 spec: bitmap uses 0x01, pool pages use 0x02, superblock uses 0x00 */
/* Additional types for future phases are defined here */

#define PAGE_TYPE_SUPERBLOCK 0x0000  /* Superblock page (reserved at page 3) */
#define PAGE_TYPE_BITMAP     0x0001  /* Bitmap page for free-page tracking */
#define PAGE_TYPE_POOL       0x0002  /* Pool page (also used for header continuation) */
#define PAGE_TYPE_DATA       0x0003  /* Data page (user file content) */

/* Zone size for zone-based allocation (1M pages per zone) */
#define ZONE_SIZE_PAGES 1048576

/* ── StorageBackend Struct ───────────────────────────────── */

typedef struct {
    int fd;                /* File descriptor */
    char path[256];      /* Path to backing file */
    uint64_t total_pages; /* Total logical pages */
    uint64_t page_size;   /* Page size */
    uint32_t segment_size; /* Pages per segment (for pool allocator) */
    int64_t bitmap_dir[2044]; /* Bitmap page indices (from header) */
    int bitmap_count;      /* Number of bitmap pages allocated */
    uint8_t* buffer;       /* Temporary buffer for reads/writes */
    int initialized;     /* Non-zero after successful open/create */
    
    /* Zone-based allocation hints */
    int64_t zone_hint_cursor; /* Next-fit cursor for allocation scanning (atomic) */
    pthread_mutex_t bitmap_lock; /* Per-instance lock for bitmap updates */
} StorageBackend;

/* ── VFS Instance ───────────────────────────────────────── */

struct vfs_t {
    StorageBackend backend;
};

/* ── XVFS Magic Numbers ─────────────────────────────────── */

/* XVFS magic: pageType=0x5658 ('VX'), flags=0x5346 ('SF') forms "XVFS" in little-endian */
#define XVFS_MAGIC_TYPE      0x5658
#define XVFS_MAGIC_FLAGS     0x5346

/* ── Flush Priorities (stored in flags field) ─────────────── */

#define FLUSH_PRIORITY_MASK  0x0003
#define FLUSH_PRIORITY_DATA  0
#define FLUSH_PRIORITY_POOL  1
#define FLUSH_PRIORITY_BITMAP 2
#define FLUSH_PRIORITY_SUPERBLOCK 3

/* ── Page allocation API (internal) ───────────────────────── */

int64_t storage_allocate(StorageBackend* sb, uint64_t count);  /* Allocate count contiguous pages */
int     storage_acquire(StorageBackend* sb, int64_t page);    /* Acquire specific page */
void    storage_free(StorageBackend* sb, int64_t page);         /* Free a page */

#endif /* VFS_INTERNAL_H */
