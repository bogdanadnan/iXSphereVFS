#ifndef VFS_TREE_H
#define VFS_TREE_H

#include "ixsphere/vfs_internal.h"
#include "nodes.h"
#include "epoch.h"

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
#define SB_OFF_SEGMENT_SIZE     16  /* in StorageBackend header page (page 0) */

#define SUPERBLOCK_PAGE 1

/* ---------------------------------------------------------------------------
 * Bootstrap & Init (§5, Workload 5.1)
 * --------------------------------------------------------------------------- */

/* Initialize the superblock on a fresh file: create root DirNode,
   write superblock fields.  Called by vfs_open when backing file is new. */
int tree_bootstrap_superblock(TreeContext* ctx);

/* Re-initialize TreeContext from an existing file: read superblock page,
   populate ctx fields, verify root DirNode.  Called by vfs_open on reopen. */
int tree_init(TreeContext* ctx);

/* ---------------------------------------------------------------------------
 * Superblock I/O helpers
 * --------------------------------------------------------------------------- */

/* Read superblock payload into a page_size-byte buffer, parse into ctx. */
int tree_superblock_read(TreeContext* ctx);

/* Write ctx fields into a page_size-byte buffer and flush to disk. */
int tree_superblock_write(TreeContext* ctx);

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
                           int64_t logical_page, int64_t epoch);

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
 *
 * Returns VFS_OK if found, VFS_ERR_NOTFOUND if no matching entry exists. */
int dirchain_find_child(TreeContext* ctx, int64_t dir_vp, const char* name,
                        int64_t epoch, int64_t* out_childPtr,
                        uint32_t* out_nodeId);

/* Walk a directory's DirContent chain and list non-tombstone entries at a
 * given epoch (read-rule dedup by childNodeId).  Fills the entries[] array
 * (up to max entries) with childNodeId, name, and isDir.  Returns the number
 * of entries written, or a negative error code. */
int dirchain_list(TreeContext* ctx, int64_t dir_vp, int64_t epoch,
                  vfs_dirent_t* entries, int max);

#endif /* VFS_TREE_H */
