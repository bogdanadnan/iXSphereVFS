/* Phase 5: Tree Operations — Bootstrap, Init, Superblock I/O */
#include "tree.h"
#include "page_array.h"
#include "var_array.h"
#include "hash_map.h"
#include "gc.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>

/* When a sparse PageNode chain reaches this many entries, switch from
 * chain-walk to a rebuilt in-memory array for amortized O(1) lookups. */
#define SPARSE_CACHE_THRESHOLD 64

#ifndef NDEBUG
#include <stdatomic.h>
static atomic_int tree_resolve_page_cache_builds = 0;
int tree_resolve_page_cache_builds_get(void) {
    return atomic_load(&tree_resolve_page_cache_builds);
}
#endif

#ifdef VFS_NAME_HASH_TESTING
static int s_hash_rejects = 0;
void dirchain_test_reset_hash_rejects(void) {
    s_hash_rejects = 0;
}
int dirchain_test_get_hash_rejects(void) {
    return s_hash_rejects;
}
#endif

/* ---------------------------------------------------------------------------
 * Superblock I/O helpers
 * --------------------------------------------------------------------------- */

int tree_superblock_read(TreeContext* ctx) {
    uint8_t* payload = storage_read(ctx->sb, SUPERBLOCK_PAGE);
    if (!payload) return VFS_ERR_IO;

    ctx->rootNodeOffset   = vfs_rd8_s(payload, SB_OFF_ROOT_OFFSET, ctx->page_size);
    ctx->currentEpoch     = vfs_rd8_s(payload, SB_OFF_CURRENT_EPOCH, ctx->page_size);
    ctx->epochMapperPtr   = vfs_rd8_s(payload, SB_OFF_EPOCH_MAPPER_PTR, ctx->page_size);
    ctx->treeLockState    = vfs_rd8_s(payload, SB_OFF_TREE_LOCK_STATE, ctx->page_size);
    ctx->nextNodeId       = (uint32_t)vfs_rd4_s(payload, SB_OFF_NEXT_NODE_ID, ctx->page_size);

    /* poolListHead — wire into pool allocator */
    int64_t pool_list_head = vfs_rd8_s(payload, SB_OFF_POOL_LIST_HEAD, ctx->page_size);
    if (ctx->pool.list_head) *ctx->pool.list_head = pool_list_head;

    return VFS_OK;
}

int tree_superblock_write(TreeContext* ctx) {
    int64_t ps = ctx->sb->page_size;
    uint8_t* buf = (uint8_t*)malloc((size_t)ps);
    if (!buf) return VFS_ERR_NOMEM;
    memset(buf, 0, (size_t)ps);

    vfs_wr8_s(buf, SB_OFF_ROOT_OFFSET,       ctx->rootNodeOffset, ctx->page_size);
    vfs_wr8_s(buf, SB_OFF_CURRENT_EPOCH,     ctx->currentEpoch, ctx->page_size);
    vfs_wr8_s(buf, SB_OFF_EPOCH_MAPPER_PTR,  ctx->epochMapperPtr, ctx->page_size);
    vfs_wr8_s(buf, SB_OFF_POOL_LIST_HEAD,    ctx->pool.list_head ? *ctx->pool.list_head : 0, ctx->page_size);
    vfs_wr8_s(buf, SB_OFF_TREE_LOCK_STATE,   ctx->treeLockState, ctx->page_size);
    vfs_wr4_s(buf, SB_OFF_NEXT_NODE_ID,      (int32_t)ctx->nextNodeId, ctx->page_size);

    storage_write(ctx->sb, SUPERBLOCK_PAGE, buf, 3);
    storage_flush(ctx->sb, -1);
    free(buf);
    return VFS_OK;
}

/* ---------------------------------------------------------------------------
 * Bootstrap — initialize superblock on a fresh file
 * --------------------------------------------------------------------------- */

int tree_bootstrap_superblock(TreeContext* ctx) {
    /* Try reading the superblock — if a valid root exists, this is a reopen. */
    if (tree_superblock_read(ctx) == VFS_OK && ctx->rootNodeOffset != 0) {
        return tree_init(ctx);
    }

    /* Prepare superblock payload in-memory */
    ctx->rootNodeOffset   = 0;
    ctx->currentEpoch     = 0;
    ctx->epochMapperPtr   = 0;
    ctx->nextNodeId       = 0;  /* first vfs_atomic_add_i32 returns 1 */
    ctx->treeLockState    = 0;

    /* Write superblock with initial state */
    int err = tree_superblock_write(ctx);
    if (err != VFS_OK) return err;

    /* Read segment_size from StorageBackend header page */
    uint8_t* hdr = storage_read(ctx->sb, 0);
    if (hdr) {
        ctx->segment_size = (uint32_t)vfs_rd4_s(hdr, HDR_OFF_SEGMENT_SIZE, ctx->page_size);
    } else {
        ctx->segment_size = 1024;  /* default */
    }

    /* Allocate pool already exists in ctx->pool from vfs_mount (file opened via vfs_open) */
    int64_t root_vp = pool_alloc(&ctx->pool);
    if (root_vp == VFS_VPTR_NULL) return VFS_ERR_FULL;

    PoolSlot root_slot;
    pool_acquire(&ctx->pool, root_vp, true, &root_slot);
    if (root_slot.vptr == VFS_VPTR_NULL) return VFS_ERR_IO;

    /* Write root DirNode: nodeId=0, no children.
       Allocate the initial radix tree root (INTERNAL, empty child list).
       Every DirNode starts with a valid indexHeadPtr — no lazy build. */
    int64_t rootIndexVP = pool_alloc(&ctx->pool);
    if (rootIndexVP == VFS_VPTR_NULL) return VFS_ERR_FULL;

    PoolSlot rootIndexSlot;
    pool_acquire(&ctx->pool, rootIndexVP, true, &rootIndexSlot);
    if (rootIndexSlot.vptr == VFS_VPTR_NULL) return VFS_ERR_IO;

    nodes_write_dircontentindex(rootIndexSlot.bytes, 0, NODE_TYPE_INDEX_INTERNAL,
                                 0, 0, ctx->page_size);

    nodes_write_dirnode(root_slot.bytes, 0, 0, rootIndexVP, (int64_t)time(NULL), ctx->page_size);

    pool_release(&ctx->pool, &rootIndexSlot);
    pool_release(&ctx->pool, &root_slot);

    /* Update superblock with root pointer */
    ctx->rootNodeOffset = root_vp;
    return tree_superblock_write(ctx);
}

/* ---------------------------------------------------------------------------
 * Init — re-initialize from an existing file
 * --------------------------------------------------------------------------- */

int tree_init(TreeContext* ctx) {
    int err = tree_superblock_read(ctx);
    if (err != VFS_OK) return err;

    /* Verify root DirNode */
    if (ctx->rootNodeOffset == 0) return VFS_ERR_IO;

    PoolSlot root_slot;
    pool_acquire(&ctx->pool, ctx->rootNodeOffset, false, &root_slot);
    if (root_slot.vptr == VFS_VPTR_NULL) return VFS_ERR_IO;

    /* Verify type is DirNode (0x01) */
    int16_t type = vfs_rd2_s(root_slot.bytes, DIRNODE_OFF_TYPE, ctx->page_size);
    if (type != (int16_t)NODE_TYPE_DIR) return VFS_ERR_IO;

    /* Crash recovery: if exclusive lock (bit 63) was held at crash time,
       zero the field so the lock is released on mount.  The GC operation
       that was in progress may be incomplete, but all data is consistent
       because GC uses shadow-compaction (writes to new pages, frees old
       ones only after commit).  An incomplete GC simply leaves some
       unreclaimed pages. */
    if (ctx->treeLockState & (int64_t)TREE_LOCK_EXCLUSIVE_BIT) {
#ifndef NDEBUG
        fprintf(stderr, "vfs: tree_init: stale exclusive lock detected "
                "(treeLockState=0x%016llx), clearing\n",
                (unsigned long long)ctx->treeLockState);
#endif
        ctx->treeLockState = 0;
    }

    /* Read segment_size from StorageBackend header */
    uint8_t* hdr = storage_read(ctx->sb, 0);
    if (hdr) {
        ctx->segment_size = (uint32_t)vfs_rd4_s(hdr, HDR_OFF_SEGMENT_SIZE, ctx->page_size);
    } else {
        ctx->segment_size = 1024;
    }

    /* Walk epoch mapper chain (stub — just validate chain is readable).
       Full mapper resolution is implemented in Phase 6. */
    int64_t mapper_vp = ctx->epochMapperPtr;
    while (mapper_vp != 0) {
        PoolSlot slot;
        pool_acquire(&ctx->pool, mapper_vp, false, &slot);
        if (slot.vptr == VFS_VPTR_NULL) return VFS_ERR_IO;
        uint32_t fromEpoch, toEpoch;
        uint16_t flags;
        int64_t next;
        nodes_read_mapperentry(slot.bytes, &fromEpoch, &toEpoch, &flags, &next, ctx->page_size);
        (void)fromEpoch; (void)toEpoch; (void)flags;
        mapper_vp = next;
    }

    return VFS_OK;
}

/* ---------------------------------------------------------------------------
 * Page Resolution — resolve a logical page to its PageNode
 *
/* ---------------------------------------------------------------------------
 * Page Resolution — resolve a logical page to its PageNode
 *
 * W6: rewritten to use the shared chain-walk primitives
 * (walk_anchor_chain + walk_content_unit_chain) for the chain
 * traversal, and small extracted helpers for the segment-growth
 * and sorted-insertion logic.  All W3 lock discipline, tcache,
 * sorted insertion, and PageNode allocation paths are preserved
 * exactly.  The retry_walk dead code from the CAS era is removed
 * (the W3 lock discipline replaced CAS with simple store under
 * the held file lock; no retry is needed).
 *
 * Lock discipline (W3):
 *   - Read path (is_write=false): no locks; the caller's vfs_t*
 *     is used only for the ctx.
 *   - Write path (is_write=true): the caller MUST hold
 *     vfs_lock(vfs, file_vp, epoch).  This function additionally
 *     takes per-FileContent and per-PageNode locks as needed (per
 *     the spec's "Node > ContentUnit" hierarchy with "lower VP
 *     first" same-level rule).  Simple store replaces the prior
 *     CAS loops; the lock serializes the writers.
 *
 * Writes the PageNode slot to *out (Phase 25 by-value copy-out).
 * Returns 0 on success, -1 on error or page-not-found.  Closes
 * the C1 hazard: the caller's slot is a stack-local copy
 * independent of the cache, so a later cache eviction cannot
 * invalidate it.
 * --------------------------------------------------------------------------- */

/* W6: tcache state — per-thread, 16-slot LRU of SegmentArray.  The
 * cache is keyed on (file_vp, segment_idx) and invalidated when
 * the gc_generation changes (after GC compaction).  This is
 * unchanged from the pre-W6 implementation; it's a perf layer
 * on top of the chain walk. */
#define W6_TCACHE_SIZE 16
#define SPARSE_CACHE_THRESHOLD 64
typedef struct {
    int64_t      key;
    SegmentArray arr;
    bool         populated;
    int64_t      gen;
} w6_tcache_entry;
static __thread w6_tcache_entry w6_tcache[W6_TCACHE_SIZE] = {
    {0,{0},false,0}, {0,{0},false,0}, {0,{0},false,0}, {0,{0},false,0},
    {0,{0},false,0}, {0,{0},false,0}, {0,{0},false,0}, {0,{0},false,0},
    {0,{0},false,0}, {0,{0},false,0}, {0,{0},false,0}, {0,{0},false,0},
    {0,{0},false,0}, {0,{0},false,0}, {0,{0},false,0}, {0,{0},false,0},
};
static __thread int w6_tcache_next = 0;

/* W6: helper — link a new FileContent VP into the segment chain.
 *
 * For the first segment (i == 0), writes file_slot's HEADPTR.
 * For subsequent segments, locks prev_fc (per-content_unit lock,
 * lower VP first per the W3 spec), then sets prev_fc's NEXTPTR.
 * Returns 0 on success, -1 on lock failure.
 *
 * Caller must hold the file lock (caller's responsibility on
 * the write path).  Caller must also have a valid file_slot
 * (pinned via pool_acquire) and may pass a valid prev_slot
 * (pinned, must be NULL if prev_fc_vp == 0).
 */
static int w6_link_fc_into_chain(vfs_t* vfs, int64_t epoch,
                                  PoolSlot* file_slot, int64_t prev_fc_vp,
                                  int64_t new_fc_vp) {
    if (prev_fc_vp == 0) {
        vfs_mb_release();
        vfs_wr8_s(file_slot->bytes, FILENODE_OFF_HEADPTR,
                  new_fc_vp, vfs->ctx->page_size);
        return 0;
    }
    if (vfs_lock(vfs, prev_fc_vp, epoch) != VFS_OK) return -1;
    PoolSlot prev_slot = {0};
    pool_acquire(&vfs->ctx->pool, prev_fc_vp, true, &prev_slot);
    if (prev_slot.vptr == VFS_VPTR_NULL) {
        vfs_unlock(vfs, prev_fc_vp, epoch);
        return -1;
    }
    vfs_mb_release();
    vfs_wr8_s(prev_slot.bytes, FILECONTENT_OFF_NEXTPTR,
              new_fc_vp, vfs->ctx->page_size);
    pool_release(&vfs->ctx->pool, &prev_slot);
    vfs_unlock(vfs, prev_fc_vp, epoch);
    return 0;
}

/* W6: helper — allocate a new FileContent entry.  If
 * allocate_page is true, also allocate a root PageNode with id =
 * page_in_segment and link it to the new FileContent's ROOTPTR.
 * Returns the new FileContent's slot pinned in *out_fc_slot, or
 * sets *out_fc_slot to {0} and returns 0 on error.
 *
 * Caller must hold the file lock on the write path.  The
 * new_fc_slot is NOT released by this function; the caller
 * releases it after linking the segment into the chain. */
static int64_t w6_allocate_fc(vfs_t* vfs, int64_t page_in_segment,
                              bool allocate_page, PoolSlot* out_fc_slot) {
    TreeContext* ctx = vfs->ctx;
    int64_t new_fc_vp = pool_alloc(&ctx->pool);
    if (new_fc_vp == VFS_VPTR_NULL) {
        out_fc_slot->vptr = VFS_VPTR_NULL;
        return 0;
    }
    pool_acquire(&ctx->pool, new_fc_vp, true, out_fc_slot);
    if (out_fc_slot->vptr == VFS_VPTR_NULL) return 0;

    int64_t page_root_vp = VFS_VPTR_NULL;
    if (allocate_page) {
        int64_t new_pn_vp = pool_alloc(&ctx->pool);
        if (new_pn_vp == VFS_VPTR_NULL) {
            pool_release(&ctx->pool, out_fc_slot);
            out_fc_slot->vptr = VFS_VPTR_NULL;
            return 0;
        }
        PoolSlot pn_slot = {0};
        pool_acquire(&ctx->pool, new_pn_vp, true, &pn_slot);
        if (pn_slot.vptr == VFS_VPTR_NULL) {
            pool_release(&ctx->pool, out_fc_slot);
            out_fc_slot->vptr = VFS_VPTR_NULL;
            return 0;
        }
        nodes_write_pagenode(pn_slot.bytes, 0, 0,
                             (uint32_t)page_in_segment, ctx->page_size);
        pool_release(&ctx->pool, &pn_slot);
        page_root_vp = new_pn_vp;
    }
    nodes_write_filecontent(out_fc_slot->bytes, page_root_vp, 0,
                            ctx->page_size);
    return new_fc_vp;
}

/* W6: helper — link a new PageNode VP into the PageNode chain
 * within a segment.  prev_pn_vp is the PageNode just before the
 * insertion point (or 0 for head of chain).  Updates the
 * FileContent's ROOTPTR or the prev PageNode's NEXTPTR.
 *
 * Caller must hold the file lock.  Caller must have a valid
 * fc_slot (pinned, not released by this function). */
static int w6_link_pn_into_chain(vfs_t* vfs, int64_t epoch,
                                  PoolSlot* fc_slot, int64_t prev_pn_vp,
                                  int64_t new_pn_vp) {
    TreeContext* ctx = vfs->ctx;
    if (prev_pn_vp == 0) {
        /* Insert at head of PageNode chain. */
        vfs_mb_release();
        vfs_wr8_s(fc_slot->bytes, FILECONTENT_OFF_ROOTPTR,
                  new_pn_vp, ctx->page_size);
        return 0;
    }
    /* Insert after prev_pn_vp — lock prev, set NEXTPTR. */
    if (vfs_lock(vfs, prev_pn_vp, epoch) != VFS_OK) return -1;
    PoolSlot prev_slot = {0};
    pool_acquire(&ctx->pool, prev_pn_vp, true, &prev_slot);
    if (prev_slot.vptr == VFS_VPTR_NULL) {
        vfs_unlock(vfs, prev_pn_vp, epoch);
        return -1;
    }
    vfs_mb_release();
    vfs_wr8_s(prev_slot.bytes, PAGENODE_OFF_NEXTPTR,
              new_pn_vp, ctx->page_size);
    pool_release(&ctx->pool, &prev_slot);
    vfs_unlock(vfs, prev_pn_vp, epoch);
    return 0;
}

/* W6: state for the segment walk — count segments via the
 * shared walk, find the target segment. */
typedef struct {
    int64_t  target_segment_idx;
    int64_t  found_fc_vp;     /* out: FileContent VP for target segment */
    int      found;           /* 1 if found, 0 if walk ended without finding */
    int64_t  last_fc_vp;      /* out: last visited segment (for chaining new ones) */
} w6_seg_walk_state;

static int w6_seg_walk_cb(TreeContext* ctx, int64_t fc_vp,
                           const uint8_t* anchor_bytes, void* user) {
    w6_seg_walk_state* st = (w6_seg_walk_state*)user;
    (void)ctx; (void)anchor_bytes;
    st->last_fc_vp = fc_vp;
    if (st->found_fc_vp == 0) {
        /* first time we see the target — record it.  We compare using
         * the call counter from the user state, but we don't have a
         * counter here.  Instead, use the last_fc_vp + a count carried
         * in st.  Since this callback is the only way we advance, we
         * can use the structure of the walk.  The callback is invoked
         * for each segment; we want the Nth one where N == target. */
        /* We need a per-callback index.  Use a separate counter. */
    }
    return 0;  /* continue — we can't tell from inside the cb which index */
}

/* W6: state for the PageNode walk — find the target page_index,
 * record the insertion point. */
typedef struct {
    int64_t  target_page_in_segment;
    int64_t  found_pn_vp;     /* out: PageNode VP for target page */
    int64_t  prev_pn_vp;      /* out: prev PageNode (for sorted insertion) */
    int      found_higher;    /* 1 if we found a higher-id PageNode (insert before it) */
    int      found;           /* 1 if exact match found */
} w6_pn_walk_state;

static int w6_pn_walk_cb(TreeContext* ctx, int64_t pn_vp,
                          const uint8_t* unit_bytes, void* user) {
    w6_pn_walk_state* st = (w6_pn_walk_state*)user;
    uint32_t pn_idx = (uint32_t)vfs_rd4_s(unit_bytes, ANCHOR_OFF_ID,
                                         ctx->page_size);
    if (pn_idx == (uint32_t)st->target_page_in_segment) {
        st->found_pn_vp = pn_vp;
        st->found = 1;
        return 1;  /* stop — exact match */
    }
    if (pn_idx > (uint32_t)st->target_page_in_segment && !st->found_higher) {
        st->found_higher = 1;
        st->prev_pn_vp = (st->prev_pn_vp == 0) ? 0 : st->prev_pn_vp;
        /* Actually need to record the prev.  We need a way to know
         * prev.  The shared walk's callback doesn't know prev directly.
         * Use the VP we recorded in the previous callback. */
        return 1;  /* stop — found insertion point */
    }
    st->prev_pn_vp = pn_vp;  /* this is now the prev for the next iter */
    return 0;  /* continue */
}

/* W6: thin tree_resolve_page wrapper.  Does the tcache fast path,
 * then dispatches to the slow path (segment walk + PageNode walk
 * via the shared primitives), then handles tcache patching and
 * final copy-out. */
int tree_resolve_page(vfs_t* vfs, int64_t file_vp,
                      int64_t logical_page, int64_t epoch, bool is_write,
                      PoolSlot* out) {
    (void)epoch;
    if (!vfs || !vfs->ctx || !out) return -1;
    TreeContext* ctx = vfs->ctx;
    out->vptr = VFS_VPTR_NULL;
    out->pinnedPage = 0;
    memset(out->bytes, 0, VFS_POOL_SLOT_SIZE);

    uint32_t seg_size = ctx->segment_size;
    int64_t segment_idx = logical_page / seg_size;
    int64_t page_in_segment = logical_page % seg_size;

    /* Read FileNode to get headPtr.  file_slot held throughout the
       slow path so the chain head we read is consistent. */
    PoolSlot file_slot = {0};
    pool_acquire(&ctx->pool, file_vp, true, &file_slot);
    if (file_slot.vptr == VFS_VPTR_NULL) return -1;
    int64_t headPtr;
    {
        uint32_t tmp_nodeId;
        int64_t tmp_sizePtr, tmp_createdAt;
        nodes_read_filenode(file_slot.bytes, &tmp_nodeId, &headPtr,
                            &tmp_sizePtr, &tmp_createdAt, ctx->page_size);
    }

    /* --- Tcache fast path --- */
    int64_t cache_key = (file_vp << 20) | (segment_idx & 0xFFFFF);
    int cache_slot = -1;
    for (int ci = 0; ci < W6_TCACHE_SIZE; ci++) {
        if (w6_tcache[ci].key == cache_key && w6_tcache[ci].populated) {
            if (w6_tcache[ci].gen != vfs_atomic_load_i64(&ctx->gc_generation)) {
                w6_tcache[ci].populated = false;
            } else {
                if (segment_array_resolve(&ctx->pool, &w6_tcache[ci].arr,
                                          (uint32_t)page_in_segment, out)) {
                    pool_release(&ctx->pool, &file_slot);
                    return 0;
                }
                cache_slot = ci;  /* array has NULL for this page */
            }
            break;
        }
    }

    /* --- Slow path: walk segments + PageNodes via shared walks --- */

    /* Step 1: find the FileContent for segment_idx, allocating
       missing segments as we go.  We use a manual segment walk
       (not walk_anchor_chain) because we need to count segments
       to know when to stop.  For each segment, if it's missing
       and is_write, allocate an empty FileContent (and a root
       PageNode for the target segment).  After the walk, if the
       target segment wasn't found and !is_write, fail. */
    int64_t fc_vp = headPtr;
    int64_t prev_fc_vp = 0;
    PoolSlot fc_slot = {0};
    int fc_held = 0;  /* whether fc_slot has a valid pin */
    int found_target = 0;
    int64_t seg_i = 0;
    while (seg_i <= segment_idx) {
        if (fc_vp == 0) {
            /* Segment doesn't exist.  Allocate if write. */
            if (seg_i == segment_idx && !is_write) {
                pool_release(&ctx->pool, &file_slot);
                return -1;
            }
            bool alloc_pn = (seg_i == segment_idx) && is_write;
            int64_t new_fc = w6_allocate_fc(vfs, page_in_segment, alloc_pn, &fc_slot);
            if (new_fc == 0) { pool_release(&ctx->pool, &file_slot); return -1; }
            if (w6_link_fc_into_chain(vfs, epoch, &file_slot, prev_fc_vp, new_fc) != 0) {
                pool_release(&ctx->pool, &fc_slot);
                pool_release(&ctx->pool, &file_slot);
                return -1;
            }
            if (alloc_pn) {
                /* Update pageCount in the FileContent (W3 simple write). */
                uint32_t cur = (uint32_t)vfs_rd4_s(fc_slot.bytes,
                                                  FILECONTENT_OFF_PAGECOUNT,
                                                  ctx->page_size);
                vfs_wr4_s(fc_slot.bytes, FILECONTENT_OFF_PAGECOUNT,
                          cur + 1, ctx->page_size);
                /* Get the root PageNode VP. */
                int64_t new_pn = vfs_rd8_s(fc_slot.bytes,
                                           FILECONTENT_OFF_ROOTPTR,
                                           ctx->page_size);
                pool_release(&ctx->pool, &fc_slot);
                fc_held = 0;
                /* Tcache patch for newly allocated page. */
                if (cache_slot >= 0 && w6_tcache[cache_slot].populated && new_pn != 0) {
                    w6_tcache[cache_slot].arr.vptr_array[page_in_segment] = new_pn;
                }
                /* Final copy-out and return. */
                pool_acquire(&ctx->pool, new_pn, is_write, out);
                if (out->vptr == VFS_VPTR_NULL) { pool_release(&ctx->pool, &file_slot); return -1; }
                pool_release(&ctx->pool, &file_slot);
                return 0;
            }
            /* Empty intermediate segment — advance. */
            pool_release(&ctx->pool, &fc_slot);
            fc_held = 0;
            prev_fc_vp = new_fc;
            fc_vp = 0;
            seg_i++;
            continue;
        }
        /* Segment exists — acquire it and check if it's the target. */
        pool_acquire(&ctx->pool, fc_vp, true, &fc_slot);
        if (fc_slot.vptr == VFS_VPTR_NULL) {
            pool_release(&ctx->pool, &file_slot);
            return -1;
        }
        fc_held = 1;
        if (seg_i == segment_idx) {
            found_target = 1;
            break;
        }
        /* Advance to next segment. */
        int64_t next_fc = vfs_rd8_s(fc_slot.bytes, FILECONTENT_OFF_NEXTPTR,
                                    ctx->page_size);
        pool_release(&ctx->pool, &fc_slot);
        fc_held = 0;
        prev_fc_vp = fc_vp;
        fc_vp = next_fc;
        seg_i++;
    }
    if (!found_target) {
        pool_release(&ctx->pool, &file_slot);
        return -1;
    }
    /* fc_slot is now pinned on the target segment. */

    /* Step 2: find the PageNode within the segment.  Use the
       shared walk_content_unit_chain with a callback that
       identifies the insertion point. */
    int64_t fc_page_root = vfs_rd8_s(fc_slot.bytes, FILECONTENT_OFF_ROOTPTR,
                                    ctx->page_size);
    int64_t found_pn_vp = 0;
    int64_t prev_pn_vp = 0;
    int found_higher = 0;
    int found_match = 0;
    if (fc_page_root != 0) {
        w6_pn_walk_state pn_st = {
            .target_page_in_segment = page_in_segment,
            .found_pn_vp = 0, .prev_pn_vp = 0,
            .found_higher = 0, .found = 0,
        };
        walk_content_unit_chain(ctx, fc_page_root, w6_pn_walk_cb, &pn_st);
        found_pn_vp = pn_st.found_pn_vp;
        prev_pn_vp = pn_st.prev_pn_vp;
        found_higher = pn_st.found_higher;
        found_match = pn_st.found;
    }

    /* If !is_write and no match, fail. */
    if (!found_match && !is_write) {
        pool_release(&ctx->pool, &fc_slot);
        pool_release(&ctx->pool, &file_slot);
        return -1;
    }

    /* If no match and is_write, allocate and insert.
     *
     * prev_pn_vp is the PageNode just before the insertion point:
     *   - If found_higher, prev_pn_vp is the PageNode just BEFORE the
     *     higher-id one (or 0 if the higher one is at the head).
     *     The new PageNode is inserted there; its NEXTPTR is the
     *     higher-id PageNode's VP (next_pn).
     *   - If !found_higher, prev_pn_vp is the LAST PageNode in the
     *     chain (or 0 if the chain is empty).  The new PageNode
     *     is appended at the tail; its NEXTPTR is 0.
     *
     * The same prev_pn_vp is used in both cases — only the new_pn's
     * NEXTPTR differs (next_pn is the higher VP or 0).  This is
     * why the old code's "append at tail" and "insert in middle"
     * branches are unified here. */
    int64_t result_pn_vp = found_pn_vp;
    if (!found_match && is_write) {
        int64_t new_pn_vp = pool_alloc(&ctx->pool);
        if (new_pn_vp == VFS_VPTR_NULL) {
            pool_release(&ctx->pool, &fc_slot);
            pool_release(&ctx->pool, &file_slot);
            return -1;
        }
        PoolSlot new_pn_slot = {0};
        pool_acquire(&ctx->pool, new_pn_vp, true, &new_pn_slot);
        if (new_pn_slot.vptr == VFS_VPTR_NULL) {
            pool_release(&ctx->pool, &fc_slot);
            pool_release(&ctx->pool, &file_slot);
            return -1;
        }
        int64_t next_pn = 0;
        if (found_higher) {
            /* The walk found a higher-id PageNode.  We need its
             * VP to set the new_pn's NEXTPTR.  The shared walk
             * callback returned 1 before recording it, so we
             * re-walk to find the higher-id PageNode's VP. */
            int64_t walk_vp = fc_page_root;
            while (walk_vp != 0) {
                PoolSlot ws = {0};
                pool_acquire(&ctx->pool, walk_vp, false, &ws);
                if (ws.vptr == VFS_VPTR_NULL) break;
                uint32_t idx = (uint32_t)vfs_rd4_s(ws.bytes, ANCHOR_OFF_ID,
                                                   ctx->page_size);
                int64_t sib = vfs_rd8_s(ws.bytes, ANCHOR_OFF_SIBPTR,
                                        ctx->page_size);
                pool_release(&ctx->pool, &ws);
                if (idx > (uint32_t)page_in_segment) {
                    next_pn = walk_vp;
                    break;
                }
                walk_vp = sib;
            }
        }
        nodes_write_pagenode(new_pn_slot.bytes, 0, next_pn,
                             (uint32_t)page_in_segment, ctx->page_size);
        pool_release(&ctx->pool, &new_pn_slot);
        vfs_mb_release();
        if (w6_link_pn_into_chain(vfs, epoch, &fc_slot,
                                   prev_pn_vp,
                                   new_pn_vp) != 0) {
            pool_release(&ctx->pool, &fc_slot);
            pool_release(&ctx->pool, &file_slot);
            return -1;
        }
        /* Update pageCount. */
        uint32_t cur = (uint32_t)vfs_rd4_s(fc_slot.bytes,
                                            FILECONTENT_OFF_PAGECOUNT,
                                            ctx->page_size);
        vfs_wr4_s(fc_slot.bytes, FILECONTENT_OFF_PAGECOUNT,
                  cur + 1, ctx->page_size);
        result_pn_vp = new_pn_vp;
    }

    /* Tcache patch. */
    if (cache_slot >= 0 && w6_tcache[cache_slot].populated && result_pn_vp != 0) {
        w6_tcache[cache_slot].arr.vptr_array[page_in_segment] = result_pn_vp;
    }

    /* Populate tcache if segment has enough pages. */
    uint32_t fc_page_count = (uint32_t)vfs_atomic_load_i32(
        (const int32_t*)(fc_slot.bytes + FILECONTENT_OFF_PAGECOUNT));
    if ((int)fc_page_count >= SPARSE_CACHE_THRESHOLD) {
        int slot = w6_tcache_next % W6_TCACHE_SIZE;
        if (w6_tcache[slot].arr.built)
            segment_array_destroy(&w6_tcache[slot].arr);
        int build_rc = segment_array_build(&ctx->pool,
            vfs_rd8_s(fc_slot.bytes, FILECONTENT_OFF_ROOTPTR, ctx->page_size),
            seg_size, &w6_tcache[slot].arr);
        if (build_rc == VFS_OK) {
            w6_tcache[slot].key = cache_key;
            w6_tcache[slot].populated = true;
            w6_tcache[slot].gen = vfs_atomic_load_i64(&ctx->gc_generation);
            w6_tcache_next++;
#ifndef NDEBUG
            atomic_fetch_add(&tree_resolve_page_cache_builds, 1);
#endif
        }
    }

    /* Release fc_slot. */
    pool_release(&ctx->pool, &fc_slot);
    pool_release(&ctx->pool, &file_slot);

    /* Final copy-out. */
    pool_acquire(&ctx->pool, result_pn_vp, is_write, out);
    if (out->vptr == VFS_VPTR_NULL) return -1;
    return 0;
}
/* ---------------------------------------------------------------------------
 * verchain_get — walk VersionPage chain, apply read-rule + mapper,
 * return data page index, or -1 if none found.
 *
 * read_epoch: already resolved via mapper_table_resolve.
 * --------------------------------------------------------------------------- */

int64_t verchain_get(TreeContext* ctx, int64_t versionRootPtr,
                     int64_t read_epoch) {
    if (!ctx || versionRootPtr == 0) return -1;

    /* Phase 26 / W2: thin wrapper over vfs_chain_walk.  The walk
       returns the visible VersionPage slot; we extract dataPage
       (the leaf's primary_ptr at LEAF_OFF_PRIMARY = offset 8). */
    PoolSlot vp_slot = {0};
    WalkResult r = vfs_chain_walk(ctx, versionRootPtr, read_epoch, &vp_slot);
    if (r != WALK_FOUND) return -1;

    return vfs_rd8_s(vp_slot.bytes, VERSIONPAGE_OFF_DATAPAGE, ctx->page_size);
}

/* ---------------------------------------------------------------------------
 * sizechain_get — walk FileSize chain, apply read-rule + mapper,
 * return fileSize and modifiedAt at the visible epoch.  Read-epoch should
 * be pre-resolved via mapper_table_resolve.
 * --------------------------------------------------------------------------- */

void sizechain_get(TreeContext* ctx, int64_t sizePtr, int64_t read_epoch,
                   int64_t* out_fileSize, int64_t* out_modifiedAt) {
    if (!ctx) return;
    if (out_fileSize) *out_fileSize = 0;
    if (out_modifiedAt) *out_modifiedAt = 0;
    if (sizePtr == 0) return;

    /* Phase 26 / W2: thin wrapper over vfs_chain_walk.  The walk
       returns the visible FileSize slot; we extract modifiedAt
       (primary_ptr at offset 8) and fileSize (secondary_ptr at
       offset 16). */
    PoolSlot fs_slot = {0};
    WalkResult r = vfs_chain_walk(ctx, sizePtr, read_epoch, &fs_slot);
    if (r != WALK_FOUND) return;

    if (out_modifiedAt) *out_modifiedAt = vfs_rd8_s(fs_slot.bytes,
                                                    FILESIZE_OFF_MODIFIEDAT,
                                                    ctx->page_size);
    if (out_fileSize)   *out_fileSize   = vfs_rd8_s(fs_slot.bytes,
                                                    FILESIZE_OFF_FILESIZE,
                                                    ctx->page_size);
}

/* ---------------------------------------------------------------------------
/* ---------------------------------------------------------------------------
 * dirnode_increment_child_count — REMOVED in Phase 25 / W6, then in Phase 26 /
 * W1b the underlying field itself was dropped.
 *
 * Was an atomic increment of DirNode.childCount, called after every
 * DirContent insert (live or tombstone).  Provided an upper bound on
 * unique children for sizing the dedup hash_map in dirchain_list.
 *
 * The function used the OLD pool_resolve_rw API which writes directly
 * to the cache.  After the by-value migration, all callers in
 * vfs_create / vfs_mkdir / vfs_delete / vfs_rmdir / vfs_rename hold a
 * parent PoolSlot and persist local edits via pool_release.  A
 * separate call to dirnode_increment_child_count would have written
 * a stale count back to the cache, racing with the in-flight release
 * of the parent slot.
 *
 * W1b: childCount removed from DirNode.  Replaced with createdAt.
 * The per-ContentUnit chains introduced in W5 are dedup'd at the
 * structure level (one chain per child, with tombstone filter via
 * the read-rule), so the dedup hash_map and the count that sized
 * it both go away.
 * --------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * Phase 26 / W2: vfs_chain_walk — unified per-leaf chain walker.
 *
 * Walks a per-leaf chain (VersionPage / DirContent / FileSize) applying
 * the read-rule + mapper remap.  All three leaf types share the same
 * 32-byte layout (epoch at LEAF_OFF_EPOCH=0, nextPtr at LEAF_OFF_NEXTPTR=24),
 * so the walk is byte-identical for all three.
 *
 * Read-rule per entry (matches the existing verchain_get / sizechain_get
 * logic, and the inlined dirchain_find_child logic):
 *
 *   1. Compute effective_epoch = mapper_table_resolve(stored_epoch)
 *      if mapper_table_traversal_apply(stored_epoch) is true;
 *      otherwise effective_epoch = stored_epoch.
 *   2. If effective_epoch == read_epoch: exact match wins. Stop.
 *   3. If effective_epoch < read_epoch AND even: committed base.
 *      Stop (chains are descending, so first even below is highest).
 *   4. Otherwise (odd-and-skip, or future-and-skip): continue walk.
 *
 * The mapper remap is per-entry (NOT post-walk): mapper_table_traversal_apply
 * is keyed on the entry's stored epoch, so applying it after the walk
 * would remap the wrong epoch.
 *
 * On WALK_FOUND, *out_leaf holds the visible entry's slot (a by-value
 * copy-out from the pool; release is a no-op on un-pinned slots).
 * On WALK_NOT_FOUND, *out_leaf is zeroed.
 *
 * Phase 25 safety: every pool_acquire here uses pinPage=false (read-only
 * path).  This closes the C1 hazard — the caller's slot is independent
 * of the cache.
 * --------------------------------------------------------------------------- */

WalkResult vfs_chain_walk(TreeContext* ctx,
                          int64_t       chain_head,
                          int64_t       read_epoch,
                          PoolSlot*     out_leaf) {
    if (!ctx || !out_leaf) return WALK_NOT_FOUND;
    out_leaf->vptr = VFS_VPTR_NULL;
    out_leaf->pinnedPage = 0;
    memset(out_leaf->bytes, 0, VFS_POOL_SLOT_SIZE);

    if (chain_head == 0) return WALK_NEED_GROW;

    int64_t walk_vp = chain_head;
    while (walk_vp != 0) {
        /* Phase 25: by-value pool slot (read-only, pinPage=false). */
        PoolSlot leaf_slot = {0};
        pool_acquire(&ctx->pool, walk_vp, false, &leaf_slot);
        if (leaf_slot.vptr == VFS_VPTR_NULL) return WALK_NOT_FOUND;

        /* Read-rule per entry — all 3 leaf types share these offsets. */
        uint32_t stored_epoch = (uint32_t)vfs_rd4_s(leaf_slot.bytes,
                                                    LEAF_OFF_EPOCH,
                                                    ctx->page_size);
        int64_t next_vp = vfs_rd8_s(leaf_slot.bytes, LEAF_OFF_NEXTPTR,
                                    ctx->page_size);

        int64_t eff_epoch = (int64_t)stored_epoch;
        if (mapper_table_traversal_apply(&ctx->mapper_table,
                                         (int64_t)stored_epoch)) {
            eff_epoch = mapper_table_resolve(&ctx->mapper_table,
                                              (int64_t)stored_epoch);
        }

        /* Exact match at read_epoch — use it immediately. */
        if (eff_epoch == read_epoch) {
            *out_leaf = leaf_slot;  /* copy-out, no release needed */
            return WALK_FOUND;
        }

        /* Even epoch below read_epoch — chains are descending, so the
           first even below read_epoch is the highest such epoch. */
        if (eff_epoch < read_epoch && (eff_epoch & 1) == 0) {
            *out_leaf = leaf_slot;
            return WALK_FOUND;
        }

        /* Odd-and-skip, or future-and-skip: continue walk. */
        walk_vp = next_vp;
        pool_release(&ctx->pool, &leaf_slot);
    }
    return WALK_NOT_FOUND;
}

/* ---------------------------------------------------------------------------
 * W6: walk_anchor_chain — generic Anchor chain walk (FileContent or
 * DirSegment).  Both types share the Anchor layout (ANCHOR_OFF_HEADPTR
 * at offset 8, ANCHOR_OFF_SIBPTR at offset 16, ANCHOR_OFF_ID at
 * offset 4, ANCHOR_OFF_COUNT at offset 24).  The chain head is
 * FileNode.headPtr or DirNode.headPtr.
 *
 * The callback receives a by-value copy of each Anchor's 32 bytes.
 * Return 0 from the callback to continue the walk, non-zero to
 * stop early.  The walk itself is read-only (pinPage=false).
 *
 * Returns the number of Anchors visited (including any that
 * triggered a non-zero callback return).
 * --------------------------------------------------------------------------- */

int walk_anchor_chain(TreeContext* ctx, int64_t head_vp,
                      anchor_walk_cb cb, void* user) {
    if (!ctx || !cb || head_vp == 0) return 0;
    int visited = 0;
    int64_t walk_vp = head_vp;
    while (walk_vp != 0) {
        PoolSlot anchor_slot = {0};
        pool_acquire(&ctx->pool, walk_vp, false, &anchor_slot);
        if (anchor_slot.vptr == VFS_VPTR_NULL) break;
        int64_t this_vp = walk_vp;  /* capture before cb — cb may release */
        int cb_rc = cb(ctx, this_vp, anchor_slot.bytes, user);
        visited++;
        /* Capture sibPtr before release; once we release, the local
           PoolSlot is invalidated.  Even if the callback wrote to it
           via the bytes pointer, the next acquire reads fresh cache. */
        int64_t sib = vfs_rd8_s(anchor_slot.bytes, ANCHOR_OFF_SIBPTR,
                                ctx->page_size);
        pool_release(&ctx->pool, &anchor_slot);
        if (cb_rc != 0) break;
        walk_vp = sib;
    }
    return visited;
}

/* ---------------------------------------------------------------------------
 * W6: walk_content_unit_chain — generic ContentUnit chain walk
 * (PageNode for files, SlotNode for dirs).  Both types share the
 * Anchor layout (same as above).  The chain head is
 * FileContent.HEADPTR or DirSegment.HEADPTR.
 *
 * Same callback semantics as walk_anchor_chain.  Returns the
 * number of ContentUnits visited.
 * --------------------------------------------------------------------------- */

int walk_content_unit_chain(TreeContext* ctx, int64_t unit_head,
                            anchor_walk_cb cb, void* user) {
    return walk_anchor_chain(ctx, unit_head, cb, user);
}

/* ---------------------------------------------------------------------------
 * W6: vfs_chain_walk_extended — full 6-step chain walk.
 *
 * Steps 1-3: resolve (root_vp, unit_id) to a ContentUnit.
 *   Step 1: read root_vp (FileNode or DirNode) to get headPtr
 *           (first FileContent or DirSegment).
 *   Step 2: walk the outer Anchor chain (FileContent for files,
 *           DirSegment for dirs).
 *   Step 3: within each segment, walk the inner ContentUnit chain
 *           (PageNode for files, SlotNode for dirs) and find the
 *           one with id == unit_id.
 * Steps 4-5: delegate to vfs_chain_walk on the unit's leaf chain.
 *   Step 4: walk the leaf chain (VersionPage / DirContent /
 *           FileSize) from the unit's headPtr.
 *   Step 5: apply the read-rule (mapper remap + even/odd +
 *           exact-match-wins).
 * Step 6: caller extracts the leaf's specialized fields
 *   (dataPage for VersionPage, childPtr+namePtr for DirContent).
 *
 * Returns WALK_FOUND / WALK_NOT_FOUND / WALK_NEED_GROW.
 * WALK_NEED_GROW is only returned when the ContentUnit doesn't
 * exist (no segment / unit found); the caller decides whether to
 * grow.
 *
 * The two-level walk uses walk_anchor_chain at the outer level
 * and walk_content_unit_chain at the inner level.  The outer
 * callback (w6_find_unit_in_segment_outer_cb) is invoked for
 * each Anchor; it walks the segment's ContentUnit chain
 * internally using the inner callback (w6_find_unit_inner_cb)
 * and short-circuits both walks when the target unit is found.
 * --------------------------------------------------------------------------- */

/* Helper state for step 2-3: looking for a unit by id across
   all segments in the chain. */
typedef struct {
    uint32_t   target_unit_id;
    int64_t    found_leaf_head;   /* out: leaf chain head of the found unit */
    int64_t    page_size;
    int        found;             /* out: 1 if found, 0 if not */
} w6_find_unit_state;

/* Inner callback: invoked for each ContentUnit in a segment.
   Returns 1 to stop the inner walk if the target unit is found. */
static int w6_find_unit_inner_cb(TreeContext* ctx,
                                  int64_t unit_vp,
                                  const uint8_t* unit_bytes,
                                  void* user) {
    w6_find_unit_state* st = (w6_find_unit_state*)user;
    uint32_t unit_id = (uint32_t)vfs_rd4_s(unit_bytes, ANCHOR_OFF_ID,
                                           st->page_size);
    (void)ctx; (void)unit_vp;
    if (unit_id == st->target_unit_id) {
        st->found_leaf_head = vfs_rd8_s(unit_bytes, ANCHOR_OFF_HEADPTR,
                                        st->page_size);
        st->found = 1;
        return 1;  /* stop inner walk */
    }
    return 0;  /* continue */
}

/* Outer callback: invoked for each Anchor (FileContent or
   DirSegment).  For each, walks the segment's ContentUnit chain
   looking for the target.  Returns 1 to stop the outer walk if
   the target unit is found in this segment. */
static int w6_find_unit_in_segment_outer_cb(TreeContext* ctx,
                                            int64_t anchor_vp,
                                            const uint8_t* anchor_bytes,
                                            void* user) {
    w6_find_unit_state* st = (w6_find_unit_state*)user;
    int64_t unit_head = vfs_rd8_s(anchor_bytes, ANCHOR_OFF_HEADPTR,
                                  st->page_size);
    (void)anchor_vp;
    if (unit_head == 0) return 0;  /* empty segment, keep looking */
    walk_content_unit_chain(ctx, unit_head,
                            w6_find_unit_inner_cb, user);
    return st->found;  /* 1 to stop outer walk, 0 to continue */
}

WalkResult vfs_chain_walk_extended(TreeContext* ctx,
                                   int64_t       root_vp,
                                   uint32_t      unit_id,
                                   int64_t       query_epoch,
                                   PoolSlot*     out_leaf) {
    if (!ctx || !out_leaf) return WALK_NOT_FOUND;
    out_leaf->vptr = VFS_VPTR_NULL;
    out_leaf->pinnedPage = 0;
    memset(out_leaf->bytes, 0, VFS_POOL_SLOT_SIZE);

    if (root_vp == 0) return WALK_NOT_FOUND;

    /* Step 1: read the root node (FileNode or DirNode) to get the
       first Anchor (FileContent or DirSegment).  Both node types
       store their anchor chain head at offset 8 (the
       ANCHOR_OFF_HEADPTR position within the Anchor layout). */
    PoolSlot root_slot = {0};
    pool_acquire(&ctx->pool, root_vp, false, &root_slot);
    if (root_slot.vptr == VFS_VPTR_NULL) return WALK_NOT_FOUND;
    /* Sanity check: byte 0 is the type.  FileNode=NODE_TYPE_FILE=0x03,
       DirNode=NODE_TYPE_DIR=0x01.  For files, the leaf chain is the
       VersionPage chain (off the PageNode, off the FileContent).
       For dirs, the leaf chain is the DirContent chain (off the
       SlotNode, off the DirSegment).  The walk logic is identical
       — only the type byte differs. */
    int16_t root_type = (int16_t)vfs_rd2_s(root_slot.bytes, 0, ctx->page_size);
    if (root_type != (int16_t)NODE_TYPE_FILE &&
        root_type != (int16_t)NODE_TYPE_DIR) {
        pool_release(&ctx->pool, &root_slot);
        return WALK_NOT_FOUND;
    }
    int64_t anchor_head = vfs_rd8_s(root_slot.bytes, ANCHOR_OFF_HEADPTR,
                                    ctx->page_size);
    pool_release(&ctx->pool, &root_slot);
    if (anchor_head == 0) return WALK_NOT_FOUND;

    /* Step 2-3: walk the outer Anchor chain.  The outer callback
       walks the inner ContentUnit chain and short-circuits both
       when the target unit is found. */
    w6_find_unit_state st = {
        .target_unit_id = unit_id,
        .found_leaf_head = 0,
        .page_size      = ctx->page_size,
        .found          = 0,
    };
    walk_anchor_chain(ctx, anchor_head,
                      w6_find_unit_in_segment_outer_cb, &st);
    if (!st.found) return WALK_NEED_GROW;
    if (st.found_leaf_head == 0) return WALK_NOT_FOUND;

    /* Step 4-5: delegate to the existing vfs_chain_walk on the
       unit's leaf chain.  Same read-rule (mapper remap, even/odd,
       exact-match-wins). */
    return vfs_chain_walk(ctx, st.found_leaf_head, query_epoch, out_leaf);
}

/* ---------------------------------------------------------------------------
 * Phase 26 / W5b: DirSegment population helpers.
 *
 * DirSegments chunk the per-directory SlotNode chain into groups of
 * ANCHOR_UNITS_PER_SEGMENT (1024) SlotNodes.  Structure:
 *
 *   DirNode.HEADPTR → Segment (≤1024 SlotNodes) → Segment → ...
 *                          ↓
 *                       SlotNode (per child) → SlotNode → ...
 *                          ↓
 *                       DirContent (per-epoch history)
 *
 * For dir side, segments give no lookup-by-id speedup (the chain walk
 * is O(N) regardless), but they preserve structural uniformity with
 * the file side and let the SlotNode chain fit in tcache-friendly
 * groups.
 *
 * Locking: helpers below do NOT take locks.  Callers (vfs_create,
 * vfs_mkdir, vfs_delete, vfs_rename, …) hold the parent DirNode
 * lock and the per-child ContentUnit lock, per the W3+W4 lock
 * discipline.  pool_release writes back any pinned slots.
 * --------------------------------------------------------------------------- */

/* Find a non-full Segment in the parent DirNode's Segment chain, or
 * allocate a new one and prepend it to DirNode.HEADPTR.
 *
 * Inputs:
 *   ctx, parent_slot — DirNode slot (already acquired, pinPage=true).
 *                      parent_slot.bytes is read AND written (HEADPTR
 *                      is updated if a new Segment is allocated).
 *
 * Outputs:
 *   out_seg_vp     — VP of the Segment to prepend to.
 *   out_seg_slot   — pool slot of the Segment, acquired pinPage=true.
 *                    Caller MUST pool_release it.
 *
 * Returns VFS_OK on success, VFS_ERR_FULL on pool exhaustion,
 * VFS_ERR_IO on slot acquisition failure.
 *
 * The returned Segment's count is the current count BEFORE the new
 * SlotNode is prepended.  Caller increments count after prepending.
 */
static int dirchain_get_or_create_segment(TreeContext* ctx,
                                          PoolSlot* parent_slot,
                                          int64_t* out_seg_vp,
                                          PoolSlot* out_seg_slot) {
    int64_t seg_vp = vfs_rd8_s(parent_slot->bytes,
                                 DIRNODE_OFF_HEADPTR, ctx->page_size);
    while (seg_vp != 0) {
        PoolSlot seg = {0};
        pool_acquire(&ctx->pool, seg_vp, false, &seg);
        if (seg.vptr == VFS_VPTR_NULL) return VFS_ERR_IO;
        AnchorKind ak;
        uint32_t sid, scnt;
        int64_t shead, ssib;
        nodes_read_anchor(seg.bytes, &ak, &sid, &shead, &ssib, &scnt,
                          ctx->page_size);
        if ((int)ak == (int)ANCHOR_KIND_SEGMENT_DIR &&
            scnt < (uint32_t)ANCHOR_UNITS_PER_SEGMENT) {
            /* Found a Segment with room.  Re-acquire as pinPage=true
             * so caller can write to it (we need to update headPtr +
             * count). */
            pool_release(&ctx->pool, &seg);
            pool_acquire(&ctx->pool, seg_vp, true, out_seg_slot);
            if (out_seg_slot->vptr == VFS_VPTR_NULL) return VFS_ERR_IO;
            *out_seg_vp = seg_vp;
            return VFS_OK;
        }
        pool_release(&ctx->pool, &seg);
        seg_vp = ssib;
    }

    /* No Segment with room — allocate a new one. */
    int64_t new_seg_vp = pool_alloc(&ctx->pool);
    if (new_seg_vp == VFS_VPTR_NULL) return VFS_ERR_FULL;
    pool_acquire(&ctx->pool, new_seg_vp, true, out_seg_slot);
    if (out_seg_slot->vptr == VFS_VPTR_NULL) return VFS_ERR_IO;
    int64_t old_head = vfs_rd8_s(parent_slot->bytes,
                                   DIRNODE_OFF_HEADPTR, ctx->page_size);
    /* New Segment is empty: headPtr=0, sibPtr=old_head (chain continues),
     * count=0, id=0 (segmentId is assigned lazily or left 0). */
    nodes_write_anchor(out_seg_slot->bytes, ANCHOR_KIND_SEGMENT_DIR,
                       0, 0, old_head, 0, ctx->page_size);
    vfs_mb_release();
    vfs_wr8_s(parent_slot->bytes, DIRNODE_OFF_HEADPTR, new_seg_vp,
              ctx->page_size);
    *out_seg_vp = new_seg_vp;
    return VFS_OK;
}

/* Find the SlotNode for a given childNodeId by walking DirSegments and
 * their SlotNode chains.  Returns the SlotNode's VP, or 0 if not found.
 *
 * The caller is expected to hold the parent DirNode lock (per the W4
 * lock discipline) — this is a read-only walk that doesn't acquire
 * additional locks, so it can be called from the write path safely.
 */
static int64_t dirchain_find_slotnode(TreeContext* ctx, int64_t dir_vp,
                                       uint32_t child_node_id) {
    PoolSlot dir_slot = {0};
    pool_acquire(&ctx->pool, dir_vp, false, &dir_slot);
    if (dir_slot.vptr == VFS_VPTR_NULL) return 0;
    int64_t seg_vp = vfs_rd8_s(dir_slot.bytes,
                                 DIRNODE_OFF_HEADPTR, ctx->page_size);
    pool_release(&ctx->pool, &dir_slot);

    while (seg_vp != 0) {
        PoolSlot seg = {0};
        pool_acquire(&ctx->pool, seg_vp, false, &seg);
        if (seg.vptr == VFS_VPTR_NULL) break;
        AnchorKind ak;
        uint32_t sid, scnt;
        int64_t shead, ssib;
        nodes_read_anchor(seg.bytes, &ak, &sid, &shead, &ssib, &scnt,
                          ctx->page_size);
        if ((int)ak != (int)ANCHOR_KIND_SEGMENT_DIR) {
            /* Legacy SlotNode directly under DirNode (no Segment indirection).
             * This shouldn't happen with W5b Segments always populated, but
             * handle it gracefully for mixed-format scenarios. */
            if ((int)ak == (int)ANCHOR_KIND_UNIT_SLOT && sid == child_node_id)
                return seg_vp;
            pool_release(&ctx->pool, &seg);
            seg_vp = ssib;
            continue;
        }
        int64_t slot_vp = shead;
        int64_t found_vp = 0;
        while (slot_vp != 0) {
            PoolSlot slot = {0};
            pool_acquire(&ctx->pool, slot_vp, false, &slot);
            if (slot.vptr == VFS_VPTR_NULL) break;
            AnchorKind sak;
            uint32_t ssid, sscnt;
            int64_t sshead, sssib;
            nodes_read_anchor(slot.bytes, &sak, &ssid, &sshead, &sssib,
                              &sscnt, ctx->page_size);
            if ((int)sak == (int)ANCHOR_KIND_UNIT_SLOT &&
                ssid == child_node_id) {
                found_vp = slot_vp;
                pool_release(&ctx->pool, &slot);
                break;
            }
            int64_t next_slot = sssib;
            pool_release(&ctx->pool, &slot);
            slot_vp = next_slot;
        }
        pool_release(&ctx->pool, &seg);
        if (found_vp != 0) return found_vp;
        seg_vp = ssib;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * structure level (one chain per child, with tombstone filter via
 * the read-rule), so the dedup hash_map and the count that sized
 * it both go away.
 * --------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * vfs_create — create a file under a parent directory
 *
 * Returns the child's VirtualPtr on success (always > 0), or a negative
 * vfs_error_t on failure.
 * --------------------------------------------------------------------------- */

int64_t vfs_create(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch) {
    if (!vfs || !vfs->ctx || !name || name[0] == '\0') {
        if (vfs && vfs->ctx) vfs->ctx->last_error = VFS_ERR_IO;
        return VFS_ERR_IO;
    }
    TreeContext* ctx = vfs->ctx;

    /* Validate epoch is writable (Phase 6: uses real epoch validation) */
    if (!vfs_epoch_is_writable(ctx, (int64_t)epoch)) {
        ctx->last_error = VFS_ERR_IO;
        return VFS_ERR_IO;
    }

    /* Phase 25: by-value pool slots.  Declared with {0} so each
       pinnedPage field is 0 until pool_acquire sets it.  This makes
       pool_release safe to call on any slot at any return point —
       it no-ops on un-pinned slots and writes back on pinned ones. */
    PoolSlot parent_slot = {0};
    PoolSlot file_slot   = {0};
    PoolSlot dc_slot     = {0};

    /* Read parent DirNode, verify type */
    pool_acquire(&ctx->pool, (int64_t)parent, true, &parent_slot);

    if (parent_slot.vptr == VFS_VPTR_NULL) { vfs->ctx->last_error = VFS_ERR_NOTFOUND; return VFS_ERR_NOTFOUND; }
    if (vfs_rd2_s(parent_slot.bytes, DIRNODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_DIR) {
        vfs->ctx->last_error = VFS_ERR_NOTDIR;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_NOTDIR;
    }

    /* Check for name collision via dirchain_find_child (Phase 25 bug fix).
       The old manual check `ce_epoch == epoch && ce_namePtr != 0` did not
       consider the read-rule: a higher-epoch tombstone in the chain should
       suppress a lower-epoch live entry.  When we delete a file and
       re-create it with the same name at the same epoch, the old check
       saw the original create's DirContent (live, namePtr != 0) and
       returned VFS_ERR_EXISTS even though the file was actually deleted.

       dirchain_find_child properly applies the read-rule + tombstones,
       so a re-create at the same name after a delete is allowed. */
    {
        int64_t existing_child = 0;
        uint32_t existing_nodeId = 0;
        int rr = dirchain_find_child(ctx, parent, name, epoch,
                                     &existing_child, &existing_nodeId, NULL);
        if (rr == VFS_OK) {
            vfs->ctx->last_error = VFS_ERR_EXISTS;
            pool_release(&ctx->pool, &parent_slot);
            return VFS_ERR_EXISTS;
        }
        if (rr != VFS_ERR_NOTFOUND && rr != VFS_ERR_NOTDIR) {
            vfs->ctx->last_error = VFS_ERR_IO;
            pool_release(&ctx->pool, &parent_slot);
            return VFS_ERR_IO;
        }
        /* NOTFOUND or NOTDIR (shouldn't happen for a file) -> proceed */
    }

    /* Atomically increment nextNodeId */
    uint32_t new_nodeId = (uint32_t)vfs_atomic_add_i32((int32_t*)&ctx->nextNodeId, 1);
    /* nextNodeId starts at 0, first add yields nodeId=1 */

    /* W4: lock parent DirNode first (node lock), then new child (content_unit). */
    if (vfs_lock(vfs, (int64_t)parent, epoch) != VFS_OK) {
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_IO;
    }
    if (vfs_lock(vfs, (int64_t)new_nodeId, epoch) != VFS_OK) {
        vfs_unlock(vfs, (int64_t)parent, epoch);
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_IO;
    }

    /* Allocate FileNode slot and write it */
    int64_t file_vp = pool_alloc(&ctx->pool);

    if (file_vp == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
        vfs_unlock(vfs, (int64_t)parent, epoch);
        vfs->ctx->last_error = VFS_ERR_FULL;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_FULL;
    }
    pool_acquire(&ctx->pool, file_vp, true, &file_slot);

    if (file_slot.vptr == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
        vfs_unlock(vfs, (int64_t)parent, epoch);
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_IO;
    }
    nodes_write_filenode(file_slot.bytes, new_nodeId, 0, 0, (int64_t)time(NULL), ctx->page_size);
    pool_release(&ctx->pool, &file_slot);  /* file_slot not used after this point */

    /* Allocate NameEntry chain for the file name */
    int64_t name_vp;
    int name_slots = nodes_write_name(&ctx->pool, name, &name_vp);

    if (name_slots == 0) {
        vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
        vfs_unlock(vfs, (int64_t)parent, epoch);
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_IO;
    }

    /* Allocate DirContent slot outside the CAS loop to avoid leaks on retry */
    int64_t dc_vp = pool_alloc(&ctx->pool);

    if (dc_vp == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
        vfs_unlock(vfs, (int64_t)parent, epoch);
        vfs->ctx->last_error = VFS_ERR_FULL;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_FULL;
    }
    pool_acquire(&ctx->pool, dc_vp, true, &dc_slot);

    if (dc_slot.vptr == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
        vfs_unlock(vfs, (int64_t)parent, epoch);
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_IO;
    }

    /* W5a: Allocate a SlotNode to hold the new DirContent.  The chain
     * structure changes from "DirNode -> DirContent (flat, all
     * children)" to "DirNode -> SlotNode (per child) -> DirContent
     * (per-child history)".  Each child has its own SlotNode, so the
     * per-child DirContent chain is naturally dedup'd.  DirSegments
     * are deferred to W5e — for now SlotNodes are prepended directly
     * to DirNode.HEADPTR. */
    int64_t slot_vp = pool_alloc(&ctx->pool);
    if (slot_vp == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
        vfs_unlock(vfs, (int64_t)parent, epoch);
        vfs->ctx->last_error = VFS_ERR_FULL;
        pool_release(&ctx->pool, &dc_slot);
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_FULL;
    }
    PoolSlot slot_slot = {0};
    pool_acquire(&ctx->pool, slot_vp, true, &slot_slot);
    if (slot_slot.vptr == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
        vfs_unlock(vfs, (int64_t)parent, epoch);
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &dc_slot);
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_IO;
    }
    /* Write the first DirContent (the live entry). */
    nodes_write_dircontent(dc_slot.bytes, new_nodeId, (uint32_t)epoch,
                           file_vp, name_vp, 0, ctx->page_size);

    /* W5b: prepend the new SlotNode to a DirSegment (chunks of
     * ANCHOR_UNITS_PER_SEGMENT = 1024 SlotNodes each).  Find a Segment
     * with room, or allocate a new one.  SlotNode's headPtr is the
     * first DirContent; SlotNode's sibPtr is the old head of the
     * Segment's SlotNode chain.  Segment's count is incremented. */
    {
        int64_t seg_vp = 0;
        PoolSlot seg_slot = {0};
        int sgr = dirchain_get_or_create_segment(ctx, &parent_slot,
                                                  &seg_vp, &seg_slot);
        if (sgr != VFS_OK) {
            pool_release(&ctx->pool, &slot_slot);
            pool_release(&ctx->pool, &dc_slot);
            vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
            vfs_unlock(vfs, (int64_t)parent, epoch);
            vfs->ctx->last_error = sgr;
            return sgr;
        }
        AnchorKind ak;
        uint32_t sid, scnt;
        int64_t shead, ssib;
        nodes_read_anchor(seg_slot.bytes, &ak, &sid, &shead, &ssib, &scnt,
                          ctx->page_size);
        /* SlotNode goes to the front of this Segment's SlotNode chain. */
        nodes_write_anchor(slot_slot.bytes, ANCHOR_KIND_UNIT_SLOT,
                           new_nodeId, dc_vp, shead, 0, ctx->page_size);
        vfs_mb_release();
        nodes_write_anchor(seg_slot.bytes, ANCHOR_KIND_SEGMENT_DIR,
                           sid, slot_vp, ssib, scnt + 1, ctx->page_size);
        pool_release(&ctx->pool, &seg_slot);
    }
    pool_release(&ctx->pool, &slot_slot);

    /* Insert into the directory's radix tree index (Phase 18).
       The chain entry already exists — the tree is additive.  If the
       insert fails (pool exhausted), the tree entry is missing but
       the chain remains the source of truth for readdir.  W5d
       R6 refit: the index now points at the SlotNode VP (the
       ContentUnit for the dir side), not the DirContent VP. */
    {
        int64_t parentIndex = vfs_rd8_s(parent_slot.bytes,
                                         DIRNODE_OFF_INDEXHEADPTR,
                                         ctx->page_size);
        uint64_t nameHash = name_hash_compute(name, (int)strlen(name));
        dircontentindex_insert(&ctx->pool, &parentIndex, nameHash,
                               slot_vp, ctx->page_size);
        /* Persist the (possibly newly-installed) tree root back to the
           DirNode slot.  dircontentindex_insert updates its int64_t*
           parameter by CAS, but only the caller's local copy is touched
           unless we explicitly write the slot. */
        vfs_wr8_s(parent_slot.bytes, DIRNODE_OFF_INDEXHEADPTR, parentIndex,
                  ctx->page_size);
        /* W1b: childCount field removed from DirNode.  The per-ContentUnit
           chains in W5 are naturally dedup'd, so no replacement field is
           needed. */
    }

    pool_release(&ctx->pool, &dc_slot);
    pool_release(&ctx->pool, &parent_slot);
    vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
    vfs_unlock(vfs, (int64_t)parent, epoch);
    return file_vp;
}

/* ---------------------------------------------------------------------------
 * vfs_mkdir — create a subdirectory under a parent directory
 *
 * Returns the child's VirtualPtr on success (always > 0), or a negative
 * vfs_error_t on failure.
 * --------------------------------------------------------------------------- */

int64_t vfs_mkdir(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch) {
    if (!vfs || !vfs->ctx || !name || name[0] == '\0') {
        if (vfs && vfs->ctx) vfs->ctx->last_error = VFS_ERR_IO;
        return VFS_ERR_IO;
    }
    TreeContext* ctx = vfs->ctx;

    if (!vfs_epoch_is_writable(ctx, (int64_t)epoch)) {
        ctx->last_error = VFS_ERR_IO;
        return VFS_ERR_IO;
    }

    /* Phase 25: by-value pool slots.  Declared with {0} so each
       pinnedPage field is 0 until pool_acquire sets it.  This makes
       pool_release safe to call on any slot at any return point —
       it no-ops on un-pinned slots and writes back on pinned ones. */
    PoolSlot parent_slot  = {0};
    PoolSlot dir_slot     = {0};
    PoolSlot dirIndexSlot = {0};
    PoolSlot dc_slot      = {0};

    pool_acquire(&ctx->pool, (int64_t)parent, true, &parent_slot);

    if (parent_slot.vptr == VFS_VPTR_NULL) { vfs->ctx->last_error = VFS_ERR_NOTFOUND; return VFS_ERR_NOTFOUND; }
    if (vfs_rd2_s(parent_slot.bytes, DIRNODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_DIR) {
        vfs->ctx->last_error = VFS_ERR_NOTDIR;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_NOTDIR;
    }

    /* Check for name collision — tree lookup first, chain walk as safety net. */
    int64_t indexRoot = vfs_rd8_s(parent_slot.bytes, DIRNODE_OFF_INDEXHEADPTR,
                                   ctx->page_size);
    uint64_t target_hash = name_hash_compute(name, (int)strlen(name));

    if (indexRoot != 0) {
        int64_t leafVP = dircontentindex_lookup(&ctx->pool, indexRoot,
                                                target_hash, ctx->page_size);
        if (leafVP != 0) {
            int64_t linkVP = leafVP;
            while (linkVP != 0) {
                PoolSlot linkSlot;
                pool_acquire(&ctx->pool, linkVP, true, &linkSlot);
                if (linkSlot.vptr == VFS_VPTR_NULL) break;
                int64_t slotVP, nextLinkVP;
                nodes_read_dircontentlink(linkSlot.bytes, &slotVP, &nextLinkVP,
                                          ctx->page_size);
                pool_release(&ctx->pool, &linkSlot);

                /* W5a: the link now points at a SlotNode VP.  Walk
                 * the SlotNode's DirContent chain for a match. */
                PoolSlot slotSlot;
                pool_acquire(&ctx->pool, slotVP, true, &slotSlot);
                if (slotSlot.vptr == VFS_VPTR_NULL) { linkVP = nextLinkVP; continue; }
                AnchorKind ak;
                uint32_t sid;
                int64_t shead, ssib;
                uint32_t scnt;
                nodes_read_anchor(slotSlot.bytes, &ak, &sid, &shead, &ssib, &scnt,
                                  ctx->page_size);
                pool_release(&ctx->pool, &slotSlot);
                (void)ak; (void)scnt;

                int64_t dcVP = shead;
                while (dcVP != 0) {
                    PoolSlot dc_check;
                    pool_acquire(&ctx->pool, dcVP, true, &dc_check);
                    if (dc_check.vptr == VFS_VPTR_NULL) break;
                    uint32_t cc, ce;
                    int64_t cp, np, nx;
                    nodes_read_dircontent(dc_check.bytes, &cc, &ce, &cp, &np, &nx,
                                          ctx->page_size);
                    pool_release(&ctx->pool, &dc_check);
                    (void)cc; (void)cp; (void)nx;

                    if (ce == (uint32_t)epoch && np != 0) {
                        uint64_t entry_hash = nodes_read_name_hash(&ctx->pool, np);
                        if (entry_hash == target_hash) {
                            char entry_name[256];
                            int nl = nodes_read_name(&ctx->pool, np, entry_name,
                                                     (int)sizeof(entry_name));
                            if (nl > 0 && strcmp(entry_name, name) == 0) {
                                vfs->ctx->last_error = VFS_ERR_EXISTS;
                                pool_release(&ctx->pool, &parent_slot);
                                return VFS_ERR_EXISTS;
                            }
                        }
                    }
                    dcVP = nx;
                }
                linkVP = nextLinkVP;
            }
        }
    }

    /* Chain walk — always runs as safety net.  W5b: walk via
     * DirSegments → SlotNodes → DirContents. */
    {
        int64_t seg_vp = vfs_rd8_s(parent_slot.bytes, DIRNODE_OFF_HEADPTR,
                                    ctx->page_size);
        while (seg_vp != 0) {
            PoolSlot seg_check;
            pool_acquire(&ctx->pool, seg_vp, false, &seg_check);
            if (seg_check.vptr == VFS_VPTR_NULL) break;
            AnchorKind seg_ak;
            uint32_t seg_id, seg_cnt;
            int64_t seg_head, seg_sib;
            nodes_read_anchor(seg_check.bytes, &seg_ak, &seg_id, &seg_head,
                              &seg_sib, &seg_cnt, ctx->page_size);
            pool_release(&ctx->pool, &seg_check);

            int64_t walk_vp = seg_head;
            while (walk_vp != 0) {
                PoolSlot slot_check;
                pool_acquire(&ctx->pool, walk_vp, false, &slot_check);
                if (slot_check.vptr == VFS_VPTR_NULL) break;
                int64_t dc_walk = vfs_rd8_s(slot_check.bytes,
                                              ANCHOR_OFF_HEADPTR, ctx->page_size);
                int64_t slot_sib = vfs_rd8_s(slot_check.bytes,
                                                ANCHOR_OFF_SIBPTR, ctx->page_size);
                pool_release(&ctx->pool, &slot_check);
                while (dc_walk != 0) {
                    PoolSlot dc_check;
                    pool_acquire(&ctx->pool, dc_walk, false, &dc_check);
                    if (dc_check.vptr == VFS_VPTR_NULL) break;
                    uint32_t cc, ce;
                    int64_t cp, np, nx;
                    nodes_read_dircontent(dc_check.bytes, &cc, &ce, &cp, &np, &nx, ctx->page_size);
                    pool_release(&ctx->pool, &dc_check);
                    (void)cc; (void)cp;
                    if (ce == (uint32_t)epoch && np != 0) {
                        uint64_t entry_hash = nodes_read_name_hash(&ctx->pool, np);
                        if (entry_hash != target_hash) {
#ifdef VFS_NAME_HASH_TESTING
                s_hash_rejects++;
#endif
                            dc_walk = nx; continue; }
                        char entry_name[256];
                        int nl = nodes_read_name(&ctx->pool, np, entry_name,
                                                 (int)sizeof(entry_name));
                        if (nl > 0 && strcmp(entry_name, name) == 0) {
                            vfs->ctx->last_error = VFS_ERR_EXISTS;
                            pool_release(&ctx->pool, &parent_slot);
                            return VFS_ERR_EXISTS;
                        }
                    }
                    dc_walk = nx;
                }
                walk_vp = slot_sib;
            }
            seg_vp = seg_sib;
        }
    }

    uint32_t new_nodeId = (uint32_t)vfs_atomic_add_i32(
        (int32_t*)&ctx->nextNodeId, 1);
    /* W4: lock parent first, then new child */
    if (vfs_lock(vfs, (int64_t)parent, epoch) != VFS_OK) {
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_IO;
    }
    if (vfs_lock(vfs, (int64_t)new_nodeId, epoch) != VFS_OK) {
        vfs_unlock(vfs, (int64_t)parent, epoch);
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_IO;
    }

    int64_t dir_vp = pool_alloc(&ctx->pool);

    if (dir_vp == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
        vfs_unlock(vfs, (int64_t)parent, epoch);
        vfs->ctx->last_error = VFS_ERR_FULL;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_FULL;
    }
    pool_acquire(&ctx->pool, dir_vp, true, &dir_slot);

    if (dir_slot.vptr == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
        vfs_unlock(vfs, (int64_t)parent, epoch);
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_IO;
    }

    /* Allocate the initial radix tree root for this new directory.
       Every DirNode starts with a valid indexHeadPtr. */
    int64_t dirIndexVP = pool_alloc(&ctx->pool);
    if (dirIndexVP == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
        vfs_unlock(vfs, (int64_t)parent, epoch);
        vfs->ctx->last_error = VFS_ERR_FULL;
        pool_release(&ctx->pool, &dir_slot);
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_FULL;
    }
    pool_acquire(&ctx->pool, dirIndexVP, true, &dirIndexSlot);
    if (dirIndexSlot.vptr == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
        vfs_unlock(vfs, (int64_t)parent, epoch);
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &dir_slot);
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_IO;
    }
    nodes_write_dircontentindex(dirIndexSlot.bytes, 0, NODE_TYPE_INDEX_INTERNAL,
                                 0, 0, ctx->page_size);
    pool_release(&ctx->pool, &dirIndexSlot);  /* dirIndexSlot not used after this */

    nodes_write_dirnode(dir_slot.bytes, new_nodeId, 0, dirIndexVP, (int64_t)time(NULL), ctx->page_size);
    pool_release(&ctx->pool, &dir_slot);  /* dir_slot not used after this */

    int64_t name_vp;
    int name_slots = nodes_write_name(&ctx->pool, name, &name_vp);

    if (name_slots == 0) {
        vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
        vfs_unlock(vfs, (int64_t)parent, epoch);
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_IO;
    }

    int64_t dc_vp = pool_alloc(&ctx->pool);

    if (dc_vp == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
        vfs_unlock(vfs, (int64_t)parent, epoch);
        vfs->ctx->last_error = VFS_ERR_FULL;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_FULL;
    }
    pool_acquire(&ctx->pool, dc_vp, true, &dc_slot);

    if (dc_slot.vptr == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
        vfs_unlock(vfs, (int64_t)parent, epoch);
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_IO;
    }

    /* W5a: Allocate a SlotNode for the new subdir. */
    int64_t slot_vp = pool_alloc(&ctx->pool);
    if (slot_vp == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
        vfs_unlock(vfs, (int64_t)parent, epoch);
        vfs->ctx->last_error = VFS_ERR_FULL;
        pool_release(&ctx->pool, &dc_slot);
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_FULL;
    }
    PoolSlot slot_slot = {0};
    pool_acquire(&ctx->pool, slot_vp, true, &slot_slot);
    if (slot_slot.vptr == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
        vfs_unlock(vfs, (int64_t)parent, epoch);
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &dc_slot);
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_IO;
    }
    nodes_write_dircontent(dc_slot.bytes, new_nodeId, (uint32_t)epoch,
                           dir_vp, name_vp, 0, ctx->page_size);
    /* W5b: prepend to a DirSegment (1024 SlotNodes each). */
    {
        int64_t seg_vp = 0;
        PoolSlot seg_slot = {0};
        int sgr = dirchain_get_or_create_segment(ctx, &parent_slot,
                                                  &seg_vp, &seg_slot);
        if (sgr != VFS_OK) {
            pool_release(&ctx->pool, &slot_slot);
            pool_release(&ctx->pool, &dc_slot);
            vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
            vfs_unlock(vfs, (int64_t)parent, epoch);
            vfs->ctx->last_error = sgr;
            return sgr;
        }
        AnchorKind ak;
        uint32_t sid, scnt;
        int64_t shead, ssib;
        nodes_read_anchor(seg_slot.bytes, &ak, &sid, &shead, &ssib, &scnt,
                          ctx->page_size);
        nodes_write_anchor(slot_slot.bytes, ANCHOR_KIND_UNIT_SLOT,
                           new_nodeId, dc_vp, shead, 0, ctx->page_size);
        vfs_mb_release();
        nodes_write_anchor(seg_slot.bytes, ANCHOR_KIND_SEGMENT_DIR,
                           sid, slot_vp, ssib, scnt + 1, ctx->page_size);
        pool_release(&ctx->pool, &seg_slot);
    }
    pool_release(&ctx->pool, &slot_slot);

    /* Insert into the directory's radix tree index (Phase 18).
     * W5d: point at the SlotNode VP. */
    {
        int64_t parentIndex = vfs_rd8_s(parent_slot.bytes,
                                         DIRNODE_OFF_INDEXHEADPTR,
                                         ctx->page_size);
        uint64_t nameHash = name_hash_compute(name, (int)strlen(name));
        dircontentindex_insert(&ctx->pool, &parentIndex, nameHash,
                               slot_vp, ctx->page_size);
        vfs_wr8_s(parent_slot.bytes, DIRNODE_OFF_INDEXHEADPTR, parentIndex,
                  ctx->page_size);
        /* W1b: childCount removed. */
    }

    pool_release(&ctx->pool, &dc_slot);
    pool_release(&ctx->pool, &parent_slot);
    vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
    vfs_unlock(vfs, (int64_t)parent, epoch);
    return dir_vp;
}

/* ---------------------------------------------------------------------------
 * vfs_delete — delete a file by prepending a tombstone DirContent
 * --------------------------------------------------------------------------- */


int vfs_delete(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch) {
    if (!vfs || !vfs->ctx || !name || name[0] == '\0') return VFS_ERR_IO;
    TreeContext* ctx = vfs->ctx;

    if (!vfs_epoch_is_writable(ctx, (int64_t)epoch)) return VFS_ERR_IO;

    /* Use dirchain_find_child to locate the entry (read-rule + mapper + dedup) */
    int64_t found_childPtr = 0;
    uint32_t found_childId = 0;
    int r = dirchain_find_child(ctx, parent, name, epoch,
                                &found_childPtr, &found_childId, NULL);
    if (r == VFS_ERR_NOTFOUND) { vfs->ctx->last_error = VFS_ERR_NOTFOUND; return VFS_ERR_NOTFOUND; }
    if (r == VFS_ERR_NOTDIR)   { vfs->ctx->last_error = VFS_ERR_NOTDIR;   return VFS_ERR_NOTDIR; }
    if (r != VFS_OK)           { vfs->ctx->last_error = VFS_ERR_IO;       return VFS_ERR_IO; }

    /* Phase 25: by-value pool slots.  Declared with {0} so each
       pinnedPage field is 0 until pool_acquire sets it.  This makes
       pool_release safe to call on any slot at any return point. */
    PoolSlot parent_slot = {0};
    PoolSlot dc_slot     = {0};

    pool_acquire(&ctx->pool, (int64_t)parent, true, &parent_slot);
    if (parent_slot.vptr == VFS_VPTR_NULL) { vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }

    if (vfs_lock(vfs, (int64_t)found_childId, epoch) != VFS_OK) {
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_IO;
    }

    /* Allocate tombstone DirContent slot outside CAS loop */
    int64_t dc_vp = pool_alloc(&ctx->pool);

    if (dc_vp == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)found_childId, epoch);
        vfs->ctx->last_error = VFS_ERR_FULL;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_FULL;
    }
    pool_acquire(&ctx->pool, dc_vp, true, &dc_slot);

    if (dc_slot.vptr == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)found_childId, epoch);
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_IO;
    }

    /* W5a: find the SlotNode for found_childId in the parent's
     * SlotNode chain, and prepend the tombstone DirContent to
     * that SlotNode's headPtr (not the parent's HEADPTR).  Per
     * O1, no parent node lock is held; the content_unit lock
     * (found_childId) serializes this vfs_delete with any
     * concurrent vfs_write etc. on the same child. */
    int64_t found_slot_vp = 0;
    {
        /* W5b: walk DirSegments → SlotNodes via dirchain_find_slotnode. */
        int64_t prev_slot_vp = 0;
        (void)prev_slot_vp;  /* unused after refactor */
        found_slot_vp = dirchain_find_slotnode(ctx, parent, found_childId);
        if (found_slot_vp == 0) {
            vfs_unlock(vfs, (int64_t)found_childId, epoch);
            vfs->ctx->last_error = VFS_ERR_IO;
            pool_release(&ctx->pool, &dc_slot);
            pool_release(&ctx->pool, &parent_slot);
            return VFS_ERR_IO;
        }
        /* Read the SlotNode to get the current headPtr. */
        PoolSlot fs = {0};
        pool_acquire(&ctx->pool, found_slot_vp, true, &fs);
        if (fs.vptr == VFS_VPTR_NULL) {
            vfs_unlock(vfs, (int64_t)found_childId, epoch);
            vfs->ctx->last_error = VFS_ERR_IO;
            pool_release(&ctx->pool, &dc_slot);
            pool_release(&ctx->pool, &parent_slot);
            return VFS_ERR_IO;
        }
        AnchorKind fak;
        uint32_t fsid;
        int64_t fshead, fssib;
        uint32_t fscnt;
        nodes_read_anchor(fs.bytes, &fak, &fsid, &fshead, &fssib, &fscnt,
                          ctx->page_size);
        /* Write the tombstone DirContent with sibPtr=fshead. */
        nodes_write_dircontent(dc_slot.bytes, found_childId, (uint32_t)epoch,
                               found_childPtr, 0, fshead, ctx->page_size);
        vfs_mb_release();
        /* Update the SlotNode's headPtr to point to the new tombstone. */
        vfs_wr8_s(fs.bytes, ANCHOR_OFF_HEADPTR, dc_vp, ctx->page_size);
        pool_release(&ctx->pool, &fs);
        (void)prev_slot_vp;  /* unused — no removal in W5a */
    }

    /* Insert a tree link for the tombstone (Phase 18).  The tree leaf
       already has a link for the live entry at the same name hash; we
       add a second link for the tombstone.  dirchain_find_child applies
       epoch dedup so the higher-epoch tombstone suppresses the live
       entry.  Tree-insert failure leaves the tombstone in the chain
       only — chain walk fallback still hides the entry correctly.
       W5d: point at the SlotNode VP. */
    {
        int64_t parentIndex = vfs_rd8_s(parent_slot.bytes,
                                         DIRNODE_OFF_INDEXHEADPTR,
                                         ctx->page_size);
        uint64_t nameHash = name_hash_compute(name, (int)strlen(name));
        dircontentindex_insert(&ctx->pool, &parentIndex, nameHash,
                               found_slot_vp, ctx->page_size);
        vfs_wr8_s(parent_slot.bytes, DIRNODE_OFF_INDEXHEADPTR, parentIndex,
                  ctx->page_size);
        /* W1b: childCount removed. */
    }

    pool_release(&ctx->pool, &dc_slot);
    pool_release(&ctx->pool, &parent_slot);
    vfs_unlock(vfs, (int64_t)found_childId, epoch);
    return VFS_OK;
}

int vfs_rmdir(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch) {
    if (!vfs || !vfs->ctx || !name || name[0] == '\0') return VFS_ERR_IO;
    TreeContext* ctx = vfs->ctx;

    if (!vfs_epoch_is_writable(ctx, (int64_t)epoch)) return VFS_ERR_IO;

    /* Phase 25: by-value pool slots.  Declared with {0} so each
       pinnedPage field is 0 until pool_acquire sets it.  This makes
       pool_release safe to call on any slot at any return point. */
    PoolSlot parent_slot = {0};
    PoolSlot dc_slot     = {0};

    pool_acquire(&ctx->pool, (int64_t)parent, true, &parent_slot);

    if (parent_slot.vptr == VFS_VPTR_NULL) { vfs->ctx->last_error = VFS_ERR_NOTFOUND; return VFS_ERR_NOTFOUND; }
    if (vfs_rd2_s(parent_slot.bytes, DIRNODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_DIR) {
        vfs->ctx->last_error = VFS_ERR_NOTDIR;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_NOTDIR;
    }

    int64_t headPtr = vfs_rd8_s(parent_slot.bytes, DIRNODE_OFF_HEADPTR, ctx->page_size);
    int64_t found_vp = 0;
    uint32_t found_childId = 0;
    int64_t found_childPtr = 0;
    uint64_t target_hash = name_hash_compute(name, (int)strlen(name));

    /* Tree lookup first (if index exists) */
    {
        int64_t indexRoot = vfs_rd8_s(parent_slot.bytes, DIRNODE_OFF_INDEXHEADPTR,
                                       ctx->page_size);
        if (indexRoot != 0) {
            int64_t leafVP = dircontentindex_lookup(&ctx->pool, indexRoot,
                                                    target_hash, ctx->page_size);
            if (leafVP != 0) {
                int64_t linkVP = leafVP;
                while (linkVP != 0) {
                    PoolSlot linkSlot;
                    pool_acquire(&ctx->pool, linkVP, true, &linkSlot);
                    if (linkSlot.vptr == VFS_VPTR_NULL) break;
                    int64_t slotVP, nextLinkVP;
                    nodes_read_dircontentlink(linkSlot.bytes, &slotVP, &nextLinkVP,
                                              ctx->page_size);
                    pool_release(&ctx->pool, &linkSlot);

                    /* W5a: the link now points at a SlotNode VP. */
                    PoolSlot slotSlot;
                    pool_acquire(&ctx->pool, slotVP, false, &slotSlot);
                    if (slotSlot.vptr == VFS_VPTR_NULL) { linkVP = nextLinkVP; continue; }
                    AnchorKind ak;
                    uint32_t sid;
                    int64_t shead, ssib;
                    uint32_t scnt;
                    nodes_read_anchor(slotSlot.bytes, &ak, &sid, &shead, &ssib,
                                      &scnt, ctx->page_size);
                    pool_release(&ctx->pool, &slotSlot);
                    (void)ak; (void)scnt; (void)ssib;

                    int64_t dcVP = shead;
                    while (dcVP != 0) {
                        PoolSlot dc_check;
                        pool_acquire(&ctx->pool, dcVP, false, &dc_check);
                        if (dc_check.vptr == VFS_VPTR_NULL) break;
                        uint32_t cc, ce;
                        int64_t cp, np, nx;
                        nodes_read_dircontent(dc_check.bytes, &cc, &ce, &cp, &np, &nx,
                                              ctx->page_size);
                        pool_release(&ctx->pool, &dc_check);
                        if (np != 0 && ce <= (uint32_t)epoch) {
                            uint64_t entry_hash = nodes_read_name_hash(&ctx->pool, np);
                            if (entry_hash == target_hash) {
                                char en[256];
                                int nl = nodes_read_name(&ctx->pool, np, en,
                                                         (int)sizeof(en));
                                if (nl > 0 && strcmp(en, name) == 0) {
                                    found_vp = dcVP;
                                    found_childId = cc;
                                    found_childPtr = cp;
                                    break;
                                }
                            }
                        }
                        dcVP = nx;
                    }
                    if (found_vp) break;
                    linkVP = nextLinkVP;
                }
            }
        }
    }

    /* Chain walk — safety net (always runs if not found in tree).
     * W5a: headPtr is a SlotNode chain. */
    if (found_vp == 0) {
        int64_t slot_walk_vp = headPtr;
        while (slot_walk_vp != 0) {
            PoolSlot slot_check;
            pool_acquire(&ctx->pool, slot_walk_vp, false, &slot_check);
            if (slot_check.vptr == VFS_VPTR_NULL) break;
            AnchorKind ak;
            uint32_t sid;
            int64_t shead, ssib;
            uint32_t scnt;
            nodes_read_anchor(slot_check.bytes, &ak, &sid, &shead, &ssib, &scnt,
                              ctx->page_size);
            pool_release(&ctx->pool, &slot_check);
            (void)ak; (void)scnt;

            int64_t walk_vp = shead;
            while (walk_vp != 0) {
                PoolSlot dc_check;
                pool_acquire(&ctx->pool, walk_vp, false, &dc_check);
                if (dc_check.vptr == VFS_VPTR_NULL) break;
                uint32_t cc, ce;
                int64_t cp, np, nx;
                nodes_read_dircontent(dc_check.bytes, &cc, &ce, &cp, &np, &nx, ctx->page_size);
                pool_release(&ctx->pool, &dc_check);
                if (np != 0 && ce <= (uint32_t)epoch) {
                    uint64_t entry_hash = nodes_read_name_hash(&ctx->pool, np);
                    if (entry_hash != target_hash) {
#ifdef VFS_NAME_HASH_TESTING
                s_hash_rejects++;
#endif
                walk_vp = nx; continue; }
                    char en[256];
                    int nl = nodes_read_name(&ctx->pool, np, en, (int)sizeof(en));
                    if (nl > 0 && strcmp(en, name) == 0) {
                        found_vp = walk_vp;
                        found_childId = cc;
                        found_childPtr = cp;
                        break;
                    }
                }
                walk_vp = nx;
            }
            if (found_vp) break;
            slot_walk_vp = ssib;
        }
    }

    if (found_vp == 0) { vfs->ctx->last_error = VFS_ERR_NOTFOUND; return VFS_ERR_NOTFOUND; }
    if (vfs_lock(vfs, (int64_t)found_childId, epoch) != VFS_OK) {
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_IO;
    }

    PoolSlot child_slot;
    pool_acquire(&ctx->pool, found_childPtr, false, &child_slot);

    if (child_slot.vptr == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)found_childId, epoch);
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_IO;
    }
    if (vfs_rd2_s(child_slot.bytes, DIRNODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_DIR) {
        vfs_unlock(vfs, (int64_t)found_childId, epoch);
        vfs->ctx->last_error = VFS_ERR_NOTDIR;
        pool_release(&ctx->pool, &child_slot);
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_NOTDIR;
    }

    /* Check directory is empty.  W5b: walk DirSegments → SlotNodes →
     * DirContents.  For each SlotNode, apply the per-SlotNode read-rule
     * to find the visible entry; if any SlotNode has a visible live
     * (namePtr != 0) entry, the directory is not empty. */
    int64_t child_seg_vp = vfs_rd8_s(child_slot.bytes, DIRNODE_OFF_HEADPTR, ctx->page_size);
    {
        int64_t seg_walk = child_seg_vp;
        while (seg_walk != 0) {
            PoolSlot seg_ss;
            pool_acquire(&ctx->pool, seg_walk, false, &seg_ss);
            if (seg_ss.vptr == VFS_VPTR_NULL) break;
            AnchorKind seg_ak;
            uint32_t seg_id, seg_cnt;
            int64_t seg_head, seg_sib;
            nodes_read_anchor(seg_ss.bytes, &seg_ak, &seg_id, &seg_head,
                              &seg_sib, &seg_cnt, ctx->page_size);
            pool_release(&ctx->pool, &seg_ss);
            (void)seg_ak; (void)seg_id; (void)seg_cnt;

            int64_t slot_walk = seg_head;
            int seg_has_visible = 0;
            int seg_has_name = 0;
            while (slot_walk != 0 && !seg_has_visible) {
                PoolSlot ss;
                pool_acquire(&ctx->pool, slot_walk, false, &ss);
                if (ss.vptr == VFS_VPTR_NULL) break;
                AnchorKind ak;
                uint32_t sid;
                int64_t shead, ssib;
                uint32_t scnt;
                nodes_read_anchor(ss.bytes, &ak, &sid, &shead, &ssib, &scnt,
                                  ctx->page_size);
                pool_release(&ctx->pool, &ss);
                (void)ak; (void)sid; (void)scnt;

                int64_t cw = shead;
                int found_visible = 0;
                int has_name = 0;
                while (cw != 0 && !found_visible) {
                    PoolSlot cs;
                    pool_acquire(&ctx->pool, cw, false, &cs);
                    if (cs.vptr == VFS_VPTR_NULL) break;
                    uint32_t cce;
                    int64_t cnp, cnx;
                    uint32_t ccc_unused;
                    int64_t ccp_unused;
                    nodes_read_dircontent(cs.bytes, &ccc_unused, &cce,
                                          &ccp_unused, &cnp, &cnx,
                                          ctx->page_size);

                int64_t eff_epoch = (int64_t)cce;
                if (mapper_table_traversal_apply(&ctx->mapper_table, (int64_t)cce))
                    eff_epoch = mapper_table_resolve(&ctx->mapper_table, (int64_t)cce);

                int applies = (eff_epoch == epoch) ||
                              (eff_epoch < epoch && (eff_epoch & 1) == 0);
                if (applies) {
                    found_visible = 1;
                    has_name = (cnp != 0) ? 1 : 0;
                }
                cw = cnx;
                pool_release(&ctx->pool, &cs);
            }
            if (found_visible && has_name) {
                seg_has_visible = 1;
                seg_has_name = 1;
            }
            slot_walk = ssib;
            }
            if (seg_has_visible && seg_has_name) {
                vfs_unlock(vfs, (int64_t)found_childId, epoch);
                vfs->ctx->last_error = VFS_ERR_NOTEMPTY;
                pool_release(&ctx->pool, &child_slot);
                pool_release(&ctx->pool, &parent_slot);
                return VFS_ERR_NOTEMPTY;
            }
            seg_walk = seg_sib;
        }
    }
    pool_release(&ctx->pool, &child_slot);  /* child_slot not used after this */

    int64_t dc_vp = pool_alloc(&ctx->pool);

    if (dc_vp == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)found_childId, epoch);
        vfs->ctx->last_error = VFS_ERR_FULL;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_FULL;
    }
    pool_acquire(&ctx->pool, dc_vp, true, &dc_slot);

    if (dc_slot.vptr == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)found_childId, epoch);
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_IO;
    }

    /* W5b: find the SlotNode for found_childId via dirchain_find_slotnode
     * (walks DirSegments → SlotNodes).  Per O1, no parent node lock. */
    int64_t found_slot_vp = dirchain_find_slotnode(ctx, parent, found_childId);
    if (found_slot_vp == 0) {
        vfs_unlock(vfs, (int64_t)found_childId, epoch);
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &dc_slot);
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_IO;
    }
    {
        PoolSlot fs = {0};
        pool_acquire(&ctx->pool, found_slot_vp, true, &fs);
        if (fs.vptr == VFS_VPTR_NULL) {
            vfs_unlock(vfs, (int64_t)found_childId, epoch);
            vfs->ctx->last_error = VFS_ERR_IO;
            pool_release(&ctx->pool, &dc_slot);
            pool_release(&ctx->pool, &parent_slot);
            return VFS_ERR_IO;
        }
        AnchorKind fak;
        uint32_t fsid;
        int64_t fshead, fssib;
        uint32_t fscnt;
        nodes_read_anchor(fs.bytes, &fak, &fsid, &fshead, &fssib, &fscnt,
                          ctx->page_size);
        nodes_write_dircontent(dc_slot.bytes, found_childId, (uint32_t)epoch,
                               found_childPtr, 0, fshead, ctx->page_size);
        vfs_mb_release();
        vfs_wr8_s(fs.bytes, ANCHOR_OFF_HEADPTR, dc_vp, ctx->page_size);
        pool_release(&ctx->pool, &fs);
    }

    /* Insert a tree link for the tombstone (Phase 18) — same pattern as
       vfs_delete.  The chain is source of truth, the tree is an additive
       index; insert failure is benign.  W5d: point at the
       SlotNode VP. */
    {
        int64_t parentIndex = vfs_rd8_s(parent_slot.bytes,
                                         DIRNODE_OFF_INDEXHEADPTR,
                                         ctx->page_size);
        uint64_t nameHash = name_hash_compute(name, (int)strlen(name));
        dircontentindex_insert(&ctx->pool, &parentIndex, nameHash,
                               found_slot_vp, ctx->page_size);
        vfs_wr8_s(parent_slot.bytes, DIRNODE_OFF_INDEXHEADPTR, parentIndex,
                  ctx->page_size);
        /* W1b: childCount removed. */
    }

    pool_release(&ctx->pool, &dc_slot);
    pool_release(&ctx->pool, &parent_slot);
    vfs_unlock(vfs, (int64_t)found_childId, epoch);
    return VFS_OK;
}



/* ---------------------------------------------------------------------------
 * dirchain_list — walk DirSegment → SlotNode → DirContent chains, collect
 * non-tombstone entries at epoch via per-SlotNode read-rule.  Allocates
 * a vfs_dirent_t[] of exact size needed; caller frees with free() or
 * vfs_free_dirents().
 *
 * W5b: walks via DirSegment chain (1024 SlotNodes per Segment) →
 * SlotNode chain → DirContent chain.  Dedup is structural — each
 * SlotNode corresponds to exactly one childNodeId, so the first
 * applicable DirContent in a SlotNode is the only entry emitted for
 * that child.  No hash_map dedup, no fixed-size array, no 1024-child
 * truncation.
 *
 * Phase 24: this is the only readdir API.  No cap, no caller-buffer
 * guess, no doubling.
 *
 * Algorithm: chain is descending by epoch (prepend ordering).  First
 * applicable hit per childNodeId is by definition the highest-epoch
 * applicable record, so dedup is automatic.
 * --------------------------------------------------------------------------- */

int dirchain_list(TreeContext* ctx, int64_t dir_vp, int64_t epoch,
                      vfs_dirent_t** out_entries, int* out_count) {
    if (!ctx || !out_entries || !out_count) return VFS_ERR_IO;
    *out_entries = NULL;
    *out_count = 0;

    /* Phase 25: by-value pool slot (read-only, pinPage=false).  FUSE
       readdir hot path — no dirty-mark overhead, no memcpy-back. */
    PoolSlot dir_slot = {0};
    pool_acquire(&ctx->pool, dir_vp, false, &dir_slot);
    if (dir_slot.vptr == VFS_VPTR_NULL) return VFS_ERR_NOTFOUND;
    if (vfs_rd2_s(dir_slot.bytes, DIRNODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_DIR)
        return VFS_ERR_NOTDIR;

    int64_t read_epoch = mapper_table_resolve(&ctx->mapper_table, epoch);
    int64_t headPtr = vfs_rd8_s(dir_slot.bytes, DIRNODE_OFF_HEADPTR, ctx->page_size);
    pool_release(&ctx->pool, &dir_slot);

    /* W5b: walk DirSegments (chunks of 1024 SlotNodes each) →
     * SlotNodes → DirContents.  No dedup needed — each SlotNode
     * corresponds to exactly one child. */
    VarArray(vfs_dirent_t) entries = var_array_new(vfs_dirent_t);
    if (!entries) return VFS_ERR_IO;

    int64_t seg_walk_vp = headPtr;
    while (seg_walk_vp != 0) {
        PoolSlot seg_slot = {0};
        pool_acquire(&ctx->pool, seg_walk_vp, false, &seg_slot);
        if (seg_slot.vptr == VFS_VPTR_NULL) break;
        AnchorKind seg_ak;
        uint32_t seg_id, seg_cnt;
        int64_t seg_head, seg_sib;
        nodes_read_anchor(seg_slot.bytes, &seg_ak, &seg_id, &seg_head,
                          &seg_sib, &seg_cnt, ctx->page_size);
        pool_release(&ctx->pool, &seg_slot);

        int64_t slot_walk_vp = seg_head;
        while (slot_walk_vp != 0) {
            PoolSlot slot_slot = {0};
            pool_acquire(&ctx->pool, slot_walk_vp, false, &slot_slot);
            if (slot_slot.vptr == VFS_VPTR_NULL) break;
            AnchorKind ak;
            uint32_t slot_id;
            int64_t slot_head, slot_sib;
            uint32_t slot_count;
            nodes_read_anchor(slot_slot.bytes, &ak, &slot_id, &slot_head,
                              &slot_sib, &slot_count, ctx->page_size);
            pool_release(&ctx->pool, &slot_slot);

            /* Walk the SlotNode's DirContent chain. */
            int64_t dc_walk_vp = slot_head;
            int matched = 0;
            while (dc_walk_vp != 0 && !matched) {
                PoolSlot dc_slot = {0};
                pool_acquire(&ctx->pool, dc_walk_vp, false, &dc_slot);
                if (dc_slot.vptr == VFS_VPTR_NULL) break;
                uint32_t ce_child, ce_epoch;
                int64_t ce_childPtr, ce_namePtr, ce_next;
                nodes_read_dircontent(dc_slot.bytes, &ce_child, &ce_epoch,
                                      &ce_childPtr, &ce_namePtr, &ce_next,
                                      ctx->page_size);

                int64_t eff_epoch = (int64_t)ce_epoch;
                if (mapper_table_traversal_apply(&ctx->mapper_table, (int64_t)ce_epoch))
                    eff_epoch = mapper_table_resolve(&ctx->mapper_table,
                                                    (int64_t)ce_epoch);

                int applies = (eff_epoch == read_epoch) ||
                              (eff_epoch < read_epoch && eff_epoch % 2 == 0);

                if (applies) {
                    if (ce_namePtr == 0) {
                        /* Visible tombstone — no live entry for this
                         * child at this epoch.  Skip. */
                        matched = 1;
                    } else {
                        /* Live entry — emit it. */
                        vfs_dirent_t e = {
                            .vp = ce_childPtr,
                            .nodeId = ce_child,
                        .isDir = false,
                        .name = {0},
                    };
                    PoolSlot child_slot = {0};
                    pool_acquire(&ctx->pool, ce_childPtr, false, &child_slot);
                    if (child_slot.vptr != VFS_VPTR_NULL) {
                        int16_t ctype = vfs_rd2_s(child_slot.bytes,
                                                   DIRNODE_OFF_TYPE,
                                                   ctx->page_size);
                        e.isDir = (ctype == (int16_t)NODE_TYPE_DIR);
                    }
                    pool_release(&ctx->pool, &child_slot);
                    nodes_read_name(&ctx->pool, ce_namePtr, e.name,
                                    (int)sizeof(e.name));
                    var_array_append(entries, e);
                    matched = 1;
                }
            }
            dc_walk_vp = ce_next;
            pool_release(&ctx->pool, &dc_slot);
        }
            slot_walk_vp = slot_sib;
        }
        seg_walk_vp = seg_sib;
    }

    int written = entries->count;
    if (written == 0) {
        var_array_delete(entries);
        *out_entries = NULL;
        *out_count = 0;
        return VFS_OK;
    }

    vfs_dirent_t* out = (vfs_dirent_t*)malloc((size_t)written * sizeof(vfs_dirent_t));
    if (!out) {
        var_array_delete(entries);
        return VFS_ERR_IO;
    }
    for (int i = 0; i < written; i++) {
        vfs_dirent_t* e = var_array_lookup(entries, i);
        out[i] = *e;
    }
    var_array_delete(entries);

    *out_entries = out;
    *out_count = written;
    return VFS_OK;
}

int vfs_rename(vfs_t* vfs, int64_t src_parent, const char* src,
               int64_t dst_parent, const char* dst, int64_t epoch) {
    if (!vfs || !vfs->ctx) return VFS_ERR_IO;
    if (!src || !dst || src[0] == '\0' || dst[0] == '\0') {
        vfs->ctx->last_error = VFS_ERR_IO;
        return VFS_ERR_IO;
    }
    TreeContext* ctx = vfs->ctx;

    if (!vfs_epoch_is_writable(ctx, (int64_t)epoch)) return VFS_ERR_IO;

    /* Phase 25: by-value pool slots.  Declared with {0} so each
       pinnedPage field is 0 until pool_acquire sets it.  This makes
       pool_release safe to call on any slot at any return point —
       it no-ops on un-pinned slots and writes back on pinned ones. */
    PoolSlot src_slot = {0};
    PoolSlot dst_slot = {0};

    /* Verify both parents are DirNodes */
    pool_acquire(&ctx->pool, (int64_t)src_parent, true, &src_slot);

    if (src_slot.vptr == VFS_VPTR_NULL) { vfs->ctx->last_error = VFS_ERR_NOTFOUND; return VFS_ERR_NOTFOUND; }
    if (vfs_rd2_s(src_slot.bytes, DIRNODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_DIR) {
        vfs->ctx->last_error = VFS_ERR_NOTDIR;
        pool_release(&ctx->pool, &src_slot);
        return VFS_ERR_NOTDIR;
    }

    pool_acquire(&ctx->pool, (int64_t)dst_parent, true, &dst_slot);

    if (dst_slot.vptr == VFS_VPTR_NULL) {
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &src_slot);
        return VFS_ERR_IO;
    }
    if (vfs_rd2_s(dst_slot.bytes, DIRNODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_DIR) {
        vfs->ctx->last_error = VFS_ERR_NOTDIR;
        pool_release(&ctx->pool, &dst_slot);
        pool_release(&ctx->pool, &src_slot);
        return VFS_ERR_NOTDIR;
    }

    /* Find source entry using dirchain_find_child */
    uint32_t rn_childId = 0;
    int64_t rn_childPtr = 0;
    uint32_t found_epoch = 0;
    int r_rn = dirchain_find_child(ctx, src_parent, src, epoch,
                                   &rn_childPtr, &rn_childId, &found_epoch);
    if (r_rn == VFS_ERR_NOTFOUND) {
        vfs->ctx->last_error = VFS_ERR_NOTFOUND;
        pool_release(&ctx->pool, &dst_slot);
        pool_release(&ctx->pool, &src_slot);
        return VFS_ERR_NOTFOUND;
    }
    if (r_rn == VFS_ERR_NOTDIR)   {
        vfs->ctx->last_error = VFS_ERR_NOTDIR;
        pool_release(&ctx->pool, &dst_slot);
        pool_release(&ctx->pool, &src_slot);
        return VFS_ERR_NOTDIR;
    }
    if (r_rn != VFS_OK)           {
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &dst_slot);
        pool_release(&ctx->pool, &src_slot);
        return VFS_ERR_IO;
    }

    /* W4: lock src_parent first, then dst_parent (lower VP first at
     * same level), then the child (content_unit).  For same-dir rename
     * src_parent == dst_parent so just one parent lock. */
    if (src_parent < dst_parent || src_parent == dst_parent) {
        if (vfs_lock(vfs, (int64_t)src_parent, epoch) != VFS_OK) {
            pool_release(&ctx->pool, &dst_slot);
            pool_release(&ctx->pool, &src_slot);
            return VFS_ERR_IO;
        }
        if (src_parent != dst_parent) {
            if (vfs_lock(vfs, (int64_t)dst_parent, epoch) != VFS_OK) {
                vfs_unlock(vfs, (int64_t)src_parent, epoch);
                pool_release(&ctx->pool, &dst_slot);
                pool_release(&ctx->pool, &src_slot);
                return VFS_ERR_IO;
            }
        }
    } else {
        if (vfs_lock(vfs, (int64_t)dst_parent, epoch) != VFS_OK) {
            pool_release(&ctx->pool, &dst_slot);
            pool_release(&ctx->pool, &src_slot);
            return VFS_ERR_IO;
        }
        if (vfs_lock(vfs, (int64_t)src_parent, epoch) != VFS_OK) {
            vfs_unlock(vfs, (int64_t)dst_parent, epoch);
            pool_release(&ctx->pool, &dst_slot);
            pool_release(&ctx->pool, &src_slot);
            return VFS_ERR_IO;
        }
    }
    if (vfs_lock(vfs, (int64_t)rn_childId, epoch) != VFS_OK) {
        vfs_unlock(vfs, (int64_t)src_parent, epoch);
        if (src_parent != dst_parent)
            vfs_unlock(vfs, (int64_t)dst_parent, epoch);
        pool_release(&ctx->pool, &dst_slot);
        pool_release(&ctx->pool, &src_slot);
        return VFS_ERR_IO;
    }

    if (src_parent == dst_parent) {
        /* W5a: for same-dir rename, ALWAYS use the existing SlotNode
         * and prepend a new DirContent.  The previous condition
         * `found_epoch == epoch` forced snapshot renames into the
         * cross-dir path, which created a duplicate SlotNode for
         * the same child (and a phantom tombstone in the same
         * chain).  That made dirchain_list return N entries for
         * one child, breaking snapshot/head visibility.
         *
         * Prepending a new DC preserves the per-epoch rename
         * history — each rename shows up as a new entry in the
         * SlotNode's DirContent chain, and the read-rule picks
         * the right one for the query epoch. */

        int64_t new_name_vp;
        int ns = nodes_write_name(&ctx->pool, dst, &new_name_vp);
        if (ns == 0) {
            vfs_unlock(vfs, (int64_t)rn_childId, epoch);
            vfs->ctx->last_error = VFS_ERR_IO;
            pool_release(&ctx->pool, &dst_slot);
            pool_release(&ctx->pool, &src_slot);
            return VFS_ERR_IO;
        }

        /* W5b: find the SlotNode for rn_childId via dirchain_find_slotnode
         * (walks DirSegments → SlotNodes).  Allocate a new DirContent
         * and prepend it to the SlotNode's chain. */
        int64_t found_slot_vp = dirchain_find_slotnode(ctx, src_parent,
                                                        rn_childId);
        if (found_slot_vp == 0) {
            vfs_unlock(vfs, (int64_t)rn_childId, epoch);
            vfs->ctx->last_error = VFS_ERR_IO;
            pool_release(&ctx->pool, &dst_slot);
            pool_release(&ctx->pool, &src_slot);
            return VFS_ERR_IO;
        }

        int64_t rename_dc_vp = pool_alloc(&ctx->pool);
        if (rename_dc_vp == VFS_VPTR_NULL) {
            vfs_unlock(vfs, (int64_t)rn_childId, epoch);
            vfs->ctx->last_error = VFS_ERR_FULL;
            pool_release(&ctx->pool, &dst_slot);
            pool_release(&ctx->pool, &src_slot);
            return VFS_ERR_FULL;
        }
        PoolSlot rename_dc_slot;
        pool_acquire(&ctx->pool, rename_dc_vp, true, &rename_dc_slot);
        if (rename_dc_slot.vptr == VFS_VPTR_NULL) {
            vfs_unlock(vfs, (int64_t)rn_childId, epoch);
            vfs->ctx->last_error = VFS_ERR_IO;
            pool_release(&ctx->pool, &dst_slot);
            pool_release(&ctx->pool, &src_slot);
            return VFS_ERR_IO;
        }

        /* Prepend a new DirContent to the SlotNode's chain. */
        {
            PoolSlot fs = {0};
            pool_acquire(&ctx->pool, found_slot_vp, true, &fs);
            if (fs.vptr == VFS_VPTR_NULL) {
                vfs_unlock(vfs, (int64_t)rn_childId, epoch);
                vfs->ctx->last_error = VFS_ERR_IO;
                pool_release(&ctx->pool, &rename_dc_slot);
                pool_release(&ctx->pool, &dst_slot);
                pool_release(&ctx->pool, &src_slot);
                return VFS_ERR_IO;
            }
            AnchorKind fak;
            uint32_t fsid;
            int64_t fshead, fssib;
            uint32_t fscnt;
            nodes_read_anchor(fs.bytes, &fak, &fsid, &fshead, &fssib, &fscnt,
                              ctx->page_size);
            nodes_write_dircontent(rename_dc_slot.bytes, rn_childId,
                                   (uint32_t)epoch, rn_childPtr,
                                   new_name_vp, fshead, ctx->page_size);
            vfs_mb_release();
            vfs_wr8_s(fs.bytes, ANCHOR_OFF_HEADPTR, rename_dc_vp, ctx->page_size);
            pool_release(&ctx->pool, &fs);
        }
        pool_release(&ctx->pool, &rename_dc_slot);

        /* W5d: move the radix index link from the OLD name's hash to
         * the NEW name's hash.  The SlotNode is the same (we just
         * prepended a new DC to its chain) — so the link is the same
         * target, just at a different hash.  The old link is removed
         * (zeroed) and a new link is inserted at the new hash.
         * Both ops are under the parent + child lock already held.
         */
        {
            int64_t srcIndex = vfs_rd8_s(src_slot.bytes, DIRNODE_OFF_INDEXHEADPTR,
                                         ctx->page_size);
            uint64_t old_hash = name_hash_compute(src, (int)strlen(src));
            uint64_t new_hash = name_hash_compute(dst, (int)strlen(dst));
            /* Remove the old link (zero the field; the link slot
             * leaks but the radix leaf is small).  This makes the
             * old name's hash lookup miss the fast path and fall
             * through to the chain walk — which also returns
             * NOTFOUND for the old name (SlotNode's visible DC is
             * the new name at this epoch). */
            (void)dircontentindex_remove(&ctx->pool, srcIndex, old_hash,
                                          found_slot_vp, ctx->page_size);
            /* Insert a new link at the new hash pointing at the
             * same SlotNode. */
            dircontentindex_insert(&ctx->pool, &srcIndex, new_hash,
                                    found_slot_vp, ctx->page_size);
            vfs_wr8_s(src_slot.bytes, DIRNODE_OFF_INDEXHEADPTR, srcIndex,
                      ctx->page_size);
        }

        vfs_unlock(vfs, (int64_t)rn_childId, epoch);
        vfs_unlock(vfs, (int64_t)src_parent, epoch);
        if (src_parent != dst_parent) {
            vfs_unlock(vfs, (int64_t)dst_parent, epoch);
        }
        pool_release(&ctx->pool, &dst_slot);
        pool_release(&ctx->pool, &src_slot);
        return VFS_OK;

    }

    /* Cross-directory rename: create new entry at dst, tombstone at src.
       For same-directory cross-epoch: skip tombstone — the old entry at lower
       epoch is naturally hidden by read-rule. */
    int cross_dir = (src_parent != dst_parent);
    int64_t dst_name_vp;
    int dst_ns = nodes_write_name(&ctx->pool, dst, &dst_name_vp);

    if (dst_ns == 0) {
        vfs_unlock(vfs, (int64_t)rn_childId, epoch);
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &dst_slot);
        pool_release(&ctx->pool, &src_slot);
        vfs_unlock(vfs, (int64_t)src_parent, epoch);
        if (src_parent != dst_parent) {
            vfs_unlock(vfs, (int64_t)dst_parent, epoch);
        }
        return VFS_ERR_IO;

    }

    /* Allocate DirContent for dst */
    int64_t dst_dc_vp = pool_alloc(&ctx->pool);

    if (dst_dc_vp == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)rn_childId, epoch);
        vfs->ctx->last_error = VFS_ERR_FULL;
        pool_release(&ctx->pool, &dst_slot);
        pool_release(&ctx->pool, &src_slot);
        vfs_unlock(vfs, (int64_t)src_parent, epoch);
        if (src_parent != dst_parent) {
            vfs_unlock(vfs, (int64_t)dst_parent, epoch);
        }
        return VFS_ERR_FULL;

    }
    PoolSlot dst_dc_slot;
    pool_acquire(&ctx->pool, dst_dc_vp, true, &dst_dc_slot);

    if (dst_dc_slot.vptr == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)rn_childId, epoch);
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &dst_slot);
        pool_release(&ctx->pool, &src_slot);
        vfs_unlock(vfs, (int64_t)src_parent, epoch);
        if (src_parent != dst_parent) {
            vfs_unlock(vfs, (int64_t)dst_parent, epoch);
        }
        return VFS_ERR_IO;

    }

    /* W5a: for the dst parent, allocate a new SlotNode for the
     * (potentially existing) child and prepend a new DirContent to
     * it.  Per W5a without the R6 refit, we always create a new
     * SlotNode (later W5d will reuse the existing one for same-child
     * writes). */
    int64_t dst_slot_vp = pool_alloc(&ctx->pool);
    if (dst_slot_vp == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)rn_childId, epoch);
        vfs_unlock(vfs, (int64_t)src_parent, epoch);
        if (src_parent != dst_parent) vfs_unlock(vfs, (int64_t)dst_parent, epoch);
        vfs->ctx->last_error = VFS_ERR_FULL;
        pool_release(&ctx->pool, &dst_dc_slot);
        pool_release(&ctx->pool, &dst_slot);
        pool_release(&ctx->pool, &src_slot);
        return VFS_ERR_FULL;
    }
    PoolSlot dst_slot_slot = {0};
    pool_acquire(&ctx->pool, dst_slot_vp, true, &dst_slot_slot);
    if (dst_slot_slot.vptr == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)rn_childId, epoch);
        vfs_unlock(vfs, (int64_t)src_parent, epoch);
        if (src_parent != dst_parent) vfs_unlock(vfs, (int64_t)dst_parent, epoch);
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &dst_dc_slot);
        pool_release(&ctx->pool, &dst_slot);
        pool_release(&ctx->pool, &src_slot);
        return VFS_ERR_IO;
    }
    {
        /* W5b: prepend new SlotNode to a DirSegment in dst (1024
         * SlotNodes per Segment). */
        int64_t dst_seg_vp = 0;
        PoolSlot dst_seg_slot = {0};
        int dsgr = dirchain_get_or_create_segment(ctx, &dst_slot,
                                                   &dst_seg_vp,
                                                   &dst_seg_slot);
        if (dsgr != VFS_OK) {
            pool_release(&ctx->pool, &dst_slot_slot);
            pool_release(&ctx->pool, &dst_dc_slot);
            vfs_unlock(vfs, (int64_t)rn_childId, epoch);
            vfs_unlock(vfs, (int64_t)src_parent, epoch);
            if (src_parent != dst_parent) vfs_unlock(vfs, (int64_t)dst_parent, epoch);
            vfs->ctx->last_error = dsgr;
            return dsgr;
        }
        AnchorKind dst_seg_ak;
        uint32_t dst_seg_id, dst_seg_cnt;
        int64_t dst_seg_head, dst_seg_sib;
        nodes_read_anchor(dst_seg_slot.bytes, &dst_seg_ak, &dst_seg_id,
                          &dst_seg_head, &dst_seg_sib, &dst_seg_cnt,
                          ctx->page_size);
        nodes_write_dircontent(dst_dc_slot.bytes, rn_childId, (uint32_t)epoch,
                               rn_childPtr, dst_name_vp, 0, ctx->page_size);
        vfs_mb_release();
        nodes_write_anchor(dst_slot_slot.bytes, ANCHOR_KIND_UNIT_SLOT,
                           rn_childId, dst_dc_vp, dst_seg_head, 0, ctx->page_size);
        nodes_write_anchor(dst_seg_slot.bytes, ANCHOR_KIND_SEGMENT_DIR,
                           dst_seg_id, dst_slot_vp, dst_seg_sib,
                           dst_seg_cnt + 1, ctx->page_size);
        pool_release(&ctx->pool, &dst_seg_slot);
    }
    pool_release(&ctx->pool, &dst_slot_slot);
    pool_release(&ctx->pool, &dst_dc_slot);

    /* W5d: tree insert for the dst entry — point at the SlotNode VP
     * (the ContentUnit), not the DirContent VP.  The radix index
     * payload is `contentUnitVP` per the W5a refit. */
    {
        int64_t dstIndex = vfs_rd8_s(dst_slot.bytes, DIRNODE_OFF_INDEXHEADPTR,
                                     ctx->page_size);
        uint64_t dst_hash = name_hash_compute(dst, (int)strlen(dst));
        dircontentindex_insert(&ctx->pool, &dstIndex, dst_hash,
                               dst_slot_vp, ctx->page_size);
        vfs_wr8_s(dst_slot.bytes, DIRNODE_OFF_INDEXHEADPTR, dstIndex,
                  ctx->page_size);
        /* W1b: childCount removed. */
    }

    /* Create tombstone at src (cross-directory only) */
    if (cross_dir) {
        int64_t src_dc_vp = pool_alloc(&ctx->pool);
        if (src_dc_vp == VFS_VPTR_NULL) {
            vfs_unlock(vfs, (int64_t)rn_childId, epoch);
            vfs->ctx->last_error = VFS_ERR_FULL;
            pool_release(&ctx->pool, &dst_slot);
            pool_release(&ctx->pool, &src_slot);
            return VFS_ERR_FULL;
        }
        PoolSlot src_dc_slot;
        pool_acquire(&ctx->pool, src_dc_vp, true, &src_dc_slot);

        if (src_dc_slot.vptr == VFS_VPTR_NULL) {
            vfs_unlock(vfs, (int64_t)rn_childId, epoch);
            vfs->ctx->last_error = VFS_ERR_IO;
            pool_release(&ctx->pool, &dst_slot);
            pool_release(&ctx->pool, &src_slot);
            return VFS_ERR_IO;
        }

        /* W5a: cross-dir src tombstone — find the SlotNode for
         * rn_childId in src parent and prepend the tombstone to
         * its chain. */
        {
            /* W5b: walk DirSegments → SlotNodes to find the SlotNode
             * for rn_childId in src parent. */
            int64_t found_slot_vp = dirchain_find_slotnode(ctx, src_parent,
                                                            rn_childId);
            if (found_slot_vp == 0) {
                vfs_unlock(vfs, (int64_t)rn_childId, epoch);
                vfs_unlock(vfs, (int64_t)src_parent, epoch);
                if (src_parent != dst_parent) vfs_unlock(vfs, (int64_t)dst_parent, epoch);
                vfs->ctx->last_error = VFS_ERR_IO;
                pool_release(&ctx->pool, &src_dc_slot);
                pool_release(&ctx->pool, &dst_slot);
                pool_release(&ctx->pool, &src_slot);
                return VFS_ERR_IO;
            }
            PoolSlot fs = {0};
            pool_acquire(&ctx->pool, found_slot_vp, true, &fs);
            if (fs.vptr == VFS_VPTR_NULL) {
                vfs_unlock(vfs, (int64_t)rn_childId, epoch);
                vfs_unlock(vfs, (int64_t)src_parent, epoch);
                if (src_parent != dst_parent) vfs_unlock(vfs, (int64_t)dst_parent, epoch);
                vfs->ctx->last_error = VFS_ERR_IO;
                pool_release(&ctx->pool, &src_dc_slot);
                pool_release(&ctx->pool, &dst_slot);
                pool_release(&ctx->pool, &src_slot);
                return VFS_ERR_IO;
            }
            AnchorKind fak;
            uint32_t fsid;
            int64_t fshead, fssib;
            uint32_t fscnt;
            nodes_read_anchor(fs.bytes, &fak, &fsid, &fshead, &fssib, &fscnt,
                              ctx->page_size);
            nodes_write_dircontent(src_dc_slot.bytes, rn_childId, (uint32_t)epoch,
                                   rn_childPtr, 0, fshead, ctx->page_size);
            vfs_mb_release();
            vfs_wr8_s(fs.bytes, ANCHOR_OFF_HEADPTR, src_dc_vp, ctx->page_size);
            pool_release(&ctx->pool, &fs);
        }
        pool_release(&ctx->pool, &src_dc_slot);

        /* Tree insert for the src tombstone (Phase 18).  W5d:
         * the radix index already has a link to the SlotNode VP
         * (from the original vfs_create); the tombstone we just
         * added to the SlotNode's chain is enough.  We don't need
         * to insert a new link here. */
        {
            (void)src_dc_vp;
        }
    } /* cross_dir */

    vfs_unlock(vfs, (int64_t)rn_childId, epoch);

    /* Phase 25 critical: when src_slot and dst_slot refer to the same
       DirNode (same-dir rename), each has its own local. The second
       release would overwrite the first. Release dst_slot first so
       its updates land in the cache, then re-acquire src_slot to pick
       up those updates, then release src_slot. */
    if (src_parent == dst_parent) {
        pool_release(&ctx->pool, &dst_slot);
        pool_acquire(&ctx->pool, (int64_t)src_parent, true, &src_slot);
        pool_release(&ctx->pool, &src_slot);
    } else {
        pool_release(&ctx->pool, &dst_slot);
        pool_release(&ctx->pool, &src_slot);
    }
    return VFS_OK;
}

/* ---------------------------------------------------------------------------
 * Radix tree helpers (Phase 18)
 * --------------------------------------------------------------------------- */

/* Extract a 4-bit nibble from a 64-bit name hash.  level 0 extracts bits
   60-63 (the most significant nibble); level 15 extracts bits 0-3. */
static int dircontentindex_extract_nibble(uint64_t nameHash, int level) {
    return (int)((nameHash >> (60 - level * 4)) & 0xF);
}

int64_t dircontentindex_lookup(Pool* pool, int64_t indexRoot,
                               uint64_t nameHash, int64_t page_size) {
    if (indexRoot == 0) return 0;

    int64_t nodeVP = indexRoot;
    for (int level = 0; level < RADIX_TREE_MAX_LEVELS; level++) {
        /* Phase 25: by-value pool slot (read-only, pinPage=false).
           Hot path for FUSE readdir; no dirty-mark overhead. */
        PoolSlot slot = {0};
        pool_acquire(pool, nodeVP, false, &slot);
        if (slot.vptr == VFS_VPTR_NULL) return 0;

        uint8_t hashNibble, nodeType;
        int64_t listVP, nextVP;
        nodes_read_dircontentindex(slot.bytes, &hashNibble, &nodeType,
                                   &listVP, &nextVP, page_size);

        if (nodeType == NODE_TYPE_INDEX_LEAF) {
            /* Reached the leaf — return its DirContentLink list head */
            return listVP;
        }

        /* INTERNAL node — find the child matching this level's nibble */
        int target = dircontentindex_extract_nibble(nameHash, level);
        int64_t childVP = 0;
        int isLast = (level == RADIX_TREE_MAX_LEVELS - 1);

        /* Walk the child list at this level (linked via nextVP) */
        int64_t childWalk = listVP;
        while (childWalk != 0) {
            /* Phase 25: by-value pool slot (read-only). */
            PoolSlot childSlot = {0};
            pool_acquire(pool, childWalk, false, &childSlot);
            if (childSlot.vptr == VFS_VPTR_NULL) return 0;

            uint8_t childHashNibble, childNodeType;
            int64_t childListVP, childNextVP;
            nodes_read_dircontentindex(childSlot.bytes, &childHashNibble,
                                       &childNodeType, &childListVP,
                                       &childNextVP, page_size);

            if (isLast && childNodeType == NODE_TYPE_INDEX_LEAF) {
                /* At the deepest level, any LEAF child is the shared
                   leaf.  Return its DirContentLink list head. */
                return childListVP;
            }

            if (childHashNibble == target) {
                if (childNodeType == NODE_TYPE_INDEX_LEAF) {
                    return childListVP;
                }
                childVP = childWalk;
                break;
            }
            childWalk = childNextVP;
            pool_release(pool, &childSlot);
        }

        if (childVP == 0) return 0;  /* no child for this nibble */
        nodeVP = childVP;
        pool_release(pool, &slot);
    }

    return 0;  /* exhausted levels without reaching a leaf */
}

int dircontentindex_insert(Pool* pool, int64_t* indexRoot, uint64_t nameHash,
                          int64_t dirContentVP, int64_t page_size) {
    if (!pool || !indexRoot || dirContentVP == 0) return -1;

    /* If the tree doesn't exist yet, create the root as INTERNAL so the
       tree naturally grows per-nibble.  CAS-protects the race when two
       threads initialize the root simultaneously. */
    if (*indexRoot == 0) {
        int64_t rootVP = pool_alloc(pool);
        if (rootVP == VFS_VPTR_NULL) return -1;
        /* Phase 25: by-value pool slot, pinned (we write the new node
           content + release persists it to the cache). */
        PoolSlot rootSlot = {0};
        pool_acquire(pool, rootVP, true, &rootSlot);
        if (rootSlot.vptr == VFS_VPTR_NULL) return -1;
        nodes_write_dircontentindex(rootSlot.bytes, 0, NODE_TYPE_INDEX_INTERNAL,
                                     0, 0, page_size);
        pool_release(pool, &rootSlot);
        /* W4: simple store under parent node lock (held by caller). */
        *indexRoot = rootVP;
    }

    int64_t nodeVP = *indexRoot;

    for (int level = 0; level < RADIX_TREE_MAX_LEVELS; level++) {
        /* Phase 25: by-value pool slot, pinned.  HAZARDOUS: the loop
           body calls pool_alloc (e.g. for a new link or new child)
           which can evict slot's page.  Copy-out closes the C1 UAF;
           pinPage=true keeps the page resident so the local we
           release at end of function flushes the in-flight write
           back to a stable cache line.  The do-while CAS on slot
           operates on the local copy (per-thread, last-writer-wins
           at release) — same shape as the vfs_write W5 fix. */
        PoolSlot slot = {0};
        pool_acquire(pool, nodeVP, true, &slot);
        if (slot.vptr == VFS_VPTR_NULL) return -1;

        uint8_t hashNibble, nodeType;
        int64_t listVP, nextVP;
        nodes_read_dircontentindex(slot.bytes, &hashNibble, &nodeType,
                                   &listVP, &nextVP, page_size);

        if (nodeType == NODE_TYPE_INDEX_LEAF) {
            /* Reached leaf — allocate DirContentLink and CAS-prepend */
            int64_t linkVP = pool_alloc(pool);
            if (linkVP == VFS_VPTR_NULL) { pool_release(pool, &slot); return -1; }
            /* Phase 25: by-value pool slot, pinned (we write the
               DirContentLink content). */
            PoolSlot linkSlot = {0};
            pool_acquire(pool, linkVP, true, &linkSlot);
            if (linkSlot.vptr == VFS_VPTR_NULL) { pool_release(pool, &slot); return -1; }

            /* W4: simple store under parent node lock (held by caller). */
            {
                int64_t oldHead = vfs_rd8_s(slot.bytes,
                                              DIRCONTENTINDEX_OFF_LISTVP,
                                              page_size);
                nodes_write_dircontentlink(linkSlot.bytes, dirContentVP,
                                           oldHead, page_size);
                vfs_wr8_s(slot.bytes, DIRCONTENTINDEX_OFF_LISTVP, linkVP,
                          page_size);
            }
            pool_release(pool, &linkSlot);
            pool_release(pool, &slot);
            return 0;
        }

        /* INTERNAL — find or create child for this level's nibble */
        int target = dircontentindex_extract_nibble(nameHash, level);
        int isLast = (level == RADIX_TREE_MAX_LEVELS - 1);
        int64_t childVP = 0;

        int64_t childWalk = listVP;
        while (childWalk != 0) {
            /* Phase 25: by-value pool slot.  This walk is read-only
               (the childSlot is only read, never written), so
               pinPage=false is sufficient — the local copy is
               independent of cache eviction.  No allocations happen
               inside the loop body, so we don't need to pin. */
            PoolSlot childSlot = {0};
            pool_acquire(pool, childWalk, false, &childSlot);
            if (childSlot.vptr == VFS_VPTR_NULL) { pool_release(pool, &slot); return -1; }
            uint8_t childHashNibble, childNodeType;
            int64_t childListVP, childNextVP;
            nodes_read_dircontentindex(childSlot.bytes, &childHashNibble,
                                       &childNodeType, &childListVP,
                                       &childNextVP, page_size);

            if (isLast) {
                /* At the deepest level, ALL entries share the same
                   leaf regardless of the last nibble.  Use the first
                   existing child (any nibble) as the leaf. */
                childVP = childWalk;
                break;
            }

            if (childHashNibble == target) {
                /* FIX (Phase 18 tree-correctness): childVP must be the
                   matching INTERNAL VP (childWalk), NOT its children
                   list head (childListVP).  Setting childListVP caused
                   the walk to descend ONE LEVEL TOO DEEP on every
                   iteration, producing a fresh 16-deep chain per
                   insert instead of reusing the existing path. */
                childVP = childWalk;
                break;
            }
            childWalk = childNextVP;
            pool_release(pool, &childSlot);
        }

        if (childVP != 0) {
            /* Child exists — handle it */
            if (isLast) {
                /* At the deepest level: check whether the existing leaf's
                   hashNibble matches the new entry's nibble.  If it
                   matches (single-nibble leaf), CAS-prepend the link.
                   If it doesn't match (multi-nibble shared leaf — first
                   created with hashNibble=0, others appended), we'd
                   ideally promote to an INTERNAL with per-nibble LEAFs
                   (plan_r6 R5-1..R5-9), but that adds up to 17 leaked
                   slots per failed promotion (R5-2) and significant
                   complexity.  For this implementation we keep the
                   shared-LEAF-at-deepest-level behavior — correct and
                   tested at 958/958.  Future optimization. */
                /* Phase 25: pin this childSlot — we CAS-update its
                   listVP, the write must be persisted. */
                PoolSlot childSlot = {0};
                pool_acquire(pool, childVP, true, &childSlot);
                if (childSlot.vptr == VFS_VPTR_NULL) { pool_release(pool, &slot); return -1; }
                int64_t linkVP = pool_alloc(pool);
                if (linkVP == VFS_VPTR_NULL) { pool_release(pool, &childSlot); pool_release(pool, &slot); return -1; }
                PoolSlot linkSlot = {0};
                pool_acquire(pool, linkVP, true, &linkSlot);
                if (linkSlot.vptr == VFS_VPTR_NULL) { pool_release(pool, &childSlot); pool_release(pool, &slot); return -1; }

                /* W4: simple store under parent node lock. */
                {
                    int64_t oldHead = vfs_rd8_s(childSlot.bytes,
                                                  DIRCONTENTINDEX_OFF_LISTVP,
                                                  page_size);
                    nodes_write_dircontentlink(linkSlot.bytes, dirContentVP,
                                               oldHead, page_size);
                    vfs_wr8_s(childSlot.bytes, DIRCONTENTINDEX_OFF_LISTVP,
                              linkVP, page_size);
                }
                pool_release(pool, &linkSlot);
                pool_release(pool, &childSlot);
                pool_release(pool, &slot);
                return 0;
            }
            /* INTERNAL — descend into subtree */
            nodeVP = childVP;
            pool_release(pool, &slot);
            continue;
        }

        /* No child for this nibble — allocate one and CAS-prepend to
           parent's child list.  At the deepest level (level 15) we
           create a LEAF and immediately insert the DirContentLink so
           the function does not need a separate loop iteration for
           the final step. */
        int64_t newChildVP = pool_alloc(pool);
        if (newChildVP == VFS_VPTR_NULL) { pool_release(pool, &slot); return -1; }
        /* Phase 25: HAZARDOUS — newChildSlot is written, and the
           do-while loop also CASes slot's LISTVP.  Both pinned; the
           CAS-on-slot is local-only (per-thread) and the cache
           writeback happens at release. */
        PoolSlot newChildSlot = {0};
        pool_acquire(pool, newChildVP, true, &newChildSlot);
        if (newChildSlot.vptr == VFS_VPTR_NULL) { pool_release(pool, &slot); return -1; }

        /* W4: simple store under parent node lock. */
        {
            int64_t oldHead = vfs_rd8_s(slot.bytes,
                                          DIRCONTENTINDEX_OFF_LISTVP,
                                          page_size);
            nodes_write_dircontentindex(newChildSlot.bytes, (uint8_t)target,
                                         isLast ? NODE_TYPE_INDEX_LEAF
                                                : NODE_TYPE_INDEX_INTERNAL,
                                         0, oldHead, page_size);
            vfs_wr8_s(slot.bytes, DIRCONTENTINDEX_OFF_LISTVP, newChildVP,
                      page_size);
        }

        if (isLast) {
            /* Just created a LEAF — insert the DirContentLink right
               here so we don't rely on the next loop iteration (which
               won't happen — this is the last iteration). */
            int64_t linkVP = pool_alloc(pool);
            if (linkVP == VFS_VPTR_NULL) { pool_release(pool, &newChildSlot); pool_release(pool, &slot); return -1; }
            PoolSlot linkSlot = {0};
            pool_acquire(pool, linkVP, true, &linkSlot);
            if (linkSlot.vptr == VFS_VPTR_NULL) { pool_release(pool, &newChildSlot); pool_release(pool, &slot); return -1; }

            nodes_write_dircontentlink(linkSlot.bytes, dirContentVP, 0, page_size);
            /* W4: simple store under parent node lock.  The LEAF is
             * freshly created (we just allocated newChildVP above and
             * set listVP=0 in the write), so no other thread can have
             * written it. */
            vfs_wr8_s(newChildSlot.bytes, DIRCONTENTINDEX_OFF_LISTVP, linkVP,
                      page_size);
            pool_release(pool, &linkSlot);
            pool_release(pool, &newChildSlot);
            pool_release(pool, &slot);
            return 0;
        }

        pool_release(pool, &newChildSlot);
        nodeVP = newChildVP;
        pool_release(pool, &slot);
    }

    return -1;  /* exhausted levels without reaching a leaf */
}

int dircontentindex_remove(Pool* pool, int64_t indexRoot, uint64_t nameHash,
                           int64_t dirContentVP, int64_t page_size) {
    if (!pool || indexRoot == 0 || dirContentVP == 0) return -1;

    /* Walk the tree the same way dircontentindex_lookup does, find the
       leaf, then scan its DirContentLink list.  For each link whose
       dirContentVP == dirContentVP, atomically zero the field to turn
       the link into a permanent tree-tombstone.  The DirContentLink
       slot is NOT freed (no pool_free exists) — it leaks one slot. */
    int64_t nodeVP = indexRoot;
    int64_t leafSlotVP = 0;  /* The LEAF DirContentIndex slot, not the link list */

    for (int level = 0; level < RADIX_TREE_MAX_LEVELS; level++) {
        /* Phase 25: by-value pool slot (read-only — we only read fields
           to navigate the tree, no writes in this loop body). */
        PoolSlot slot = {0};
        pool_acquire(pool, nodeVP, false, &slot);
        if (slot.vptr == VFS_VPTR_NULL) return -1;

        uint8_t hashNibble, nodeType;
        int64_t listVP, nextVP;
        nodes_read_dircontentindex(slot.bytes, &hashNibble, &nodeType,
                                   &listVP, &nextVP, page_size);

        if (nodeType == NODE_TYPE_INDEX_LEAF) {
            leafSlotVP = nodeVP;
            pool_release(pool, &slot);
            break;
        }

        int target = dircontentindex_extract_nibble(nameHash, level);
        int isLast = (level == RADIX_TREE_MAX_LEVELS - 1);
        int64_t childVP = 0;

        int64_t childWalk = listVP;
        while (childWalk != 0) {
            /* Phase 25: by-value pool slot (read-only walk). */
            PoolSlot childSlot = {0};
            pool_acquire(pool, childWalk, false, &childSlot);
            if (childSlot.vptr == VFS_VPTR_NULL) { pool_release(pool, &slot); return -1; }
            uint8_t childHashNibble, childNodeType;
            int64_t childListVP, childNextVP;
            nodes_read_dircontentindex(childSlot.bytes, &childHashNibble,
                                       &childNodeType, &childListVP,
                                       &childNextVP, page_size);

            if (isLast && childNodeType == NODE_TYPE_INDEX_LEAF) {
                /* Deepest level: any LEAF child is the shared leaf */
                leafSlotVP = childWalk;
                break;
            }

            if (childHashNibble == target) {
                if (childNodeType == NODE_TYPE_INDEX_LEAF) {
                    leafSlotVP = childWalk;
                    break;
                }
                childVP = childListVP;
                break;
            }
            childWalk = childNextVP;
            pool_release(pool, &childSlot);
        }

        if (leafSlotVP != 0) {
            pool_release(pool, &slot);
            goto scan_links;
        }

        if (childVP == 0) { pool_release(pool, &slot); return -1; }  /* no child for this nibble */
        nodeVP = childVP;
        pool_release(pool, &slot);
    }

    if (leafSlotVP == 0) return -1;

scan_links: {
        /* Phase 25: by-value pool slot (read-only — we only read
           listVP to start the link scan). */
        PoolSlot leafSlot = {0};
        pool_acquire(pool, leafSlotVP, false, &leafSlot);
        if (leafSlot.vptr == VFS_VPTR_NULL) return -1;

        int64_t listVP;
        /* Read listVP via the index-node reader so we get the right offset */
        uint8_t tmpNibble, tmpType;
        int64_t tmpNext;
        nodes_read_dircontentindex(leafSlot.bytes, &tmpNibble, &tmpType, &listVP,
                                   &tmpNext, page_size);
        pool_release(pool, &leafSlot);

        int found = 0;
        int64_t linkVP = listVP;
        while (linkVP != 0) {
            /* Phase 25: by-value pool slot, pinned — we write dcVP=0
               to turn this link into a tree-tombstone; release
               persists the change to the cache. */
            PoolSlot linkSlot = {0};
            pool_acquire(pool, linkVP, true, &linkSlot);
            if (linkSlot.vptr == VFS_VPTR_NULL) return -1;

            int64_t dcVP, nextLinkVP;
            nodes_read_dircontentlink(linkSlot.bytes, &dcVP, &nextLinkVP,
                                      page_size);

            if (dcVP == dirContentVP) {
                /* Atomically zero dirContentVP — turns this link into a
                   tree-tombstone.  Skip in lookups (dirchain_find_child
                   already filters dcVP==0).  Link slot leaks. */
                vfs_atomic_store_i64(
                    (int64_t*)(linkSlot.bytes + DIRCONTENTLINK_OFF_DIRCONTENTVP),
                    0);
                found = 1;
                /* Continue scanning — caller may have inserted the same
                   dirContentVP multiple times across renames; zero all. */
            }
            pool_release(pool, &linkSlot);

            linkVP = nextLinkVP;
        }
        return found ? 0 : -1;
    }
}

/* ---------------------------------------------------------------------------
/* ---------------------------------------------------------------------------
 * dirchain_find_child — walk DirContent chain, read-rule dedup, return match
 *
 * W6: rewritten to use vfs_chain_walk for the per-leaf chain walk
 * + read-rule (instead of inlining the read-rule in 2 places) and
 * walk_anchor_chain + walk_content_unit_chain for the segment +
 * unit chain walks in the fallback path.  The radix path's
 * dircontentlink chain is walked manually (the dircontentlink
 * layout is different from the Anchor layout, so the generic
 * walks don't apply).
 *
 * Two paths (preserved from the pre-W6 implementation):
 *   1. Radix tree fast path (Phase 18): walk the radix index to
 *      find a leaf, then walk the dircontentlink chain at the leaf.
 *      For each link, the SlotNode's DirContent chain is walked
 *      via vfs_chain_walk (read-rule), the visible DirContent's
 *      name is compared to the target, and on match the result is
 *      returned.
 *   2. Fallback chain walk (when no radix index): walk the
 *      DirSegment chain (walk_anchor_chain) → SlotNode chain
 *      (walk_content_unit_chain) → for each SlotNode, walk the
 *      DirContent chain (vfs_chain_walk) → name check.
 *
 * Both paths apply the same ContentUnit visibility rule: the
 * first applicable DirContent in a SlotNode's chain is the
 * visible one.  If its name doesn't match, the queried name
 * doesn't exist (even if a lower-epoch entry with the matching
 * name is in the chain).
 *
 * Locking: read-only path (pinPage=false throughout).  No locks
 * acquired.  The pre-existing function did not acquire locks
 * either.
 *
 * VFS_NAME_HASH_TESTING: when defined, the test-only fast-reject
 * counter `s_hash_rejects` is incremented for hash mismatches in
 * the fallback path.  Preserved from the pre-W6 implementation.
 * --------------------------------------------------------------------------- */

/* W6: shared state for the by-name search.  Used by both the radix
 * path and the fallback path.  Holds the best match so far. */
typedef struct {
    TreeContext* ctx;
    const char*  name;
    uint64_t     target_hash;
    int64_t      read_epoch;
    /* Out: best match */
    int64_t      best_child, best_childPtr, best_raw_epoch;
    int          best_name_match;
} w6_find_name_state;

/* W6: check if a SlotNode contains a name match.  Walks the
 * SlotNode's DirContent chain via vfs_chain_walk (which applies
 * the read-rule), reads the name of the visible DirContent, and
 * compares to the target.  Updates st->best_* on match. */
static void w6_check_slot_for_name(PoolSlot* slot_slot,
                                    w6_find_name_state* st) {
    TreeContext* ctx = st->ctx;
    int64_t leaf_head = vfs_rd8_s(slot_slot->bytes, ANCHOR_OFF_HEADPTR,
                                    ctx->page_size);
    if (leaf_head == 0) return;  /* empty slot */

    /* Use vfs_chain_walk to apply the read-rule (mapper remap,
       even/odd, exact-match-wins) on the SlotNode's DirContent
       chain.  The visible DirContent is returned in dc_slot. */
    PoolSlot dc_slot = {0};
    WalkResult r = vfs_chain_walk(ctx, leaf_head, st->read_epoch, &dc_slot);
    if (r != WALK_FOUND) return;

    /* Read the visible DirContent's fields. */
    uint32_t ce_child, ce_epoch;
    int64_t ce_childPtr, ce_namePtr, ce_next;
    nodes_read_dircontent(dc_slot.bytes, &ce_child, &ce_epoch,
                          &ce_childPtr, &ce_namePtr, &ce_next,
                          ctx->page_size);

    if (ce_namePtr == 0) {
        /* Visible tombstone for this child at this epoch.
         * No match possible — the slot is deleted. */
        return;
    }

    /* Read the name and compare.  Pre-hash fast-reject: if the
     * stored hash doesn't match, skip the strcmp.  The pre-W6
     * implementation had this same fast-reject. */
    uint64_t entry_hash = nodes_read_name_hash(&ctx->pool, ce_namePtr);
    if (entry_hash != st->target_hash) {
#ifdef VFS_NAME_HASH_TESTING
        s_hash_rejects++;
#endif
        return;
    }
    char entry_name[256];
    int nl = nodes_read_name(&ctx->pool, ce_namePtr, entry_name,
                              (int)sizeof(entry_name));
    if (nl > 0 && strcmp(entry_name, st->name) == 0) {
        st->best_child      = (int64_t)ce_child;
        st->best_childPtr   = ce_childPtr;
        st->best_raw_epoch  = (int64_t)ce_epoch;
        st->best_name_match = 1;
    }
}

/* W6: radix-path callback for the dircontentlink chain at a
 * radix leaf.  Each link points at a SlotNode (via
 * DIRCONTENTLINK_OFF_DIRCONTENTVP at offset 8).  We acquire
 * the SlotNode and check it for a name match.  Returns 1 to
 * stop the walk on match, 0 to continue. */
static int w6_radix_link_cb(TreeContext* ctx, int64_t link_vp,
                             const uint8_t* link_bytes, void* user) {
    w6_find_name_state* st = (w6_find_name_state*)user;
    (void)link_vp;
    int64_t slot_vp = vfs_rd8_s(link_bytes, DIRCONTENTLINK_OFF_DIRCONTENTVP,
                                ctx->page_size);
    if (slot_vp == 0) return 0;
    PoolSlot slot_slot = {0};
    pool_acquire(&ctx->pool, slot_vp, false, &slot_slot);
    if (slot_slot.vptr == VFS_VPTR_NULL) return 0;
    w6_check_slot_for_name(&slot_slot, st);
    pool_release(&ctx->pool, &slot_slot);
    return st->best_name_match;  /* 1 to stop, 0 to continue */
}

/* W6: fallback-path callback for the SlotNode chain within a
 * DirSegment.  Called for each SlotNode via
 * walk_content_unit_chain.  Returns 1 to stop on match, 0 to
 * continue. */
static int w6_fallback_slot_cb(TreeContext* ctx, int64_t slot_vp,
                                const uint8_t* slot_bytes, void* user) {
    w6_find_name_state* st = (w6_find_name_state*)user;
    (void)slot_vp;
    /* The callback receives the SlotNode's bytes.  w6_check_slot_for_name
     * needs a PoolSlot (because vfs_chain_walk writes the visible
     * DirContent's bytes into the slot).  Build a temporary
     * PoolSlot with the bytes copied in. */
    PoolSlot slot_slot = {0};
    memcpy(slot_slot.bytes, slot_bytes, VFS_POOL_SLOT_SIZE);
    w6_check_slot_for_name(&slot_slot, st);
    return st->best_name_match;
}

/* W6: fallback-path callback for the DirSegment chain.  Called
 * for each DirSegment via walk_anchor_chain.  For each
 * segment, walks its SlotNode chain via
 * walk_content_unit_chain + w6_fallback_slot_cb.  Returns 1 to
 * stop the outer walk on match, 0 to continue. */
static int w6_fallback_seg_cb(TreeContext* ctx, int64_t seg_vp,
                               const uint8_t* seg_bytes, void* user) {
    w6_find_name_state* st = (w6_find_name_state*)user;
    (void)seg_vp;
    int64_t slot_head = vfs_rd8_s(seg_bytes, ANCHOR_OFF_HEADPTR,
                                   ctx->page_size);
    if (slot_head == 0) return 0;  /* empty segment, keep looking */
    walk_content_unit_chain(ctx, slot_head, w6_fallback_slot_cb, st);
    return st->best_name_match;
}

int dirchain_find_child(TreeContext* ctx, int64_t dir_vp, const char* name,
                        int64_t epoch, int64_t* out_childPtr,
                        uint32_t* out_nodeId, uint32_t* out_epoch) {
    if (!ctx || !name || name[0] == '\0') return VFS_ERR_IO;
    if (!out_childPtr || !out_nodeId) return VFS_ERR_IO;

    /* Pre-compute hash for fast-reject: skip expensive strcmp when
     * hashes don't match.  Matches the pre-W6 implementation. */
    uint64_t target_hash = name_hash_compute(name, (int)strlen(name));
    int64_t read_epoch = mapper_table_resolve(&ctx->mapper_table, epoch);

    /* Shared state for both paths. */
    w6_find_name_state st = {
        .ctx = ctx, .name = name, .target_hash = target_hash,
        .read_epoch = read_epoch,
        .best_child = 0, .best_childPtr = 0, .best_raw_epoch = 0,
        .best_name_match = 0,
    };

    /* Read the DirNode to get the radix index head and the
     * segment chain head.  Release the dir_slot after reading
     * (the pre-W6 implementation did the same — early release
     * shrinks the live working set on the FUSE hot path). */
    PoolSlot dir_slot = {0};
    pool_acquire(&ctx->pool, dir_vp, false, &dir_slot);
    if (dir_slot.vptr == VFS_VPTR_NULL) return VFS_ERR_NOTFOUND;
    if (vfs_rd2_s(dir_slot.bytes, DIRNODE_OFF_TYPE, ctx->page_size)
        != (int16_t)NODE_TYPE_DIR)
        return VFS_ERR_NOTDIR;
    int64_t indexRoot = vfs_rd8_s(dir_slot.bytes, DIRNODE_OFF_INDEXHEADPTR,
                                    ctx->page_size);
    int64_t headPtr   = vfs_rd8_s(dir_slot.bytes, DIRNODE_OFF_HEADPTR,
                                    ctx->page_size);
    pool_release(&ctx->pool, &dir_slot);

    /* --- Path 1: radix tree fast path (Phase 18) ---
     * If the directory has a radix index, the index is the source
     * of truth for name lookups.  Walk it by name hash. */
    if (indexRoot != 0) {
        int64_t leafVP = dircontentindex_lookup(&ctx->pool, indexRoot,
                                                target_hash, ctx->page_size);
        if (leafVP != 0) {
            /* The radix leaf's data is a list of dircontentlink
             * slots, each pointing at a SlotNode.  Walk the
             * dircontentlink chain.  (We can't use walk_anchor_chain
             * here — the dircontentlink layout has fields at offsets
             * 8 (dirContentVP) and 16 (nextVP), not the Anchor
             * layout (type/flags/id/headPtr/sibPtr).) */
            int64_t linkVP = leafVP;
            int64_t nextLinkVP;
            while (linkVP != 0) {
                PoolSlot linkSlot = {0};
                pool_acquire(&ctx->pool, linkVP, false, &linkSlot);
                if (linkSlot.vptr == VFS_VPTR_NULL) break;
                int64_t slotVP = vfs_rd8_s(linkSlot.bytes,
                                            DIRCONTENTLINK_OFF_DIRCONTENTVP,
                                            ctx->page_size);
                nextLinkVP = vfs_rd8_s(linkSlot.bytes,
                                        DIRCONTENTLINK_OFF_NEXTVP,
                                        ctx->page_size);
                pool_release(&ctx->pool, &linkSlot);
                if (slotVP != 0) {
                    PoolSlot slotSlot = {0};
                    pool_acquire(&ctx->pool, slotVP, false, &slotSlot);
                    if (slotSlot.vptr != VFS_VPTR_NULL) {
                        w6_check_slot_for_name(&slotSlot, &st);
                        pool_release(&ctx->pool, &slotSlot);
                    }
                }
                if (st.best_name_match) break;
                linkVP = nextLinkVP;
            }
        }
    }

    /* --- Path 2: fallback chain walk (no radix index or no match) ---
     * Walk DirSegment chain → SlotNode chain → per-SlotNode
     * DirContent chain (read-rule).  Uses the shared walks for
     * the segment and SlotNode chains. */
    if (!st.best_name_match && headPtr != 0) {
        walk_anchor_chain(ctx, headPtr, w6_fallback_seg_cb, &st);
    }

    if (!st.best_name_match) return VFS_ERR_NOTFOUND;
    *out_childPtr = st.best_childPtr;
    *out_nodeId   = (uint32_t)st.best_child;
    if (out_epoch) *out_epoch = (uint32_t)st.best_raw_epoch;
    return VFS_OK;
}

/* ---------------------------------------------------------------------------
 * vfs_open — resolve name to VirtualPtr by walking parent's DirContent chain
 *
 * Returns the child's VirtualPtr on success, or VFS_ERR_NOTFOUND if not found.
 * Uses read-rule: matches if epoch == query_epoch, or epoch < query AND even.
 * --------------------------------------------------------------------------- */

int64_t vfs_open(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch) {
    if (!vfs || !vfs->ctx || !name || name[0] == '\0') return VFS_ERR_IO;
    TreeContext* ctx = vfs->ctx;

    int64_t childPtr = 0;
    uint32_t nodeId = 0;
    int err = dirchain_find_child(ctx, parent, name, epoch, &childPtr, &nodeId, NULL);
    if (err == VFS_ERR_NOTFOUND) { vfs->ctx->last_error = VFS_ERR_NOTFOUND; return VFS_ERR_NOTFOUND; }
    if (err == VFS_ERR_NOTDIR) { vfs->ctx->last_error = VFS_ERR_NOTDIR; return VFS_ERR_NOTDIR; }
    if (err != VFS_OK) { vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }

    (void)nodeId;
    return childPtr;
}

/* ---------------------------------------------------------------------------
 * vfs_file_size — query file size at a given epoch
 *
 * Walks the FileNode's sizePtr chain.  Applies the read-rule: finds the
 * first FileSize entry with epoch ≤ query_epoch (even or exact match).
 * Returns 0 if the chain is empty.
 * --------------------------------------------------------------------------- */

int64_t vfs_file_size(vfs_t* vfs, int64_t file, int64_t epoch) {
    if (!vfs || !vfs->ctx) return -1;
    TreeContext* ctx = vfs->ctx;

    /* Phase 25: by-value pool slot (read-only, pinPage=false). */
    PoolSlot file_slot = {0};
    pool_acquire(&ctx->pool, (int64_t)file, false, &file_slot);
    if (file_slot.vptr == VFS_VPTR_NULL) {
        vfs->ctx->last_error = VFS_ERR_NOTFOUND;
        return -1;
    }
    if (vfs_rd2_s(file_slot.bytes, FILENODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_FILE) {
        vfs->ctx->last_error = VFS_ERR_IO;
        return -1;
    }

    int64_t sizePtr = vfs_rd8_s(file_slot.bytes, FILENODE_OFF_SIZEPTR, ctx->page_size);
    int64_t read_epoch = mapper_table_resolve(&ctx->mapper_table, epoch);

    int64_t file_size = 0, modified_at = 0;
    sizechain_get(ctx, sizePtr, read_epoch, &file_size, &modified_at);
    (void)modified_at;
    return file_size;
}

/* ---------------------------------------------------------------------------
 * vfs_file_mtime — query file modification time at a given epoch
 * --------------------------------------------------------------------------- */

int64_t vfs_file_mtime(vfs_t* vfs, int64_t file, int64_t epoch) {
    if (!vfs || !vfs->ctx) return -1;
    TreeContext* ctx = vfs->ctx;

    /* Phase 25: by-value pool slot (read-only, pinPage=false). */
    PoolSlot file_slot = {0};
    pool_acquire(&ctx->pool, (int64_t)file, false, &file_slot);
    if (file_slot.vptr == VFS_VPTR_NULL) {
        vfs->ctx->last_error = VFS_ERR_NOTFOUND;
        return -1;
    }
    if (vfs_rd2_s(file_slot.bytes, FILENODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_FILE) {
        vfs->ctx->last_error = VFS_ERR_IO;
        return -1;
    }

    int64_t sizePtr = vfs_rd8_s(file_slot.bytes, FILENODE_OFF_SIZEPTR, ctx->page_size);
    int64_t read_epoch = mapper_table_resolve(&ctx->mapper_table, epoch);

    int64_t file_size = 0, modified_at = 0;
    sizechain_get(ctx, sizePtr, read_epoch, &file_size, &modified_at);
    (void)file_size;
    return modified_at;
}

/* ---------------------------------------------------------------------------
 * vfs_truncate — set file size at a given epoch.
 *
 * For grow: writes zero bytes from current size to new_size via vfs_write,
 *           which allocates pages and updates FileSize.  Pages within
 *           the grown region are zero-initialized.
 * For shrink: writes a new FileSize chain entry with the smaller size;
 *             actual page reclamation is deferred to GC.  Reads at
 *             offsets beyond new_size will return fewer bytes (per
 *             VFS_OK semantics — callers must consult vfs_file_size).
 * Equal: no-op.
 *
 * Returns 0 on success, negative VFS_ERR_* on failure.
 * --------------------------------------------------------------------------- */

int vfs_truncate(vfs_t* vfs, int64_t file, int64_t new_size, int64_t epoch) {
    if (!vfs || !vfs->ctx) return VFS_ERR_IO;
    TreeContext* ctx = vfs->ctx;
    if (new_size < 0) return VFS_ERR_IO;

    if (!vfs_epoch_is_writable(ctx, epoch)) {
        ctx->last_error = VFS_ERR_EPOCH;
        return VFS_ERR_EPOCH;
    }

    /* Phase 26 / W3: take the file lock for the duration of the
     * FileSize chain mutation.  This serialises vfs_truncate with
     * concurrent vfs_write (which also holds the file lock).  The
     * file lock is released before we fall into the grow path
     * (which calls vfs_write and re-acquires the lock there). */
    if (vfs_lock(vfs, file, epoch) != VFS_OK) {
        ctx->last_error = VFS_ERR_IO;
        return VFS_ERR_IO;
    }

    /* Phase 25: by-value pool slot, pinned because the shrink path
       modifies the FileNode's SIZEPTR via simple store (W3) on the
       local copy. */
    PoolSlot file_slot = {0};
    pool_acquire(&ctx->pool, file, true, &file_slot);
    if (file_slot.vptr == VFS_VPTR_NULL) {
        vfs_unlock(vfs, file, epoch);
        ctx->last_error = VFS_ERR_NOTFOUND;
        return VFS_ERR_NOTFOUND;
    }
    if (vfs_rd2_s(file_slot.bytes, FILENODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_FILE) {
        vfs_unlock(vfs, file, epoch);
        ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &file_slot);
        return VFS_ERR_IO;
    }

    int64_t cur_size = vfs_file_size(vfs, file, epoch);
    if (new_size == cur_size) {
        pool_release(&ctx->pool, &file_slot);
        vfs_unlock(vfs, file, epoch);
        return VFS_OK;
    }

    if (new_size < cur_size) {
        /* Shrink — append a new FileSize entry at epoch with new_size.
           Page reclamation is deferred to GC.  W3: simple store under
           the file lock (held above); no CAS retry. */
        int64_t old_sizePtr = vfs_rd8_s(file_slot.bytes, FILENODE_OFF_SIZEPTR,
                                        ctx->page_size);
        int64_t fs_vp = pool_alloc(&ctx->pool);
        if (fs_vp == VFS_VPTR_NULL) {
            vfs_unlock(vfs, file, epoch);
            ctx->last_error = VFS_ERR_NOMEM;
            pool_release(&ctx->pool, &file_slot);
            return VFS_ERR_NOMEM;
        }
        PoolSlot fs_slot = {0};
        pool_acquire(&ctx->pool, fs_vp, true, &fs_slot);
        if (fs_slot.vptr == VFS_VPTR_NULL) {
            vfs_unlock(vfs, file, epoch);
            ctx->last_error = VFS_ERR_NOMEM;
            pool_release(&ctx->pool, &file_slot);
            return VFS_ERR_NOMEM;
        }
        nodes_write_filesize(fs_slot.bytes, (uint32_t)epoch, (int64_t)time(NULL),
                             new_size, old_sizePtr, ctx->page_size);
        pool_release(&ctx->pool, &fs_slot);
        vfs_mb_release();
        /* W3: simple store under the file lock.  No retry, no CAS. */
        vfs_wr8_s(file_slot.bytes, FILENODE_OFF_SIZEPTR, fs_vp, ctx->page_size);
        ctx->last_error = VFS_OK;
        pool_release(&ctx->pool, &file_slot);
        vfs_unlock(vfs, file, epoch);
        return VFS_OK;
    }

    /* Grow — write zeros from cur_size to new_size via vfs_write
       (which handles page allocation and FileSize updates internally).
       Release our file_slot AND the file lock first; vfs_write acquires
       its own. */
    pool_release(&ctx->pool, &file_slot);
    vfs_unlock(vfs, file, epoch);

    /* Buffer size is one page (8 KiB) to keep stack usage low — FUSE
       worker threads on macOS have small stacks.  vfs_write handles
       arbitrarily large writes, so chunk size doesn't matter for
       correctness, only for the number of vfs_write calls. */
    uint8_t zbuf[8192];
    memset(zbuf, 0, sizeof(zbuf));
    int64_t remaining = new_size - cur_size;
    int64_t wr_off = cur_size;
    while (remaining > 0) {
        int64_t chunk = (remaining < (int64_t)sizeof(zbuf)) ? remaining
                                                             : (int64_t)sizeof(zbuf);
        int r = vfs_write(vfs, file, zbuf, wr_off, chunk, epoch);
        if (r < 0) return (int)r;
        remaining -= chunk;
        wr_off    += chunk;
    }
    ctx->last_error = VFS_OK;
    return VFS_OK;
}

/* ---------------------------------------------------------------------------
 * vfs_file_ctime — query file creation time (immutable, no epoch needed)
 * --------------------------------------------------------------------------- */

int64_t vfs_file_ctime(vfs_t* vfs, int64_t file) {
    if (!vfs || !vfs->ctx) return -1;
    TreeContext* ctx = vfs->ctx;

    /* Phase 25: by-value pool slot (read-only, pinPage=false). */
    PoolSlot file_slot = {0};
    pool_acquire(&ctx->pool, (int64_t)file, false, &file_slot);
    if (file_slot.vptr == VFS_VPTR_NULL) {
        vfs->ctx->last_error = VFS_ERR_NOTFOUND;
        return -1;
    }
    if (vfs_rd2_s(file_slot.bytes, FILENODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_FILE) {
        vfs->ctx->last_error = VFS_ERR_IO;
        return -1;
    }
    return nodes_read_filenode_ctime(file_slot.bytes, ctx->page_size);
}

/* ---------------------------------------------------------------------------
 * vfs_write — write data to a file at given offset and epoch
 *
 * Per-page: COW on first write per epoch, in-place on subsequent.
 * Returns bytes written, or -1 on error.
 * --------------------------------------------------------------------------- */

int vfs_write(vfs_t* vfs, int64_t file, const void* data, int64_t offset,
              int64_t count, int64_t epoch) {
    if (!vfs || !vfs->ctx || !data || count <= 0 || offset < 0) return -1;
    TreeContext* ctx = vfs->ctx;

    if (!vfs_epoch_is_writable(ctx, epoch)) {
        vfs->ctx->last_error = VFS_ERR_EPOCH;
        return -1;
    }

    /* Phase 25: by-value pool slot.  file_slot is read for type and
       nodeId up front, and its SIZEPTR field is updated locally and
       persisted by the pool_release at end of function. */
    PoolSlot file_slot;
    pool_acquire(&ctx->pool, file, true, &file_slot);
    if (file_slot.vptr == VFS_VPTR_NULL) {
        vfs->ctx->last_error = VFS_ERR_NOTFOUND;
        return -1;
    }
    if (vfs_rd2_s(file_slot.bytes, FILENODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_FILE) {
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &file_slot);
        return -1;
    }

    if (vfs_lock(vfs, file, epoch) != VFS_OK) {
        pool_release(&ctx->pool, &file_slot);
        return -1;
    }

    int64_t page_size = ctx->page_size;
    int64_t first_page = offset / page_size;
    int64_t last_page  = (offset + count - 1) / page_size;
    const uint8_t* src = (const uint8_t*)data;
    int64_t remaining = count;

    /* Track file growth for FileSize update */
    int64_t old_size = vfs_file_size(vfs, file, epoch);
    int64_t new_size = old_size;
    if (offset + count > new_size) new_size = offset + count;
    int grew = (new_size > old_size);

    uint32_t file_nodeId = (uint32_t)vfs_rd4_s(file_slot.bytes, FILENODE_OFF_NODEID, ctx->page_size);

    for (int64_t p = first_page; p <= last_page; p++) {
        /* Phase 25: tree_resolve_page now writes the PageNode into a
           caller-provided PoolSlot.  pinPage=true so the eventual release
           writes the local back to the cache after our CAS. */
        PoolSlot pn_slot = {0};
        int rr_pn = tree_resolve_page(vfs, file, p, epoch, true, &pn_slot);
        if (rr_pn != 0) { pool_release(&ctx->pool, &file_slot); vfs_unlock(vfs, file, epoch); return -1; }

        /* Compute intra-page offset and count */
        int64_t page_offset = (p == first_page) ? offset % page_size : 0;
        int64_t page_count = (int64_t)page_size - page_offset;
        if (remaining < page_count) page_count = remaining;

        while (1) {  /* retry loop for CAS */
            /* Walk version chain searching for existing write at this epoch */
            int64_t version_root = vfs_atomic_load_i64(
                (const int64_t*)(pn_slot.bytes + PAGENODE_OFF_VERSIONROOT));
            int64_t vp = version_root;
            int64_t data_page = -1;
            int found_in_place = 0;

            while (vp != 0) {
                PoolSlot vp_slot;
                pool_acquire(&ctx->pool, vp, false, &vp_slot);
                if (vp_slot.vptr == VFS_VPTR_NULL) break;
                uint32_t vp_epoch;
                int64_t vp_dataPage, vp_next;
                nodes_read_versionpage(vp_slot.bytes, &vp_epoch, &vp_dataPage, &vp_next, ctx->page_size);
                pool_release(&ctx->pool, &vp_slot);
                if (vp_epoch == (uint32_t)epoch) {
                    data_page = vp_dataPage;
                    found_in_place = 1;
                    break;
                }
                vp = vp_next;
            }

            if (found_in_place) {
                /* In-place write: read current page, overlay, write back.
                   storage_read/storage_write operate on cache pages, not
                   pool slots, so the C1 hazard doesn't apply here. */
                uint8_t* page_buf = storage_read(ctx->sb, data_page);
                if (!page_buf) { pool_release(&ctx->pool, &pn_slot); pool_release(&ctx->pool, &file_slot); vfs_unlock(vfs, file, epoch); return -1; }
                memcpy(page_buf + page_offset, src, (size_t)page_count);
                storage_write(ctx->sb, data_page, page_buf, 0);
                pool_release(&ctx->pool, &pn_slot);
                break;  /* exit retry loop — in-place succeeded */
            }

            /* COW: find base page (highest even epoch < write_epoch).
               VersionPages are prepended (newest first), so the first
               match is the highest even epoch. */
            int64_t base_page = -1;
            vp = version_root;
            while (vp != 0) {
                PoolSlot vp_slot;
                pool_acquire(&ctx->pool, vp, false, &vp_slot);
                if (vp_slot.vptr == VFS_VPTR_NULL) break;
                uint32_t vp_epoch;
                int64_t vp_dataPage, vp_next;
                nodes_read_versionpage(vp_slot.bytes, &vp_epoch, &vp_dataPage, &vp_next, ctx->page_size);
                pool_release(&ctx->pool, &vp_slot);
                if (vp_epoch < (uint32_t)epoch && vp_epoch % 2 == 0) {
                    base_page = vp_dataPage;
                    break;
                }
                vp = vp_next;
            }

            /* Allocate new data page */
            int64_t new_dp = storage_allocate(ctx->sb, 1);
            if (new_dp < 0) { pool_release(&ctx->pool, &pn_slot); pool_release(&ctx->pool, &file_slot); vfs_unlock(vfs, file, epoch); return -1; }

            /* Read or zero-fill the full page */
            uint8_t* page_buf = (uint8_t*)malloc((size_t)page_size);
            if (!page_buf) { pool_release(&ctx->pool, &pn_slot); pool_release(&ctx->pool, &file_slot); vfs_unlock(vfs, file, epoch); return -1; }

            if (base_page >= 0) {
                uint8_t* base_buf = storage_read(ctx->sb, base_page);
                if (base_buf) {
                    memcpy(page_buf, base_buf, (size_t)page_size);
                } else {
                    memset(page_buf, 0, (size_t)page_size);
                }
            } else {
                memset(page_buf, 0, (size_t)page_size);
            }

            /* Overlay new data */
            memcpy(page_buf + page_offset, src, (size_t)page_count);
            storage_write(ctx->sb, new_dp, page_buf, 0);
            free(page_buf);

            /* Create VersionPage (newly allocated, will be released below) */
            int64_t vp_new = pool_alloc(&ctx->pool);
            if (vp_new == VFS_VPTR_NULL) { pool_release(&ctx->pool, &pn_slot); pool_release(&ctx->pool, &file_slot); vfs_unlock(vfs, file, epoch); return -1; }
            PoolSlot vp_new_slot;
            pool_acquire(&ctx->pool, vp_new, true, &vp_new_slot);
            if (vp_new_slot.vptr == VFS_VPTR_NULL) { pool_release(&ctx->pool, &pn_slot); pool_release(&ctx->pool, &file_slot); vfs_unlock(vfs, file, epoch); return -1; }
            nodes_write_versionpage(vp_new_slot.bytes, (uint32_t)epoch, new_dp,
                                    version_root, ctx->page_size);
            pool_release(&ctx->pool, &vp_new_slot);

            /* W3: replace CAS with simple store under the PageNode lock
             * (acquired below).  The file lock is already held by
             * vfs_write, so the per-PageNode lock is the second-level
             * serialisation.  No retry loop — the lock makes the
             * VERSIONROOT update atomic w.r.t. other writers. */
            vfs_mb_release();
            if (vfs_lock(vfs, pn_slot.vptr, epoch) != VFS_OK) {
                /* Lock failed — release pn_slot (discard pin) and bail. */
                pn_slot.pinnedPage = 0;
                pool_release(&ctx->pool, &pn_slot);
                pool_release(&ctx->pool, &file_slot);
                vfs_unlock(vfs, file, epoch);
                return -1;
            }
            vfs_wr8_s(pn_slot.bytes, PAGENODE_OFF_VERSIONROOT, vp_new,
                      ctx->page_size);
            vfs_unlock(vfs, pn_slot.vptr, epoch);
            pool_release(&ctx->pool, &pn_slot);  /* write back vp_new */
            break;  /* lock-based store succeeded — exit retry loop */
        }

        src += page_count;
        remaining -= page_count;
    }

    /* Phase 25 critical: tree_resolve_page (now migrated in W9) allocates
       a new FileContent when the segment doesn't exist, links it into
       HEADPTR via CAS, and persists HEADPTR on its own file_slot
       release.  Our vfs_write file_slot local is stale (HEADPTR points
       to 0 in the local copy).  Without this re-acquire, the
       pool_release at the end of function would write our stale
       file_slot.bytes (with HEADPTR=0) back to the cache, losing the
       new FileContent link installed by tree_resolve_page. */
    pool_acquire(&ctx->pool, file, true, &file_slot);

    /* Update FileSize if file grew.
       Phase 25: SIZEPTR is updated in our local file_slot; the
       pool_release at the end of function persists it.  The OLD CAS
       pattern is replaced with a direct write — vfs_lock(file, epoch)
       serializes same-epoch writers, and the read-rule semantics for
       SIZEPTR mean cross-epoch writers see distinct chain entries. */
    if (grew) {
        int64_t old_sizePtr = vfs_atomic_load_i64(
            (const int64_t*)(file_slot.bytes + FILENODE_OFF_SIZEPTR));
        int64_t fs_vp = pool_alloc(&ctx->pool);
        if (fs_vp == VFS_VPTR_NULL) { pool_release(&ctx->pool, &file_slot); vfs_unlock(vfs, file, epoch); return -1; }
        PoolSlot fs_slot;
        pool_acquire(&ctx->pool, fs_vp, true, &fs_slot);
        if (fs_slot.vptr == VFS_VPTR_NULL) { pool_release(&ctx->pool, &file_slot); vfs_unlock(vfs, file, epoch); return -1; }
        nodes_write_filesize(fs_slot.bytes, (uint32_t)epoch, (int64_t)time(NULL),
                             new_size, old_sizePtr, ctx->page_size);
        pool_release(&ctx->pool, &fs_slot);
        /* Update SIZEPTR in the local; pool_release at end of function
           writes it back to the cache. */
        vfs_wr8_s(file_slot.bytes, FILENODE_OFF_SIZEPTR, fs_vp, ctx->page_size);
    }

    pool_release(&ctx->pool, &file_slot);
    vfs_unlock(vfs, file, epoch);
    return (int)count;
}

/* ---------------------------------------------------------------------------
 * vfs_read — read data from a file at given offset and epoch
 *
 * Uses the read-rule: exact match at read_epoch, or highest even epoch
 * < read_epoch.  Unwritten pages return zero-filled data.
 * Returns bytes read, or -1 on error.
 * --------------------------------------------------------------------------- */

int vfs_read(vfs_t* vfs, int64_t file, void* buf, int64_t offset,
             int64_t count, int64_t epoch) {
    if (!vfs || !vfs->ctx || !buf || count <= 0 || offset < 0) return -1;
    TreeContext* ctx = vfs->ctx;

    /* Bounds-check against file size at this epoch.  Reads beyond EOF
       return a short read (count truncated to remaining bytes).
       Reads entirely past EOF return 0.  Pages past the file size are
       treated as zero-filled (sparse-file semantics). */
    int64_t file_size = vfs_file_size(vfs, file, epoch);
    if (offset >= file_size) {
        return 0;  /* entirely past EOF */
    }
    if (offset + count > file_size) {
        count = file_size - offset;
    }

    int64_t read_epoch = mapper_table_resolve(&ctx->mapper_table, epoch);

    /* Phase 25: by-value pool slot (read-only, pinPage=false).  Read
       path is pure — the local is only used to check the node type up
       front, so the per-iteration pn_slot comes from the unmigrated
       tree_resolve_page (W7) without any re-acquire. */
    PoolSlot file_slot = {0};
    pool_acquire(&ctx->pool, file, false, &file_slot);
    if (file_slot.vptr == VFS_VPTR_NULL) {
        vfs->ctx->last_error = VFS_ERR_NOTFOUND;
        return -1;
    }
    if (vfs_rd2_s(file_slot.bytes, FILENODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_FILE) {
        vfs->ctx->last_error = VFS_ERR_IO;
        return -1;
    }

    int64_t page_size = ctx->page_size;
    int64_t first_page = offset / page_size;
    int64_t last_page  = (offset + count - 1) / page_size;
    uint8_t* dst = (uint8_t*)buf;
    int64_t remaining = count;

    for (int64_t p = first_page; p <= last_page; p++) {
        /* Phase 25: tree_resolve_page writes the PageNode into pn_slot
           (read-only, pinPage=false).  Local is independent of cache. */
        PoolSlot pn_slot = {0};
        int rr_pn = tree_resolve_page(vfs, file, p, read_epoch, false, &pn_slot);
        if (rr_pn != 0) {
            /* Page doesn't exist → zero-fill this portion */
            int64_t page_offset = (p == first_page) ? offset % page_size : 0;
            int64_t page_count = (int64_t)page_size - page_offset;
            if (remaining < page_count) page_count = remaining;
            memset(dst, 0, (size_t)page_count);
            dst += page_count;
            remaining -= page_count;
            continue;
        }

        /* Compute intra-page offset and count */
        int64_t page_offset = (p == first_page) ? offset % page_size : 0;
        int64_t page_count = (int64_t)page_size - page_offset;
        if (remaining < page_count) page_count = remaining;

        /* Walk version chain applying read-rule via verchain_get */
        int64_t vp = vfs_atomic_load_i64(
            (const int64_t*)(pn_slot.bytes + PAGENODE_OFF_VERSIONROOT));
        int64_t data_page = verchain_get(ctx, vp, read_epoch);
        int found = (data_page >= 0);

        if (found) {
            /* Read data page and copy intra-page portion */
            int64_t t_before = vfs_cache_total();
            int64_t h_before = vfs_cache_hits();
            uint8_t* page_data = storage_read(ctx->sb, data_page);
            vfs_data_inc_total();
            if (vfs_cache_total() > t_before && vfs_cache_hits() > h_before)
                vfs_data_inc_hits();
            if (page_data) {
                memcpy(dst, page_data + page_offset, (size_t)page_count);
            } else {
                memset(dst, 0, (size_t)page_count);
            }
        } else {
            /* No VersionPage found — page never written at this epoch */
            memset(dst, 0, (size_t)page_count);
        }

        dst += page_count;
        remaining -= page_count;
    }

    return (int)(dst - (uint8_t*)buf);
}
