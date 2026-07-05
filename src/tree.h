#ifndef VFS_TREE_H
#define VFS_TREE_H

#include "ixsphere/vfs_internal.h"
#include "nodes.h"
#include "epoch.h"
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * Superblock payload offsets (SPEC §4.1)
 *
 * The superblock lives at logical page 1.  All multi-byte fields are
 * little-endian (use vfs_rdX/vfs_wrX helpers from page_buf.h).
 * --------------------------------------------------------------------------- */

#define SB_OFF_ROOT_OFFSET      0   /* int64_t — VirtualPtr to root DirNode */
#define SB_OFF_CURRENT_EPOCH    8   /* int64_t — latest epoch counter */
#define SB_OFF_EPOCH_MAPPER_PTR 16  /* int64_t — VirtualPtr to first MapperEntry */
#define SB_OFF_POOL_LIST_HEAD   24  /* int64_t — logical page of first pool page */
#define SB_OFF_TREE_LOCK_STATE  32  /* int64_t — §9.6 bit layout */
#define SB_OFF_NEXT_NODE_ID     40  /* uint32_t */
#define SB_OFF_TOUCHED_FILES_PTR 48 /* int64_t — VirtualPtr */
#define SB_OFF_FORMAT_VERSION   56 /* uint32_t — format version number */
/* Bytes 60-63 are reserved for future 8-byte alignment. */
#define SB_OFF_SEGMENT_SIZE     16  /* in StorageBackend header page (page 0) */

#define SUPERBLOCK_PAGE 1

/* ---------------------------------------------------------------------------
 * Bootstrap & Init (§5, Workload 5.1)
 * --------------------------------------------------------------------------- */

/* Initialize the superblock on a fresh file: create root DirNode,
   write superblock fields.  Called by vfs_mount when backing file is new. */
int tree_bootstrap_superblock(TreeContext* ctx);

/* Re-initialize TreeContext from an existing file: read superblock page,
   populate ctx fields, verify root DirNode.  Called by vfs_mount on reopen. */
int tree_init(TreeContext* ctx);

/* ---------------------------------------------------------------------------
 * Superblock I/O helpers
 * --------------------------------------------------------------------------- */

/* Read superblock payload into a page_size-byte buffer, parse into ctx. */
int tree_superblock_read(TreeContext* ctx);

/* Write ctx fields into a page_size-byte buffer and flush to disk. */
int tree_superblock_write(TreeContext* ctx);

/* ---------------------------------------------------------------------------
 * Format migration
 * --------------------------------------------------------------------------- */

/* Migrate a v1 file (PageNodes without pageIndex) to v2 (with pageIndex).
 * Walks all FileContent→PageNode chains and writes the correct pageIndex
 * into each PageNode.  Returns VFS_OK on success, or a negative error code. */
int tree_migrate_v1_to_v2(TreeContext* ctx);

/* Recursively walk a directory tree, writing pageIndex into all PageNodes.
 * Called by tree_migrate_v1_to_v2 starting from the root directory. */
int tree_migrate_walk_dir(TreeContext* ctx, int64_t dir_vp);

/* Walk a single file's FileContent→PageNode chain, writing sequential
 * pageIndex values (0, 1, 2, ...) into each PageNode at offset 16. */
int tree_migrate_walk_file(TreeContext* ctx, int64_t file_vp);

/* ---------------------------------------------------------------------------
 * Page Resolution (§5, Shared Utility)
 * --------------------------------------------------------------------------- */

/* Resolve a logical page within a file to its PageNode.
 *
 * Walks the FileContent chain to find the segment containing this page.
 * Creates missing FileContent + PageNode entries on file growth.
 * Builds the in-memory VirtualPtr array on first access to a segment.
 *
 * Returns a pointer to the PageNode slot (via pool_resolve), or NULL on error.
 *
 * file_vp  — VirtualPtr to the FileNode
 * logical_page — page index within the file (0-based)
 * epoch    — write epoch (used for segment growth decisions)
 */
uint8_t* tree_resolve_page(TreeContext* ctx, int64_t file_vp,
                           int64_t logical_page, int64_t epoch, bool is_write);

/* ---------------------------------------------------------------------------
 * Context accessors (inline helpers)
 * --------------------------------------------------------------------------- */

VFS_INLINE int64_t tree_root(TreeContext* ctx) {
    return ctx->rootNodeOffset;
}

VFS_INLINE int64_t tree_current_epoch(TreeContext* ctx) {
    return ctx->currentEpoch;
}

VFS_INLINE int64_t tree_next_node_id(TreeContext* ctx) {
    return (int64_t)ctx->nextNodeId;
}

VFS_INLINE uint32_t tree_segment_size(TreeContext* ctx) {
    return ctx->segment_size;
}

/* ---------------------------------------------------------------------------
 * Directory chain lookup
 * --------------------------------------------------------------------------- */

/* Walk a directory's DirContent chain to find an entry by name at a given
 * epoch (applying the read-rule and mapper remapping).
 *
 * ctx         — VFS tree context
 * dir_vp      — VirtualPtr of the DirNode to search
 * name        — entry name to match
 * epoch       — query epoch (mapper_resolve is applied internally)
 * out_childPtr — set to the child's VirtualPtr on match, unchanged otherwise
 * out_nodeId  — set to the child's nodeId on match, unchanged otherwise
 * out_epoch   — set to the raw stored epoch of the matching DirContent entry,
 *               or unchanged if NULL.
 *
 * Returns VFS_OK if found, VFS_ERR_NOTFOUND if no matching entry exists. */
int dirchain_find_child(TreeContext* ctx, int64_t dir_vp, const char* name,
                        int64_t epoch, int64_t* out_childPtr,
                        uint32_t* out_nodeId, uint32_t* out_epoch);

/* Walk a directory's DirContent chain and list non-tombstone entries at a
 * given epoch (read-rule dedup by childNodeId).  Fills the entries[] array
 * (up to max entries) with childNodeId, name, and isDir.  Returns the number
 * of entries written, or a negative error code. */
int dirchain_list(TreeContext* ctx, int64_t dir_vp, int64_t epoch,
                  vfs_dirent_t* entries, int max);

/* ---------------------------------------------------------------------------
 * Version chain lookup
 * --------------------------------------------------------------------------- */

/* Walk a VersionPage chain starting from versionRootPtr and apply the
 * read-rule with mapper traversal remapping to find the visible data page.
 *
 * Returns the logical data page index (for use with storage_read), or -1
 * if no VersionPage applies at the given epoch (page never written).
 *
 * read_epoch is the already-resolved query epoch (caller should call
 * mapper_table_resolve before passing it). */
int64_t verchain_get(TreeContext* ctx, int64_t versionRootPtr,
                     int64_t read_epoch);

/* Walk a FileSize chain starting from sizePtr, apply the read-rule with
 * mapper remapping, and return the fileSize and modifiedAt at the visible
 * epoch.  If no entry applies, returns 0 for fileSize and 0 for modifiedAt.
 * read_epoch should be pre-resolved via mapper_table_resolve. */
void sizechain_get(TreeContext* ctx, int64_t sizePtr, int64_t read_epoch,
                   int64_t* out_fileSize, int64_t* out_modifiedAt);

#endif /* VFS_TREE_H */
