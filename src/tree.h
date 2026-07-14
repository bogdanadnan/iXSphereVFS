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

/* SB_OFF_SEGMENT_SIZE is defined in storage.h as HDR_OFF_SEGMENT_SIZE
 * (offset 16 in the StorageBackend header page, not the superblock).
 * Don't redefine it here. */

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
 * Phase 26 W6: rewritten to use the shared chain-walk primitives
 * (walk_anchor_chain + walk_content_unit_chain) for the
 * segment + unit chain walks, and named helpers
 * (allocate_filecontent, link_fc_into_chain, link_pn_into_chain)
 * for the segment-growth and sorted-insertion logic.  The
 * function is no longer a "thin wrapper" — it has substantial
 * segment-growth and tcache logic.  Prefer vfs_chain_walk_extended
 * for new code that just needs to find a PageNode by id (no
 * growth, no tcache).
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
 * W6: uses vfs_chain_walk for the per-leaf chain walk + read-rule
 * (single source of truth for the read-rule; no inlined copies).
 * The radix-tree fast path walks the dircontentlink chain
 * manually (the dircontentlink layout differs from the Anchor
 * layout).  The fallback path uses walk_anchor_chain for the
 * DirSegment chain and walk_content_unit_chain for the SlotNode
 * chain.  vfs_chain_walk is called for the per-SlotNode
 * DirContent chain.  New code that doesn't need the by-name
 * match should use vfs_chain_walk_extended directly.
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

/* W6: unified chain-walk API.  Resolves root_vp + unit_id to a
 * ContentUnit (via the Anchor chain + ContentUnit chain) and then
 * walks the ContentUnit's leaf chain (VersionPage / DirContent /
 * FileSize) with the read-rule.  All 6 steps in one call.
 *
 * root_vp:     the FileNode or DirNode VP
 * unit_id:     the lookup key — page_index for files, slotId for dirs
 * query_epoch: the query epoch (caller should pre-resolve via
 *              mapper_table_resolve for snapshot remap)
 * out_leaf:    receives the visible leaf's slot (by-value copy-out)
 *
 * Returns WALK_FOUND if a visible leaf exists, WALK_NOT_FOUND if
 * the chain is exhausted without finding one, WALK_NEED_GROW if
 * the ContentUnit doesn't exist (file-write only; caller should
 * grow + retry).
 *
 * This is the 6-step walk from the spec collapsed into one entry
 * point.  tree_resolve_page / dirchain_find_child / vfs_rename are
 * thin wrappers over this.  Steps 1-3 (Anchor chain + ContentUnit
 * walk) are byte-identical between files (FileContent / PageNode)
 * and dirs (DirSegment / SlotNode) because both use the Anchor
 * layout (ANCHOR_OFF_HEADPTR, ANCHOR_OFF_SIBPTR, ANCHOR_OFF_ID).
 * Steps 4-5 (leaf walk + read-rule) are byte-identical to the
 * existing vfs_chain_walk — the extended form delegates to it
 * once the leaf chain head is resolved.  Step 6 (leaf
 * specialization — extract dataPage for VersionPage,
 * childPtr+namePtr for DirContent) is the caller's job.
 *
 * Lock discipline: when called on the write path, the caller must
 * hold vfs_lock(vfs, root_vp, epoch) per the W3 spec.  This
 * function does NOT acquire any locks; it reads the by-value
 * PoolSlots (pinPage=false).
 */
WalkResult vfs_chain_walk_extended(TreeContext* ctx,
                                   int64_t       root_vp,
                                   uint32_t      unit_id,
                                   int64_t       query_epoch,
                                   PoolSlot*     out_leaf);

/* W6: Anchor chain walk callback.  Receives the anchor's VP
 * and a by-value copy of the Anchor's 32 bytes.  Return 0 to
 * continue, non-zero to stop.  The callback can stash data in
 * *user; the user pointer is passed through unchanged.
 *
 * anchor_vp is the VirtualPtr of the current Anchor — useful for
 * the caller to record prev_vp for sorted insertion
 * (tree_resolve_page inserts PageNodes in sorted page_index order)
 * or to break the walk when a target VP is reached. */
typedef int (*anchor_walk_cb)(TreeContext* ctx,
                              int64_t anchor_vp,
                              const uint8_t* anchor_bytes,
                              void* user);

/* W6: walk a chain of Anchor nodes (FileContent for files,
 * DirSegment for dirs) via sibPtr.  Calls cb for each Anchor.
 * head_vp is the chain head (FileNode.headPtr or
 * DirNode.headPtr).  pinPage=false (read-only).  Returns the
 * number of Anchors visited.  Used by vfs_chain_walk_extended
 * and by callers that need to iterate (dirchain_find_child's
 * chain-walk fallback). */
int walk_anchor_chain(TreeContext* ctx, int64_t head_vp,
                      anchor_walk_cb cb, void* user);

/* W6: walk a chain of ContentUnit nodes (PageNode for files,
 * SlotNode for dirs) via sibPtr.  Calls cb for each ContentUnit.
 * unit_head is the segment's headPtr (FileContent.HEADPTR or
 * DirSegment.HEADPTR).  pinPage=false (read-only).  Returns
 * the number of ContentUnits visited.  Used by
 * vfs_chain_walk_extended and by callers that need to iterate
 * (dirchain_find_child's by-name search). */
int walk_content_unit_chain(TreeContext* ctx, int64_t unit_head,
                            anchor_walk_cb cb, void* user);

/* Walk a FileSize chain starting from sizePtr, apply the read-rule with
 * mapper remapping, and return the fileSize and modifiedAt at the visible
 * epoch.  If no entry applies, returns 0 for fileSize and 0 for modifiedAt.
 * read_epoch should be pre-resolved via mapper_table_resolve. */
void sizechain_get(TreeContext* ctx, int64_t sizePtr, int64_t read_epoch,
                   int64_t* out_fileSize, int64_t* out_modifiedAt);

#endif /* VFS_TREE_H */
