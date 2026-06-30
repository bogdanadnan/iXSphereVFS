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
/* flags: bits 0-1 = flush priority; on header page, stores 0x56585346 magic */

#pragma pack(push, 1)
typedef struct {
    uint32_t flags;       /* Flush priority in bits 0-1; magic 0x56585346 on header page */
    uint32_t checksum;    /* CRC32C of payload */
    uint32_t generation;  /* Generation number for mirroring */
    int32_t  mirrorPage;  /* Mirror sibling index, -1 if none */
} PageHeader;
#pragma pack(pop)

/* ── XVFS Magic Value ─────────────────────────────────────────── */
/* Magic stored directly in flags field (4 bytes) */
#define VFS_MAGIC    0x56585346  /* "XVFS" in little-endian */

/* ── Zone size for zone-based allocation (1M pages per zone) ────── */
#define ZONE_SIZE_PAGES 1048576

/* ── StorageBackend Struct ─────────────────────────────────────────── */

#define MAX_ZONES 256  /* Max zones for per-zone cursors */

typedef struct {
    int fd;                /* File descriptor */
    char path[256];      /* Path to backing file */
    uint64_t total_pages; /* Total logical pages */
    uint64_t page_size;   /* Page size */
    uint32_t segment_size; /* Pages per segment (for pool allocator) */
    int64_t bitmap_dir[2044]; /* Bitmap page indices (from header) - in-memory copy */
    int bitmap_count;      /* Number of bitmap pages allocated */
    uint8_t* buffer;       /* Temporary buffer for reads/writes */
    int initialized;     /* Non-zero after successful open/create */
    
    /* Per-zone hint cursors - indexed by zone number (§3.3) */
    int64_t zone_cursors[MAX_ZONES];
    
    /* Bitmap lock - protects bitmap page read/write operations */
    pthread_mutex_t bitmap_lock;
} StorageBackend;

/* ── VFS Instance ───────────────────────────────────────── */

struct vfs_t {
    StorageBackend backend;
};

/* ── Flush Priorities (stored in flags field) ─────────────── */

#define FLUSH_PRIORITY_MASK  0x00000003
#define FLUSH_PRIORITY_DATA  0
#define FLUSH_PRIORITY_POOL  1
#define FLUSH_PRIORITY_BITMAP 2
#define FLUSH_PRIORITY_SUPERBLOCK 3

/* ── Page allocation API (internal) ───────────────────────── */

int64_t storage_allocate(StorageBackend* sb, uint64_t count);  /* Allocate count contiguous pages */
int     storage_acquire(StorageBackend* sb, int64_t page);    /* Acquire specific page */
void    storage_free(StorageBackend* sb, int64_t page);         /* Free a page */

/* ── Page I/O API (internal) ─────────────────────────────────── */

uint8_t* storage_read(StorageBackend* sb, int64_t logicalPage);
void     storage_write(StorageBackend* sb, int64_t logicalPage, uint8_t* payload, uint8_t priority);
void     storage_flush(StorageBackend* sb, int64_t logicalPage);

#endif /* VFS_INTERNAL_H */
