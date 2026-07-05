#ifndef IXSPHERE_VFS_H
#define IXSPHERE_VFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * Public iXSphere VFS API — single header for library consumers
 * ===========================================================================
 *
 * All functions return a negative vfs_error_t on error unless otherwise
 * noted.  The `epoch` parameter specifies which version of the data to
 * read or write (0 = root, odd = snapshot, even = writable base).
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * Error codes
 * --------------------------------------------------------------------------- */

typedef enum {
    VFS_OK            =  0,
    VFS_ERR_IO        = -1,
    VFS_ERR_NOTFOUND  = -2,
    VFS_ERR_EXISTS    = -3,
    VFS_ERR_NOTDIR    = -4,
    VFS_ERR_NOTEMPTY  = -5,
    VFS_ERR_CONFLICT  = -6,
    VFS_ERR_FULL      = -7,
    VFS_ERR_NOMEM     = -8,
    VFS_ERR_EPOCH     = -9,
} vfs_error_t;

/* Return a human-readable string for an error code. */
const char* vfs_error_string(vfs_error_t err);

/* ---------------------------------------------------------------------------
 * CRC32C
 * --------------------------------------------------------------------------- */

/* CRC32C (Castagnoli) checksum — used for data integrity verification. */
uint32_t vfs_crc32c(const uint8_t* data, size_t len);

/* ---------------------------------------------------------------------------
 * Opaque VFS handle
 * --------------------------------------------------------------------------- */

typedef struct vfs_t vfs_t;

/* ---------------------------------------------------------------------------
 * Directory entry type (for vfs_readdir)
 * --------------------------------------------------------------------------- */

typedef struct {
    int64_t nodeId;
    char    name[256];
    bool    isDir;
} vfs_dirent_t;

/* ---------------------------------------------------------------------------
 * Lifecycle (§12.1)
 * --------------------------------------------------------------------------- */

/* Open or create a VFS file.  path is the file path; page_size is typically
   8192.  Returns NULL on failure (check vfs_last_error for details). */
vfs_t*  vfs_mount(const char* path, int64_t page_size);

/* Close a VFS handle, flushing all pending writes to disk. */
void    vfs_unmount(vfs_t* vfs);

/* Flush all dirty pages to disk (fsync).  Returns VFS_OK on success. */
int     vfs_flush(vfs_t* vfs);

/* Return the error code from the last failed operation, or VFS_OK if none. */
vfs_error_t vfs_last_error(vfs_t* vfs);

/* ---------------------------------------------------------------------------
 * File Operations (§12.2)
 * --------------------------------------------------------------------------- */

/* Create a file under parent directory.  Returns the child's VirtualPtr on
   success (always > 0), or a negative error code on failure. */
int64_t vfs_create(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch);

/* Delete a file under parent directory. */
int     vfs_delete(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch);

/* Rename a file or directory. */
int     vfs_rename(vfs_t* vfs, int64_t src_parent, const char* src,
                   int64_t dst_parent, const char* dst, int64_t epoch);

/* Open a file by parent directory and name.  Returns the child's nodeId,
   or a negative error code (VFS_ERR_NOTFOUND) if not found. */
int64_t vfs_open(vfs_t* vfs, int64_t parent, const char* name,
                      int64_t epoch);

/* Read up to count bytes from file at offset.  Returns bytes actually read,
   or a negative error code. */
int     vfs_read(vfs_t* vfs, int64_t file, void* buf, int64_t offset,
                 int64_t count, int64_t epoch);

/* Write up to count bytes to file at offset.  Returns bytes actually written,
   or a negative error code. */
int     vfs_write(vfs_t* vfs, int64_t file, const void* data, int64_t offset,
                  int64_t count, int64_t epoch);

/* Query file size at a given epoch.  Returns size in bytes. */
int64_t vfs_file_size(vfs_t* vfs, int64_t file, int64_t epoch);

/* Query file modification time at a given epoch.  Returns Unix timestamp. */
int64_t vfs_file_mtime(vfs_t* vfs, int64_t file, int64_t epoch);

/* Query file creation time (immutable after creation).  Returns Unix timestamp. */
int64_t vfs_file_ctime(vfs_t* vfs, int64_t file);

/* ---------------------------------------------------------------------------
 * Directory Operations (§12.3)
 * --------------------------------------------------------------------------- */

/* Create a directory under parent.  Returns the child's VirtualPtr on success
   (always > 0), or a negative error code on failure. */
int64_t vfs_mkdir(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch);

/* Remove an empty directory. */
int     vfs_rmdir(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch);

/* List directory contents.  Returns the number of entries written (up to max),
   or a negative error code. */
int     vfs_readdir(vfs_t* vfs, int64_t dir, vfs_dirent_t* entries,
                    int max, int64_t epoch);

/* ---------------------------------------------------------------------------
 * Lock Operations (§9.3)
 * --------------------------------------------------------------------------- */

/* Acquire a per-file lock.  Returns VFS_OK on success. */
int     vfs_lock(vfs_t* vfs, int64_t file, int64_t epoch);

/* Release a per-file or global lock. */
int     vfs_unlock(vfs_t* vfs, int64_t file, int64_t epoch);

/* ---------------------------------------------------------------------------
 * Snapshot & Commit (§12.4)
 * --------------------------------------------------------------------------- */

/* Create a snapshot.  Returns the snapshot epoch (always odd),
   or a negative error code. */
int64_t vfs_snapshot(vfs_t* vfs);

/* Commit a snapshot, persisting its changes to the base epoch. */
int     vfs_commit(vfs_t* vfs, int64_t snapshot_epoch);

/* Soft-delete a snapshot, reverting its changes. */
int     vfs_delete_snapshot(vfs_t* vfs, int64_t snapshot_epoch);

/* ---------------------------------------------------------------------------
 * Garbage Collection (§12.5)
 * --------------------------------------------------------------------------- */

/* Shadow-compact the tree, removing soft-deleted epochs and reclaiming
   space.  Returns VFS_OK on success. */
int     vfs_gc(vfs_t* vfs);

#ifdef __cplusplus
}
#endif

#endif /* IXSPHERE_VFS_H */
