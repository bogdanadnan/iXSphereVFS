#ifndef IXSPHERE_TREE_API_H
#define IXSPHERE_TREE_API_H

#include "ixsphere_vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Directory entry type for vfs_readdir
 * --------------------------------------------------------------------------- */

typedef struct {
    int64_t nodeId;
    char    name[256];
    bool    isDir;
} vfs_dirent_t;

/* ---------------------------------------------------------------------------
 * File Operations (§12.2)
 * --------------------------------------------------------------------------- */

/* Create a file under parent directory. Returns new nodeId, or -1 on error. */
int vfs_create(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch);

/* Delete a file under parent directory. */
int vfs_delete(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch);

/* Rename a file or directory. */
int vfs_rename(vfs_t* vfs, int64_t src_parent, const char* src,
               int64_t dst_parent, const char* dst, int64_t epoch);

/* Open a file by parent directory and name. Returns nodeId, or -1 if not found. */
int64_t vfs_open_file(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch);

/* Read up to count bytes from file at offset. Returns bytes read, or -1 on error. */
int vfs_read(vfs_t* vfs, int64_t file, void* buf, int64_t offset,
             int64_t count, int64_t epoch);

/* Write up to count bytes to file at offset. Returns bytes written, or -1 on error. */
int vfs_write(vfs_t* vfs, int64_t file, const void* data, int64_t offset,
              int64_t count, int64_t epoch);

/* Query file size at a given epoch. Returns size in bytes. */
int64_t vfs_file_size(vfs_t* vfs, int64_t file, int64_t epoch);

/* Query file modification time at a given epoch. Returns Unix timestamp. */
int64_t vfs_file_mtime(vfs_t* vfs, int64_t file, int64_t epoch);

/* Query file creation time (immutable). Returns Unix timestamp. */
int64_t vfs_file_ctime(vfs_t* vfs, int64_t file);

/* ---------------------------------------------------------------------------
 * Directory Operations (§12.3)
 * --------------------------------------------------------------------------- */

/* Create a directory under parent. */
int vfs_mkdir(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch);

/* Remove an empty directory. */
int vfs_rmdir(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch);

/* List directory contents. Returns number of entries written (up to max). */
int vfs_readdir(vfs_t* vfs, int64_t dir, vfs_dirent_t* entries,
                int max, int64_t epoch);

/* ---------------------------------------------------------------------------
 * Lock Operations (§9.3)
 * --------------------------------------------------------------------------- */

/* Acquire a per-file lock. epoch=0 acquires the global lock for the file. */
int vfs_lock(vfs_t* vfs, int64_t file, int64_t epoch);

/* Release a per-file or global lock. */
int vfs_unlock(vfs_t* vfs, int64_t file, int64_t epoch);

/* ---------------------------------------------------------------------------
 * Snapshot & Commit (§12.4)
 * --------------------------------------------------------------------------- */

/* Create a snapshot. Returns the snapshot epoch (always odd), or -1 on error. */
int64_t vfs_snapshot(vfs_t* vfs);

/* Commit a snapshot. */
int vfs_commit(vfs_t* vfs, int64_t snapshot_epoch);

/* Soft-delete a snapshot. */
int vfs_delete_snapshot(vfs_t* vfs, int64_t snapshot_epoch);

/* ---------------------------------------------------------------------------
 * Manual GC (§12.5)
 * --------------------------------------------------------------------------- */

/* Shadow-compact the tree, removing soft-deleted epochs. */
int vfs_gc(vfs_t* vfs);

/* ---------------------------------------------------------------------------
 * Flush (§12.1)
 * --------------------------------------------------------------------------- */

/* Flush all dirty pages to disk. */
int vfs_flush(vfs_t* vfs);

#ifdef __cplusplus
}
#endif

#endif /* IXSPHERE_TREE_API_H */
