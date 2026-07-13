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

#ifndef NDEBUG
/* Debug counter: number of tcache rebuilds from sparse chain. */
int tree_resolve_page_cache_builds_get(void);
#endif

#ifdef VFS_NAME_HASH_TESTING
/* Debug counter: number of hash-based name rejections in DirContent walks. */
void dirchain_test_reset_hash_rejects(void);
int dirchain_test_get_hash_rejects(void);
#endif

/* ---------------------------------------------------------------------------
 * Page Resolution (§5, Shared Utility)
 * --------------------------------------------------------------------------- */

/* Resolve a logical page within a file to its PageNode.
 *
 * Walks the FileContent chain to find the segment containing this page.
 * Creates missing FileContent + PageNode entries on file growth.
 * Builds the in-memory VirtualPtr array on first access to a segment.
 *
 * Phase 25: by-value copy-out.  Writes the 32-byte PageNode slot into
 * *out.  Returns 0 on success, -1 on error or page-not-found.  The
 * caller's PoolSlot is a stack-local copy independent of the cache,
 * so a later cache eviction cannot invalidate it (closes the C1 hazard).
 *
 * file_vp  — VirtualPtr to the FileNode
 * logical_page — page index within the file (0-based)
 * epoch    — write epoch (used for segment growth decisions)
 * is_write — true if caller intends to write.  When true, the caller
 *            MUST hold vfs_lock(vfs, file_vp, epoch) — the function
 *            will additionally take per-ContentUnit and per-PageNode
 *            locks as needed (per Phase 26 / W3 lock discipline) and
 *            use simple store instead of CAS.
 * out      — caller-provided PoolSlot to receive the PageNode bytes
 */
int tree_resolve_page(vfs_t* vfs, int64_t file_vp,
                      int64_t logical_page, int64_t epoch, bool is_write,
                      PoolSlot* out);

/* Phase 25: TEST-ONLY compatibility shim.  Returns a raw pointer to
   a per-thread static PoolSlot's bytes.  NOT thread-safe; tests are
   single-threaded.  Page is dirty-marked (pinned) when is_write=true
   so subsequent writes persist; caller must call
   tree_resolve_page_compat_release() after writing.  See pool.h for
   the rationale. */
extern uint8_t* tree_resolve_page_compat(vfs_t* vfs, int64_t file_vp,
                                          int64_t logical_page, int64_t epoch,
                                          bool is_write);
extern void tree_resolve_page_compat_release(vfs_t* vfs);

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

/* --- Directory radix tree (Phase 18) --- */
int64_t dircontentindex_lookup(Pool* pool, int64_t indexRoot,
                               uint64_t nameHash, int64_t page_size);
int dircontentindex_insert(Pool* pool, int64_t* indexRoot, uint64_t nameHash,
                          int64_t dirContentVP, int64_t page_size);

/* Walk the tree to the leaf for nameHash, scan the leaf's DirContentLink
 * list, and zero the dirContentVP of any link matching dirContentVP.  This
 * turns the link into a tree-tombstone — subsequent lookups skip it.
 *
 * Returns 0 if a matching link was found and zeroed, -1 otherwise.
 * Note: this does NOT free the DirContentLink slot or alter tree structure;
 * the slot becomes a permanent one-link tree-tombstone (32 bytes leaked). */
int dircontentindex_remove(Pool* pool, int64_t indexRoot, uint64_t nameHash,
                           int64_t dirContentVP, int64_t page_size);

/* Walk a directory's DirContent chain and list non-tombstone entries at a
 * given epoch (read-rule dedup by childNodeId).  Fills the entries[] array
 * Walks a directory's DirContent chain ONCE and produces a
 * heap-allocated vfs_dirent_t[] sized to the actual entry count.
 * No cap, no caller-buffer guess.  Caller must free the returned
 * array with free() (or via vfs_free_dirents() in vfs.h).  Returns
 * VFS_OK on success; on error, *out_entries and *out_count are set
 * to NULL/0.
 *
 * Phase 24: this is the only directory-listing API.  The old
 * caller-buffer dirchain_list is gone.
 *
 * Used by FUSE-side caching where the directory contents must be
 * retrieved in full to support cursor-based readdir with offset. */
int dirchain_list(TreeContext* ctx, int64_t dir_vp, int64_t epoch,
                  vfs_dirent_t** out_entries, int* out_count);

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

/* ---------------------------------------------------------------------------
 * Phase 26 / W2: vfs_chain_walk — unified per-leaf chain walker.
 *
 * Walks a per-leaf chain (VersionPage, DirContent, or FileSize — all
 * share the unified 32-byte layout: epoch at offset 0, nextPtr at 24)
 * applying the read-rule with mapper remapping.  Steps 4-5 of the
 * 6-step walk in §4.3 of the spec.  Steps 1-3 (find the right
 * ContentUnit) are done by the caller (tree_resolve_page /
 * dirchain_find_child); step 6 (leaf specialization) is also done
 * by the caller.
 *
 * The visible entry's slot is copied into *out_leaf.  Returns:
 *   WALK_FOUND         — visible entry returned in *out_leaf
 *   WALK_NOT_FOUND     — chain exhausted without finding an applicable
 *                         entry (caller should treat as "no data" /
 *                         "not found")
 *   WALK_NEED_GROW     — chain head was 0 (caller should grow and retry;
 *                         only valid for file write path)
 *
 * read_epoch is the already-resolved query epoch (caller should call
 * mapper_table_resolve before passing it).
 * --------------------------------------------------------------------------- */

typedef enum {
    WALK_FOUND = 0,
    WALK_NOT_FOUND = 1,
    WALK_NEED_GROW = 2,
} WalkResult;

WalkResult vfs_chain_walk(TreeContext* ctx,
                          int64_t       chain_head,
                          int64_t       read_epoch,
                          PoolSlot*     out_leaf);

/* Walk a FileSize chain starting from sizePtr, apply the read-rule with
 * mapper remapping, and return the fileSize and modifiedAt at the visible
 * epoch.  If no entry applies, returns 0 for fileSize and 0 for modifiedAt.
 * read_epoch should be pre-resolved via mapper_table_resolve. */
void sizechain_get(TreeContext* ctx, int64_t sizePtr, int64_t read_epoch,
                   int64_t* out_fileSize, int64_t* out_modifiedAt);

#endif /* VFS_TREE_H */
