/*
 * iXSphereVFS — Spec 30c Epoch-Versioned Unified Tree
 *
 * Public API. Include this header to use the VFS.
 */
#ifndef IXSPHERE_VFS_H
#define IXSPHERE_VFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Page constants ─────────────────────────────────────── */

#define VFS_PAGE_SIZE       8192
#define VFS_PAGE_HEADER_SIZE 12
#define VFS_SECTION_SLOTS   1021
#define VFS_POOL_SLOTS       340
#define VFS_SECTION_PAGES    1021   /* 8KB pages per section, ~8.1MB */

/* ── Page types ─────────────────────────────────────────── */

typedef enum {
    VFS_PAGE_SUPERBLOCK = 0x00,
    VFS_PAGE_BITMAP     = 0x01,
    VFS_PAGE_TREENODE   = 0x02,
    VFS_PAGE_POOL       = 0x03,
    VFS_PAGE_MAPPER     = 0x04,
    VFS_PAGE_DATA       = 0x05,
} vfs_page_type_t;

/* ── Entry types (directory/file nodes) ─────────────────── */

typedef enum {
    VFS_ENTRY_DIRCHILD  = 0x01,
    VFS_ENTRY_FILECHILD = 0x02,
    VFS_ENTRY_TOMBSTONE = 0x03,
    VFS_ENTRY_SECTION   = 0x04,
    VFS_ENTRY_FILESIZE  = 0x05,
} vfs_entry_type_t;

/* ── Virtual pointer ────────────────────────────────────── */

typedef uint64_t vfs_virtual_ptr_t;

#define VFS_VPTR_NULL       ((vfs_virtual_ptr_t)0)
#define VFS_VPTR_PAGE(vp)   ((int64_t)((vp) >> 10))
#define VFS_VPTR_SLOT(vp)   ((int)((vp) & 0x3FF))
#define VFS_VPTR_MAKE(p, s) (((vfs_virtual_ptr_t)(p) << 10) | ((s) & 0x3FF))

/* ── Version node ───────────────────────────────────────── */

typedef struct {
    uint32_t epoch;
    uint32_t data_crc32c;
    int64_t  physical_page;
    vfs_virtual_ptr_t next;
} vfs_version_node_t;

/* ── Error codes ────────────────────────────────────────── */

typedef enum {
    VFS_OK = 0,
    VFS_ERR_NOMEM = -1,
    VFS_ERR_NOTFOUND = -2,
    VFS_ERR_CONFLICT = -3,
    VFS_ERR_IO = -4,
    VFS_ERR_CORRUPT = -5,
} vfs_error_t;

/* ── Handle ─────────────────────────────────────────────── */

typedef struct vfs_t vfs_t;

/* ── API ────────────────────────────────────────────────── */

/* Create/open a VFS instance backed by the given file path. */
vfs_t*      vfs_open(const char* path);

/* Close and destroy a VFS instance. */
void        vfs_close(vfs_t* vfs);

/* ── Epoch operations ───────────────────────────────────── */

int64_t     vfs_epoch_current(vfs_t* vfs);
int64_t     vfs_snapshot_take(vfs_t* vfs);
int         vfs_snapshot_commit(vfs_t* vfs, int64_t snapshot_epoch);
int         vfs_snapshot_delete(vfs_t* vfs, int64_t snapshot_epoch);

/* ── File I/O ───────────────────────────────────────────── */

/* Read from a file at the given epoch. */
int         vfs_read(vfs_t* vfs, int64_t file_node, int64_t offset,
                     void* buf, size_t count, int64_t epoch);

/* Write to a file at the given epoch. */
int         vfs_write(vfs_t* vfs, int64_t file_node, int64_t offset,
                      const void* data, size_t count, int64_t epoch);

/* ── Directory operations ───────────────────────────────── */

int64_t     vfs_file_create(vfs_t* vfs, int64_t parent_node,
                            const char* name, int64_t epoch);
int64_t     vfs_dir_create(vfs_t* vfs, int64_t parent_node,
                           const char* name, int64_t epoch);
int         vfs_delete(vfs_t* vfs, int64_t parent_node,
                       const char* name, int64_t epoch);
int         vfs_rename(vfs_t* vfs, int64_t src_parent, const char* src_name,
                       int64_t dst_parent, const char* dst_name, int64_t epoch);

/* ── Garbage collection ─────────────────────────────────── */

int         vfs_gc(vfs_t* vfs);

/* ── Error ──────────────────────────────────────────────── */

vfs_error_t vfs_last_error(vfs_t* vfs);
const char* vfs_error_string(vfs_error_t err);

#ifdef __cplusplus
}
#endif
#endif /* IXSPHERE_VFS_H */
