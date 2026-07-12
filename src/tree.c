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
 * Walks the FileContent chain to find the segment containing this page.
 * Creates missing FileContent + PageNode entries on file growth.
 * Builds the in-memory VirtualPtr array on first access to a segment.
 *
 * Writes the PageNode slot to *out (Phase 25 by-value copy-out).  Returns 0
 * on success, -1 on error or page-not-found.  Closes the C1 hazard: the
 * caller's slot is a stack-local copy independent of the cache, so a
 * later cache eviction cannot invalidate it.
 * --------------------------------------------------------------------------- */

int tree_resolve_page(TreeContext* ctx, int64_t file_vp,
                      int64_t logical_page, int64_t epoch, bool is_write,
                      PoolSlot* out) {
    (void)epoch;  /* not yet used — future: segment growth decisions */
    if (!out) return -1;
    out->vptr = VFS_VPTR_NULL;
    out->pinnedPage = 0;
    memset(out->bytes, 0, VFS_POOL_SLOT_SIZE);

    uint32_t seg_size = ctx->segment_size;
    int64_t segment_idx = logical_page / seg_size;
    int64_t page_in_segment = logical_page % seg_size;

    /* Read FileNode to get headPtr (first FileContent) */
    PoolSlot file_slot = {0};
    pool_acquire(&ctx->pool, file_vp, true, &file_slot);
    if (file_slot.vptr == VFS_VPTR_NULL) return -1;

    int64_t fc_vp;   /* VirtualPtr to current FileContent */

    uint32_t tmp_nodeId;
    int64_t tmp_headPtr, tmp_sizePtr, tmp_createdAt;

    nodes_read_filenode(file_slot.bytes, &tmp_nodeId, &tmp_headPtr, &tmp_sizePtr, &tmp_createdAt, ctx->page_size);
    fc_vp = tmp_headPtr;

    /* Walk FileContent chain to find the target segment */
    int64_t prev_fc_vp = 0;  /* previous FileContent's VirtualPtr, for linking */

    /* Use a single accumulator slot for the function's return value; we'll
       re-acquire to *out at the end.  Internal loop work uses temporary
       PoolSlot locals. */
    int64_t result_vp = 0;
    int rc = -1;

    for (int64_t i = 0; i <= segment_idx; i++) {
        if (fc_vp == VFS_VPTR_NULL) {
            /* Segment doesn't exist yet.  If this is the target segment and
             * the caller is writing, allocate FileContent + one PageNode.
             * Otherwise allocate an empty FileContent (or return -1). */
            if (i == segment_idx && !is_write) { pool_release(&ctx->pool, &file_slot); return -1; }

            int64_t page_root_vp = VFS_VPTR_NULL;
            if (i == segment_idx && is_write) {
                /* Lazy: allocate exactly one PageNode for the requested page */
                int64_t pn_vp = pool_alloc(&ctx->pool);
                if (pn_vp == VFS_VPTR_NULL) { pool_release(&ctx->pool, &file_slot); return -1; }
                PoolSlot pn_slot = {0};
                pool_acquire(&ctx->pool, pn_vp, true, &pn_slot);
                if (pn_slot.vptr == VFS_VPTR_NULL) { pool_release(&ctx->pool, &file_slot); return -1; }
                nodes_write_pagenode(pn_slot.bytes, 0, 0, (uint32_t)page_in_segment, ctx->page_size);
                pool_release(&ctx->pool, &pn_slot);
                page_root_vp = pn_vp;
            }
            /* else: empty segment (page_root_vp stays 0) */

            /* Allocate FileContent entry */
            int64_t new_fc_vp = pool_alloc(&ctx->pool);
            if (new_fc_vp == VFS_VPTR_NULL) { pool_release(&ctx->pool, &file_slot); return -1; }
            PoolSlot fc_slot = {0};
            pool_acquire(&ctx->pool, new_fc_vp, true, &fc_slot);
            if (fc_slot.vptr == VFS_VPTR_NULL) { pool_release(&ctx->pool, &file_slot); return -1; }
            nodes_write_filecontent(fc_slot.bytes, page_root_vp, 0, ctx->page_size);

            /* CAS-link into chain with release barrier */
            vfs_mb_release();
            if (i == 0) {
                int64_t expected = 0;
                int64_t desired = new_fc_vp;
                int64_t old = vfs_cas_i64(
                    (int64_t*)(file_slot.bytes + FILENODE_OFF_HEADPTR),
                    expected, desired);
                if (old != expected) {
                    pool_release(&ctx->pool, &fc_slot);
                    fc_vp = old;
                    i--;  /* retry this segment */
                    continue;
                }
                fc_vp = new_fc_vp;
            } else {
                PoolSlot prev_slot = {0};
                pool_acquire(&ctx->pool, prev_fc_vp, true, &prev_slot);
                if (prev_slot.vptr != VFS_VPTR_NULL) {
                    int64_t expected = 0;
                    int64_t desired = new_fc_vp;
                    int64_t off = FILECONTENT_OFF_NEXTPTR;
                    int64_t old = vfs_cas_i64(
                        (int64_t*)(prev_slot.bytes + off), expected, desired);
                    pool_release(&ctx->pool, &prev_slot);
                    if (old != expected) {
                        pool_release(&ctx->pool, &fc_slot);
                        fc_vp = old;
                        i--;  /* retry this segment */
                        continue;
                    }
                } else {
                    pool_release(&ctx->pool, &prev_slot);
                }
                fc_vp = new_fc_vp;
            }
            if (i == segment_idx && is_write) {
                vfs_atomic_add_i32((int32_t*)(fc_slot.bytes + FILECONTENT_OFF_PAGECOUNT), 1);
                pool_release(&ctx->pool, &fc_slot);
                result_vp = page_root_vp;
                rc = 0;
                break;
            }
            pool_release(&ctx->pool, &fc_slot);
            /* Empty intermediate segment — advance to next */
            prev_fc_vp = fc_vp;
            fc_vp = 0;  /* next segment doesn't exist yet */
            continue;
        }

        PoolSlot fc_slot = {0};
        pool_acquire(&ctx->pool, fc_vp, true, &fc_slot);
        if (fc_slot.vptr == VFS_VPTR_NULL) { pool_release(&ctx->pool, &file_slot); return -1; }

        if (i == segment_idx) {
            /* Per-thread segment cache — rebuilt when sparse chain exceeds threshold.
             * Entries track gc_generation to auto-invalidate after GC compaction. */
            #define TCACHE_SIZE 16
            static __thread struct {
                int64_t      key;
                SegmentArray arr;
                bool         populated;
                int64_t      gen;
            } tcache[TCACHE_SIZE];
            static __thread int tcache_next = 0;

            int64_t fc_page_root = vfs_rd8_s(fc_slot.bytes, FILECONTENT_OFF_ROOTPTR, ctx->page_size);
            uint32_t fc_page_count = (uint32_t)vfs_rd4_s(fc_slot.bytes, FILECONTENT_OFF_PAGECOUNT, ctx->page_size);
            int64_t cache_key = (file_vp << 20) | (segment_idx & 0xFFFFF);

            /* Check tcache first */
            int cache_slot = -1;
            int tcache_hit = 0;
            for (int ci = 0; ci < TCACHE_SIZE; ci++) {
                if (tcache[ci].key == cache_key && tcache[ci].populated) {
                    if (tcache[ci].gen != vfs_atomic_load_i64(&ctx->gc_generation)) {
                        tcache[ci].populated = false;
                    } else {
                        if (segment_array_resolve(&ctx->pool, &tcache[ci].arr,
                                                  (uint32_t)page_in_segment, out)) {
                            /* Cache hit — *out has the PageNode bytes. */
                            tcache_hit = 1;
                            break;
                        }
                        /* Array has NULL for this page — don't invalidate the
                           whole array.  Fall through to chain walk to find or
                           create the PageNode, then patch the array entry. */
                        cache_slot = ci;
                    }
                    break;
                }
            }
            if (tcache_hit) {
                pool_release(&ctx->pool, &fc_slot);
                pool_release(&ctx->pool, &file_slot);
                return 0;
            }

            /* Walk the sparse PageNode chain in ascending page_index order.
             * Track prev_vp for sorted insertion. */
            int64_t pn_vp = fc_page_root;
            int64_t prev_vp = 0;
            int total_pages_seen = 0;
#ifndef NDEBUG
            int64_t prev_page_index = -1;
#endif
            while (pn_vp != 0) {
                total_pages_seen++;
                PoolSlot pn_slot = {0};
                pool_acquire(&ctx->pool, pn_vp, true, &pn_slot);
                if (pn_slot.vptr == VFS_VPTR_NULL) break;
                uint32_t pn_idx;
                int64_t pn_next;
                int64_t pn_ver_root;
                nodes_read_pagenode(pn_slot.bytes, &pn_ver_root, &pn_next, &pn_idx, ctx->page_size);
                (void)pn_ver_root;
                pool_release(&ctx->pool, &pn_slot);

#ifndef NDEBUG
                assert(prev_page_index < (int64_t)pn_idx);
                prev_page_index = (int64_t)pn_idx;
#endif

                if ((int64_t)pn_idx == page_in_segment) {
                    result_vp = pn_vp;
                    rc = 0;
                    goto done;
                }

                if ((int64_t)pn_idx > page_in_segment) {
                    /* Found a higher-index node — insert before it */
                    if (!is_write) { pool_release(&ctx->pool, &fc_slot); pool_release(&ctx->pool, &file_slot); return -1; }

                    int64_t new_pn_vp = pool_alloc(&ctx->pool);
                    if (new_pn_vp == VFS_VPTR_NULL) { pool_release(&ctx->pool, &fc_slot); pool_release(&ctx->pool, &file_slot); return -1; }
                    PoolSlot new_slot = {0};
                    pool_acquire(&ctx->pool, new_pn_vp, true, &new_slot);
                    if (new_slot.vptr == VFS_VPTR_NULL) { pool_release(&ctx->pool, &fc_slot); pool_release(&ctx->pool, &file_slot); return -1; }
                    nodes_write_pagenode(new_slot.bytes, 0, pn_vp,
                                         (uint32_t)page_in_segment, ctx->page_size);
                    pool_release(&ctx->pool, &new_slot);
                    vfs_mb_release();

                    if (prev_vp == 0) {
                        int64_t old_root = vfs_cas_i64(
                            (int64_t*)(fc_slot.bytes + FILECONTENT_OFF_ROOTPTR),
                            pn_vp, new_pn_vp);
                        if (old_root != pn_vp) {
                            fc_page_root = old_root;
                            goto retry_walk;
                        }
                    } else {
                        PoolSlot prev_slot = {0};
                        pool_acquire(&ctx->pool, prev_vp, true, &prev_slot);
                        if (prev_slot.vptr == VFS_VPTR_NULL) { pool_release(&ctx->pool, &fc_slot); pool_release(&ctx->pool, &file_slot); return -1; }
                        int64_t old_next = vfs_cas_i64(
                            (int64_t*)(prev_slot.bytes + PAGENODE_OFF_NEXTPTR),
                            pn_vp, new_pn_vp);
                        pool_release(&ctx->pool, &prev_slot);
                        if (old_next != pn_vp) {
                            fc_page_root = vfs_rd8_s(fc_slot.bytes,
                                FILECONTENT_OFF_ROOTPTR, ctx->page_size);
                            goto retry_walk;
                        }
                    }
                    result_vp = new_pn_vp;
                    vfs_atomic_add_i32((int32_t*)(fc_slot.bytes + FILECONTENT_OFF_PAGECOUNT), 1);
                    rc = 0;
                    goto done;
                }

                prev_vp = pn_vp;
                pn_vp = pn_next;
            }

            /* Chain ended without finding the page */
            if (!is_write) { pool_release(&ctx->pool, &fc_slot); pool_release(&ctx->pool, &file_slot); return -1; }

            /* Append at tail */
            {
                int64_t new_pn_vp = pool_alloc(&ctx->pool);
                if (new_pn_vp == VFS_VPTR_NULL) { pool_release(&ctx->pool, &fc_slot); pool_release(&ctx->pool, &file_slot); return -1; }
                PoolSlot new_slot = {0};
                pool_acquire(&ctx->pool, new_pn_vp, true, &new_slot);
                if (new_slot.vptr == VFS_VPTR_NULL) { pool_release(&ctx->pool, &fc_slot); pool_release(&ctx->pool, &file_slot); return -1; }
                nodes_write_pagenode(new_slot.bytes, 0, 0,
                                     (uint32_t)page_in_segment, ctx->page_size);
                pool_release(&ctx->pool, &new_slot);
                vfs_mb_release();

                if (prev_vp == 0) {
                    int64_t old_root = vfs_cas_i64(
                        (int64_t*)(fc_slot.bytes + FILECONTENT_OFF_ROOTPTR),
                        0, new_pn_vp);
                    if (old_root != 0) {
                        fc_page_root = old_root;
                        goto retry_walk;
                    }
                } else {
                    PoolSlot tail_slot = {0};
                    pool_acquire(&ctx->pool, prev_vp, true, &tail_slot);
                    if (tail_slot.vptr == VFS_VPTR_NULL) { pool_release(&ctx->pool, &fc_slot); pool_release(&ctx->pool, &file_slot); return -1; }
                    int64_t old_next = vfs_cas_i64(
                        (int64_t*)(tail_slot.bytes + PAGENODE_OFF_NEXTPTR),
                        0, new_pn_vp);
                    pool_release(&ctx->pool, &tail_slot);
                    if (old_next != 0) {
                        fc_page_root = vfs_rd8_s(fc_slot.bytes,
                            FILECONTENT_OFF_ROOTPTR, ctx->page_size);
                        goto retry_walk;
                    }
                }
                result_vp = new_pn_vp;
                vfs_atomic_add_i32((int32_t*)(fc_slot.bytes + FILECONTENT_OFF_PAGECOUNT), 1);
                rc = 0;
                goto done;
            }

        done:
            /* Patch the cached array if we found/created a PageNode for
               a slot that was NULL in the cache */
            if (cache_slot >= 0 && result_vp != 0 &&
                tcache[cache_slot].populated) {
                tcache[cache_slot].arr.vptr_array[page_in_segment] = result_vp;
            }

            /* Populate tcache via segment_array_build if segment has enough pages */
            fc_page_count = (uint32_t)vfs_atomic_load_i32(
                (const int32_t*)(fc_slot.bytes + FILECONTENT_OFF_PAGECOUNT));
            if ((int)fc_page_count >= SPARSE_CACHE_THRESHOLD) {
                int slot = tcache_next % TCACHE_SIZE;
                if (tcache[slot].arr.built)
                    segment_array_destroy(&tcache[slot].arr);
                int build_rc = segment_array_build(&ctx->pool,
                    vfs_rd8_s(fc_slot.bytes, FILECONTENT_OFF_ROOTPTR, ctx->page_size),
                    seg_size, &tcache[slot].arr);
                if (build_rc == VFS_OK) {
                    tcache[slot].key = cache_key;
                    tcache[slot].populated = true;
                    tcache[slot].gen = vfs_atomic_load_i64(&ctx->gc_generation);
                    tcache_next++;
#ifndef NDEBUG
                    atomic_fetch_add(&tree_resolve_page_cache_builds, 1);
#endif
                }
            }
            /* Phase 25: break out of the segment loop.  The OLD code
               returned the raw pointer; in the copy-out model we
               persist the tcache update and exit.  We must NOT fall
               through to retry_walk (which would re-walk the chain
               and double the cache-read count per call). */
            pool_release(&ctx->pool, &fc_slot);
            break;

        retry_walk:
            /* CAS failed — re-walk the chain from the (possibly updated) root.
               If we find the page, set result_vp/rc; otherwise loop again. */
            {
                pn_vp = fc_page_root;
                prev_vp = 0;
                total_pages_seen = 0;
                (void)total_pages_seen;
#ifndef NDEBUG
                prev_page_index = -1;
#endif
                int found_in_retry = 0;
                while (pn_vp != 0) {
                    total_pages_seen++;
                    PoolSlot pn_slot = {0};
                    pool_acquire(&ctx->pool, pn_vp, true, &pn_slot);
                    if (pn_slot.vptr == VFS_VPTR_NULL) break;
                    uint32_t pn_idx;
                    int64_t pn_next;
                    int64_t pn_ver_root;
                    nodes_read_pagenode(pn_slot.bytes, &pn_ver_root, &pn_next, &pn_idx, ctx->page_size);
                    (void)pn_ver_root;
                    pool_release(&ctx->pool, &pn_slot);
#ifndef NDEBUG
                    assert(prev_page_index < (int64_t)pn_idx);
                    prev_page_index = (int64_t)pn_idx;
#endif
                    if ((int64_t)pn_idx == page_in_segment) {
                        result_vp = pn_vp;
                        rc = 0;
                        found_in_retry = 1;
                        break;
                    }
                    prev_vp = pn_vp;
                    pn_vp = pn_next;
                }
                if (found_in_retry) break;
                /* Still not found — retry the whole segment resolution */
                pool_release(&ctx->pool, &fc_slot);
                i--;
                continue;
            }
        }

        prev_fc_vp = fc_vp;
        fc_vp = vfs_rd8_s(fc_slot.bytes, FILECONTENT_OFF_NEXTPTR, ctx->page_size);
        pool_release(&ctx->pool, &fc_slot);
    }

    pool_release(&ctx->pool, &file_slot);
    if (rc != 0 || result_vp == 0) return -1;

    /* Final copy-out: re-acquire the result slot into *out.  When
       is_write is true, we want the slot pinned so the caller's
       subsequent release (after the CAS-on-local) writes the
       modified VERSIONROOT back to the cache.  For read paths, an
       un-pinned slot is sufficient — release is a no-op and the
       local is a pure stack copy. */
    pool_acquire(&ctx->pool, result_vp, is_write, out);
    if (out->vptr == VFS_VPTR_NULL) return -1;
    return 0;
}

/* Phase 25: TEST-ONLY compat shim — see tree.h.  Preserves the OLD
   `uint8_t*` return shape for the test suite, rotating through a
   thread-local pool of PoolSlots so multiple calls return distinct
   pointers (the OLD API shape). */
#define VFS_TREE_COMPAT_SLOTS 128
static __thread PoolSlot _tree_resolve_compat[VFS_TREE_COMPAT_SLOTS] = {{0}};
static __thread int      _tree_resolve_compat_next = 0;

uint8_t* tree_resolve_page_compat(TreeContext* ctx, int64_t file_vp,
                                   int64_t logical_page, int64_t epoch,
                                   bool is_write) {
    PoolSlot* slot = &_tree_resolve_compat[_tree_resolve_compat_next];
    _tree_resolve_compat_next = (_tree_resolve_compat_next + 1) % VFS_TREE_COMPAT_SLOTS;
    int rc = tree_resolve_page(ctx, file_vp, logical_page, epoch, is_write, slot);
    if (rc != 0) return NULL;
    return slot->bytes;
}

void tree_resolve_page_compat_release(TreeContext* ctx) {
    /* No-op — the rotating slot is overwritten on the next call.  Kept
       for API symmetry. */
    (void)ctx;
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
    if (vfs_lock(vfs, (int64_t)new_nodeId, epoch) != VFS_OK) {
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_IO;
    }

    /* Allocate FileNode slot and write it */
    int64_t file_vp = pool_alloc(&ctx->pool);

    if (file_vp == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
        vfs->ctx->last_error = VFS_ERR_FULL;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_FULL;
    }
    pool_acquire(&ctx->pool, file_vp, true, &file_slot);

    if (file_slot.vptr == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
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
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_IO;
    }

    /* Allocate DirContent slot outside the CAS loop to avoid leaks on retry */
    int64_t dc_vp = pool_alloc(&ctx->pool);

    if (dc_vp == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
        vfs->ctx->last_error = VFS_ERR_FULL;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_FULL;
    }
    pool_acquire(&ctx->pool, dc_vp, true, &dc_slot);

    if (dc_slot.vptr == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_IO;
    }

    /* CAS-prepend DirContent to parent's headPtr */
    int64_t old_head;
    do {
        old_head = vfs_atomic_load_i64(
            (const int64_t*)(parent_slot.bytes + DIRNODE_OFF_HEADPTR));

        nodes_write_dircontent(dc_slot.bytes, new_nodeId, (uint32_t)epoch,
                               file_vp, name_vp, old_head, ctx->page_size);
        vfs_mb_release();
    } while (vfs_cas_i64((int64_t*)(parent_slot.bytes + DIRNODE_OFF_HEADPTR),
                         old_head, dc_vp) != old_head);

    /* Insert into the directory's radix tree index (Phase 18).
       The chain entry already exists — the tree is additive.  If the
       insert fails (pool exhausted), the tree entry is missing but
       the chain remains the source of truth for readdir. */
    {
        int64_t parentIndex = vfs_rd8_s(parent_slot.bytes,
                                         DIRNODE_OFF_INDEXHEADPTR,
                                         ctx->page_size);
        uint64_t nameHash = name_hash_compute(name, (int)strlen(name));
        dircontentindex_insert(&ctx->pool, &parentIndex, nameHash,
                               dc_vp, ctx->page_size);
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
                int64_t dcVP, nextLinkVP;
                nodes_read_dircontentlink(linkSlot.bytes, &dcVP, &nextLinkVP,
                                          ctx->page_size);
                pool_release(&ctx->pool, &linkSlot);

                PoolSlot dc_check;
                pool_acquire(&ctx->pool, dcVP, true, &dc_check);
                if (dc_check.vptr == VFS_VPTR_NULL) { linkVP = nextLinkVP; continue; }

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
                linkVP = nextLinkVP;
            }
        }
    }

    /* Chain walk — always runs as safety net. */
    {
        int64_t headPtr = vfs_rd8_s(parent_slot.bytes, DIRNODE_OFF_HEADPTR,
                                     ctx->page_size);
        int64_t walk_vp = headPtr;
        while (walk_vp != 0) {
            PoolSlot dc_check;
            pool_acquire(&ctx->pool, walk_vp, true, &dc_check);
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
                walk_vp = nx; continue; }
                char entry_name[256];
                int nl = nodes_read_name(&ctx->pool, np, entry_name,
                                         (int)sizeof(entry_name));
                if (nl > 0 && strcmp(entry_name, name) == 0) {
                    vfs->ctx->last_error = VFS_ERR_EXISTS;
                    pool_release(&ctx->pool, &parent_slot);
                    return VFS_ERR_EXISTS;
                }
            }
            walk_vp = nx;
        }
    }

    uint32_t new_nodeId = (uint32_t)vfs_atomic_add_i32(
        (int32_t*)&ctx->nextNodeId, 1);
    if (vfs_lock(vfs, (int64_t)new_nodeId, epoch) != VFS_OK) {
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_IO;
    }

    int64_t dir_vp = pool_alloc(&ctx->pool);

    if (dir_vp == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
        vfs->ctx->last_error = VFS_ERR_FULL;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_FULL;
    }
    pool_acquire(&ctx->pool, dir_vp, true, &dir_slot);

    if (dir_slot.vptr == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_IO;
    }

    /* Allocate the initial radix tree root for this new directory.
       Every DirNode starts with a valid indexHeadPtr. */
    int64_t dirIndexVP = pool_alloc(&ctx->pool);
    if (dirIndexVP == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
        vfs->ctx->last_error = VFS_ERR_FULL;
        pool_release(&ctx->pool, &dir_slot);
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_FULL;
    }
    pool_acquire(&ctx->pool, dirIndexVP, true, &dirIndexSlot);
    if (dirIndexSlot.vptr == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
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
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_IO;
    }

    int64_t dc_vp = pool_alloc(&ctx->pool);

    if (dc_vp == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
        vfs->ctx->last_error = VFS_ERR_FULL;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_FULL;
    }
    pool_acquire(&ctx->pool, dc_vp, true, &dc_slot);

    if (dc_slot.vptr == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &parent_slot);
        return VFS_ERR_IO;
    }

    int64_t old_head;
    do {
        old_head = vfs_atomic_load_i64(
            (const int64_t*)(parent_slot.bytes + DIRNODE_OFF_HEADPTR));
        nodes_write_dircontent(dc_slot.bytes, new_nodeId, (uint32_t)epoch,
                               dir_vp, name_vp, old_head, ctx->page_size);
        vfs_mb_release();
    } while (vfs_cas_i64((int64_t*)(parent_slot.bytes + DIRNODE_OFF_HEADPTR),
                         old_head, dc_vp) != old_head);

    /* Insert into the directory's radix tree index (Phase 18). */
    {
        int64_t parentIndex = vfs_rd8_s(parent_slot.bytes,
                                         DIRNODE_OFF_INDEXHEADPTR,
                                         ctx->page_size);
        uint64_t nameHash = name_hash_compute(name, (int)strlen(name));
        dircontentindex_insert(&ctx->pool, &parentIndex, nameHash,
                               dc_vp, ctx->page_size);
        vfs_wr8_s(parent_slot.bytes, DIRNODE_OFF_INDEXHEADPTR, parentIndex,
                  ctx->page_size);
        /* W1b: childCount removed. */
    }

    pool_release(&ctx->pool, &dc_slot);
    pool_release(&ctx->pool, &parent_slot);
    vfs_unlock(vfs, (int64_t)new_nodeId, epoch);
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

    /* CAS-prepend tombstone to parent's headPtr */
    int64_t old_head;
    do {
        old_head = vfs_atomic_load_i64(
            (const int64_t*)(parent_slot.bytes + DIRNODE_OFF_HEADPTR));

        /* Tombstone: namePtr=0 means deleted */
        nodes_write_dircontent(dc_slot.bytes, found_childId, (uint32_t)epoch,
                               found_childPtr, 0, old_head, ctx->page_size);
        vfs_mb_release();
    } while (vfs_cas_i64((int64_t*)(parent_slot.bytes + DIRNODE_OFF_HEADPTR),
                         old_head, dc_vp) != old_head);

    /* Insert a tree link for the tombstone (Phase 18).  The tree leaf
       already has a link for the live entry at the same name hash; we
       add a second link for the tombstone.  dirchain_find_child applies
       epoch dedup so the higher-epoch tombstone suppresses the live
       entry.  Tree-insert failure leaves the tombstone in the chain
       only — chain walk fallback still hides the entry correctly. */
    {
        int64_t parentIndex = vfs_rd8_s(parent_slot.bytes,
                                         DIRNODE_OFF_INDEXHEADPTR,
                                         ctx->page_size);
        uint64_t nameHash = name_hash_compute(name, (int)strlen(name));
        dircontentindex_insert(&ctx->pool, &parentIndex, nameHash,
                               dc_vp, ctx->page_size);
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
                    int64_t dcVP, nextLinkVP;
                    nodes_read_dircontentlink(linkSlot.bytes, &dcVP, &nextLinkVP,
                                              ctx->page_size);
                    pool_release(&ctx->pool, &linkSlot);

                    PoolSlot dc_check;
                    pool_acquire(&ctx->pool, dcVP, false, &dc_check);
                    if (dc_check.vptr == VFS_VPTR_NULL) { linkVP = nextLinkVP; continue; }

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
                    linkVP = nextLinkVP;
                }
            }
        }
    }

    /* Chain walk — safety net (always runs if not found in tree) */
    if (found_vp == 0) {
        int64_t walk_vp = headPtr;
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

    /* Check directory is empty using read-rule: for each childNodeId, find the
       entry at the highest epoch ≤ query_epoch.  If any such entry has
       namePtr ≠ 0, the directory is not empty (tombstones with namePtr=0
       indicate deleted entries). */
    int64_t child_head = vfs_rd8_s(child_slot.bytes, DIRNODE_OFF_HEADPTR, ctx->page_size);
    {
        /* Simple approach: walk chain and collect unique childNodeIds.
           For each child, track the highest epoch and whether the entry
           at that epoch has namePtr != 0. */
        #define MAX_RMDIR_CHILDREN 1024
        uint32_t child_ids[MAX_RMDIR_CHILDREN];
        uint32_t child_best_epoch[MAX_RMDIR_CHILDREN];
        int     child_has_name[MAX_RMDIR_CHILDREN];
        int child_count = 0;

        int64_t cw = child_head;
        while (cw != 0 && child_count < MAX_RMDIR_CHILDREN) {
            PoolSlot cs;
            pool_acquire(&ctx->pool, cw, false, &cs);
            if (cs.vptr == VFS_VPTR_NULL) break;
            uint32_t ccc, cce;
            int64_t ccp, cnp, cnx;
            nodes_read_dircontent(cs.bytes, &ccc, &cce, &ccp, &cnp, &cnx,
                                  ctx->page_size);
            pool_release(&ctx->pool, &cs);
            (void)ccp;

            if (cce <= (uint32_t)epoch) {
                int found = -1;
                for (int i = 0; i < child_count; i++) {
                    if (child_ids[i] == ccc) { found = i; break; }
                }
                if (found >= 0) {
                    if (cce > child_best_epoch[found]) {
                        child_best_epoch[found] = cce;
                        child_has_name[found] = (cnp != 0) ? 1 : 0;
                    }
                } else {
                    child_ids[child_count] = ccc;
                    child_best_epoch[child_count] = cce;
                    child_has_name[child_count] = (cnp != 0) ? 1 : 0;
                    child_count++;
                }
            }
            cw = cnx;
        }

        for (int i = 0; i < child_count; i++) {
            if (child_has_name[i]) {
                vfs_unlock(vfs, (int64_t)found_childId, epoch);
                vfs->ctx->last_error = VFS_ERR_NOTEMPTY;
                pool_release(&ctx->pool, &child_slot);
                pool_release(&ctx->pool, &parent_slot);
                return VFS_ERR_NOTEMPTY;
            }
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

    int64_t old_head;
    do {
        old_head = vfs_atomic_load_i64(
            (const int64_t*)(parent_slot.bytes + DIRNODE_OFF_HEADPTR));
        nodes_write_dircontent(dc_slot.bytes, found_childId, (uint32_t)epoch,
                               found_childPtr, 0, old_head, ctx->page_size);
        vfs_mb_release();
    } while (vfs_cas_i64((int64_t*)(parent_slot.bytes + DIRNODE_OFF_HEADPTR),
                         old_head, dc_vp) != old_head);

    /* Insert a tree link for the tombstone (Phase 18) — same pattern as
       vfs_delete.  The chain is source of truth, the tree is an additive
       index; insert failure is benign. */
    {
        int64_t parentIndex = vfs_rd8_s(parent_slot.bytes,
                                         DIRNODE_OFF_INDEXHEADPTR,
                                         ctx->page_size);
        uint64_t nameHash = name_hash_compute(name, (int)strlen(name));
        dircontentindex_insert(&ctx->pool, &parentIndex, nameHash,
                               dc_vp, ctx->page_size);
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
 * dirchain_list — walk DirContent chain, collect non-tombstone entries
 * at epoch via read-rule dedup by childNodeId.  Allocates a vfs_dirent_t[]
 * of exact size needed; caller frees with free() or vfs_free_dirents().
 *
 * Phase 19: dedup state is a heap-backed VarArray (DirchainDedupEntry),
 * unbounded.  Replaces the 5 parallel fixed-size arrays (DENTRY_CACHE_MAX)
 * that previously truncated directories at 1024 unique children.
 *
 * Phase 24: this is the only readdir API (replaces both the old
 * caller-buffer dirchain_list and dirchain_list_all).  No cap, no
 * caller-buffer guess, no doubling.
 *
 * Algorithm: chain is descending by epoch (prepend ordering).  First
 * applicable hit per childNodeId is by definition the highest-epoch
 * applicable record, so the dedup scans for "already seen" only — no
 * epoch comparison is needed.
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

    /* W1b: childCount removed.  Until W5 removes the dedup entirely,
       size the dedup hash_map with a modest default (0 → hash_map
       picks the minimum scale=4, capacity=16).  The dedup only
       protects the readdir output from the rare same-child
       multi-link case (W5 deletes the multi-link), so an under-sized
       map just incurs a few rehashes. */
    VarArray(DirchainDedupEntry) dedup = var_array_new(DirchainDedupEntry);
    HashMap(int64_t, int64_t) seen = hash_map_new_for_max(int64_t, int64_t, 0);
    if (!dedup || !seen) {
        var_array_delete(dedup);
        hash_map_free(seen);
        return VFS_ERR_IO;
    }

    int64_t walk_vp = headPtr;
    while (walk_vp != 0) {
        /* Phase 25: by-value pool slot (read-only). */
        PoolSlot dc_slot = {0};
        pool_acquire(&ctx->pool, walk_vp, false, &dc_slot);
        if (dc_slot.vptr == VFS_VPTR_NULL) break;
        uint32_t ce_child, ce_epoch;
        int64_t ce_childPtr, ce_namePtr, ce_next;
        nodes_read_dircontent(dc_slot.bytes, &ce_child, &ce_epoch, &ce_childPtr,
                              &ce_namePtr, &ce_next, ctx->page_size);

        int64_t eff_epoch = (int64_t)ce_epoch;
        if (mapper_table_traversal_apply(&ctx->mapper_table, (int64_t)ce_epoch))
            eff_epoch = mapper_table_resolve(&ctx->mapper_table, (int64_t)ce_epoch);

        int applies = (eff_epoch == read_epoch) ||
                      (eff_epoch < read_epoch && eff_epoch % 2 == 0);
        if (!applies) { walk_vp = ce_next; pool_release(&ctx->pool, &dc_slot); continue; }

        /* O(1) "already seen" via hash_map.  Chain is descending by
           epoch (prepend ordering), so first-hit-wins keeps the
           highest-epoch record. */
        if (hash_map_contains(seen, (int64_t)ce_child)) {
            walk_vp = ce_next;
            pool_release(&ctx->pool, &dc_slot);
            continue;
        }

        DirchainDedupEntry entry = {
            .childNodeId = (int64_t)ce_child,
            .childPtr    = ce_childPtr,
            .name_set    = (ce_namePtr != 0),
            .namePtr     = ce_namePtr,
        };
        int64_t dedup_idx = dedup->count;
        (void)var_array_append(dedup, entry);
        (void)hash_map_put(seen, (int64_t)ce_child, dedup_idx);
        walk_vp = ce_next;
        pool_release(&ctx->pool, &dc_slot);
    }

    /* Allocate output buffer to upper bound (dedup->count including
       tombstones), then realloc to exact size after we know the
       non-tombstone count.  This keeps the implementation to a
       single pass over the dedup array. */
    int total_count = dedup->count;
    if (total_count == 0) {
        hash_map_free(seen);
        var_array_delete(dedup);
        *out_entries = NULL;
        *out_count = 0;
        return VFS_OK;
    }

    vfs_dirent_t* out = (vfs_dirent_t*)malloc((size_t)total_count * sizeof(vfs_dirent_t));
    if (!out) {
        hash_map_free(seen);
        var_array_delete(dedup);
        return VFS_ERR_IO;
    }

    int written = 0;
    for (int i = 0; i < dedup->count; i++) {
        DirchainDedupEntry* e = var_array_lookup(dedup, i);
        if (!e || !e->name_set) continue;

        out[written].vp     = e->childPtr;
        out[written].nodeId = e->childNodeId;
        out[written].name[0] = '\0';
        out[written].isDir = false;

        /* Phase 25: by-value pool slot (read-only) for child type check. */
        PoolSlot child_slot = {0};
        pool_acquire(&ctx->pool, e->childPtr, false, &child_slot);
        if (child_slot.vptr != VFS_VPTR_NULL) {
            int16_t ctype = vfs_rd2_s(child_slot.bytes, DIRNODE_OFF_TYPE,
                                       ctx->page_size);
            out[written].isDir = (ctype == (int16_t)NODE_TYPE_DIR);
        }
        pool_release(&ctx->pool, &child_slot);
        if (e->namePtr != 0)
            nodes_read_name(&ctx->pool, e->namePtr,
                            out[written].name,
                            (int)sizeof(out[written].name));
        written++;
    }

    var_array_delete(dedup);
    hash_map_free(seen);

    /* Shrink to exact size if we skipped some tombstones.  realloc
       with a smaller size may move the block; that's fine since the
       caller just gets back the (possibly new) pointer. */
    if (written < total_count) {
        vfs_dirent_t* exact = (vfs_dirent_t*)realloc(out, (size_t)written * sizeof(vfs_dirent_t));
        if (exact) out = exact;  /* realloc with NULL or same-size returns original */
    }

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

    if (vfs_lock(vfs, (int64_t)rn_childId, epoch) != VFS_OK) {
        pool_release(&ctx->pool, &dst_slot);
        pool_release(&ctx->pool, &src_slot);
        return VFS_ERR_IO;
    }

    if (src_parent == dst_parent && found_epoch == (uint32_t)epoch) {
        int64_t new_name_vp;
        int ns = nodes_write_name(&ctx->pool, dst, &new_name_vp);
        if (ns == 0) {
            vfs_unlock(vfs, (int64_t)rn_childId, epoch);
            vfs->ctx->last_error = VFS_ERR_IO;
            pool_release(&ctx->pool, &dst_slot);
            pool_release(&ctx->pool, &src_slot);
            return VFS_ERR_IO;
        }

        /* Capture the DirContent VP for the live entry at the src name.
           We need it so we can zero its old tree link and add a new link
           at the dst name's hash. */
        int64_t walk_vp = vfs_rd8_s(src_slot.bytes, DIRNODE_OFF_HEADPTR, ctx->page_size);
        int64_t matched_walk_vp = 0;
        while (walk_vp != 0) {
            PoolSlot dc;
            pool_acquire(&ctx->pool, walk_vp, true, &dc);
            if (dc.vptr == VFS_VPTR_NULL) break;
            uint32_t cc, ce;
            int64_t cp, np, nx;
            nodes_read_dircontent(dc.bytes, &cc, &ce, &cp, &np, &nx, ctx->page_size);
            if (cp == rn_childPtr && np != 0 && ce <= (uint32_t)epoch) {
                matched_walk_vp = walk_vp;
                vfs_mb_release();
                vfs_atomic_store_i64((int64_t*)(dc.bytes + DIRCONTENT_OFF_NAMEPTR), new_name_vp);
                pool_release(&ctx->pool, &dc);
                break;
            }
            pool_release(&ctx->pool, &dc);
            walk_vp = nx;
        }
        if (matched_walk_vp == 0) {
            vfs->ctx->last_error = VFS_ERR_IO;
            vfs_unlock(vfs, (int64_t)rn_childId, epoch);
            pool_release(&ctx->pool, &dst_slot);
            pool_release(&ctx->pool, &src_slot);
            return VFS_ERR_IO;
        }

        /* Tree update (Phase 18): zero the link at the OLD name's hash
           (the link still points to matched_walk_vp — which now has the
           new name), and add a fresh link at the NEW name's hash.
           dirchain_find_child applies epoch + name dedup so the stale
           link's hash-mismatch naturally hides it. */
        {
            int64_t srcIndex = vfs_rd8_s(src_slot.bytes, DIRNODE_OFF_INDEXHEADPTR,
                                         ctx->page_size);
            uint64_t old_hash = name_hash_compute(src, (int)strlen(src));
            uint64_t new_hash = name_hash_compute(dst, (int)strlen(dst));
            dircontentindex_remove(&ctx->pool, srcIndex, old_hash,
                                   matched_walk_vp, ctx->page_size);
            dircontentindex_insert(&ctx->pool, &srcIndex, new_hash,
                                   matched_walk_vp, ctx->page_size);
            vfs_wr8_s(src_slot.bytes, DIRNODE_OFF_INDEXHEADPTR, srcIndex,
                      ctx->page_size);
            /* W1b: childCount removed. */
        }

        vfs_unlock(vfs, (int64_t)rn_childId, epoch);
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
        return VFS_ERR_IO;
    }

    /* Allocate DirContent for dst */
    int64_t dst_dc_vp = pool_alloc(&ctx->pool);

    if (dst_dc_vp == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)rn_childId, epoch);
        vfs->ctx->last_error = VFS_ERR_FULL;
        pool_release(&ctx->pool, &dst_slot);
        pool_release(&ctx->pool, &src_slot);
        return VFS_ERR_FULL;
    }
    PoolSlot dst_dc_slot;
    pool_acquire(&ctx->pool, dst_dc_vp, true, &dst_dc_slot);

    if (dst_dc_slot.vptr == VFS_VPTR_NULL) {
        vfs_unlock(vfs, (int64_t)rn_childId, epoch);
        vfs->ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &dst_slot);
        pool_release(&ctx->pool, &src_slot);
        return VFS_ERR_IO;
    }

    /* CAS-prepend to dst_parent's headPtr */
    int64_t dst_old_head;
    do {
        dst_old_head = vfs_atomic_load_i64(
            (const int64_t*)(dst_slot.bytes + DIRNODE_OFF_HEADPTR));
        nodes_write_dircontent(dst_dc_slot.bytes, rn_childId, (uint32_t)epoch,
                               rn_childPtr, dst_name_vp, dst_old_head,
                               ctx->page_size);
        vfs_mb_release();
    } while (vfs_cas_i64((int64_t*)(dst_slot.bytes + DIRNODE_OFF_HEADPTR),
                         dst_old_head, dst_dc_vp) != dst_old_head);
    pool_release(&ctx->pool, &dst_dc_slot);  /* dst_dc_slot not used after CAS */

    /* Tree insert for the dst entry (Phase 18) — additive index. */
    {
        int64_t dstIndex = vfs_rd8_s(dst_slot.bytes, DIRNODE_OFF_INDEXHEADPTR,
                                     ctx->page_size);
        uint64_t dst_hash = name_hash_compute(dst, (int)strlen(dst));
        dircontentindex_insert(&ctx->pool, &dstIndex, dst_hash,
                               dst_dc_vp, ctx->page_size);
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

        int64_t src_old_head;
        do {
            src_old_head = vfs_atomic_load_i64(
                (const int64_t*)(src_slot.bytes + DIRNODE_OFF_HEADPTR));
            nodes_write_dircontent(src_dc_slot.bytes, rn_childId, (uint32_t)epoch,
                                   rn_childPtr, 0, src_old_head, ctx->page_size);
            vfs_mb_release();
        } while (vfs_cas_i64((int64_t*)(src_slot.bytes + DIRNODE_OFF_HEADPTR),
                             src_old_head, src_dc_vp) != src_old_head);
        pool_release(&ctx->pool, &src_dc_slot);

        /* Tree insert for the src tombstone (Phase 18) — same pattern as
           vfs_delete: tombstone link at the src name's hash. */
        {
            int64_t srcIndex = vfs_rd8_s(src_slot.bytes, DIRNODE_OFF_INDEXHEADPTR,
                                         ctx->page_size);
            uint64_t src_hash = name_hash_compute(src, (int)strlen(src));
            dircontentindex_insert(&ctx->pool, &srcIndex, src_hash,
                                   src_dc_vp, ctx->page_size);
            vfs_wr8_s(src_slot.bytes, DIRNODE_OFF_INDEXHEADPTR, srcIndex,
                      ctx->page_size);
            /* W1b: childCount removed. */
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
        /* CAS-claim the root pointer (writes to the DirNode's cached
           field or the caller's in-memory slot). */
        vfs_cas_i64((int64_t*)indexRoot, 0, rootVP);
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

            int64_t oldHead;
            do {
                oldHead = vfs_atomic_load_i64(
                    (const int64_t*)(slot.bytes + DIRCONTENTINDEX_OFF_LISTVP));
                nodes_write_dircontentlink(linkSlot.bytes, dirContentVP,
                                           oldHead, page_size);
            } while (vfs_cas_i64(
                         (int64_t*)(slot.bytes + DIRCONTENTINDEX_OFF_LISTVP),
                         oldHead, linkVP) != oldHead);
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

                int64_t oldHead;
                do {
                    oldHead = vfs_atomic_load_i64(
                        (const int64_t*)(childSlot.bytes +
                                         DIRCONTENTINDEX_OFF_LISTVP));
                    nodes_write_dircontentlink(linkSlot.bytes, dirContentVP,
                                               oldHead, page_size);
                } while (vfs_cas_i64(
                             (int64_t*)(childSlot.bytes +
                                        DIRCONTENTINDEX_OFF_LISTVP),
                             oldHead, linkVP) != oldHead);
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

        int64_t oldHead;
        do {
            oldHead = vfs_atomic_load_i64(
                (const int64_t*)(slot.bytes + DIRCONTENTINDEX_OFF_LISTVP));
            nodes_write_dircontentindex(newChildSlot.bytes, (uint8_t)target,
                                         isLast ? NODE_TYPE_INDEX_LEAF
                                                : NODE_TYPE_INDEX_INTERNAL,
                                         0, oldHead, page_size);
        } while (vfs_cas_i64(
                     (int64_t*)(slot.bytes + DIRCONTENTINDEX_OFF_LISTVP),
                     oldHead, newChildVP) != oldHead);

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
            /* LEAF's listVP starts at 0, so CAS-claim it directly */
            int64_t old = vfs_cas_i64(
                (int64_t*)(newChildSlot.bytes + DIRCONTENTINDEX_OFF_LISTVP),
                0, linkVP);
            if (old != 0) {
                /* Another thread raced and inserted first — our slot
                   is orphaned (harmless one-slot leak). */
            }
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
 * dirchain_find_child — walk DirContent chain, read-rule dedup, return match
 * --------------------------------------------------------------------------- */

int dirchain_find_child(TreeContext* ctx, int64_t dir_vp, const char* name,
                        int64_t epoch, int64_t* out_childPtr,
                        uint32_t* out_nodeId, uint32_t* out_epoch) {
    if (!ctx || !name || name[0] == '\0') return VFS_ERR_IO;
    if (!out_childPtr || !out_nodeId) return VFS_ERR_IO;

    /* Pre-compute hash for fast-reject: skip expensive strcmp when hashes
     * don't match.  The hash is stored in the first NameEntry slot at
     * offset 0 and read via nodes_read_name_hash. */
    uint64_t target_hash = name_hash_compute(name, (int)strlen(name));

    /* Phase 25: by-value pool slot (read-only, pinPage=false).  FUSE
       hot path — copy-out closes C1; release is no-op on un-pinned. */
    PoolSlot dir_slot = {0};
    pool_acquire(&ctx->pool, dir_vp, false, &dir_slot);
    if (dir_slot.vptr == VFS_VPTR_NULL) return VFS_ERR_NOTFOUND;
    if (vfs_rd2_s(dir_slot.bytes, DIRNODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_DIR)
        return VFS_ERR_NOTDIR;

    int64_t read_epoch = mapper_table_resolve(&ctx->mapper_table, epoch);

    /* --- Phase 18: radix tree fast path ---
       If the directory has a tree index, walk it by name hash.  The
       tree is the source of truth for name lookups — no chain fallback
       when the tree exists. */
    int64_t indexRoot = vfs_rd8_s(dir_slot.bytes, DIRNODE_OFF_INDEXHEADPTR,
                                   ctx->page_size);
    int64_t headPtr   = vfs_rd8_s(dir_slot.bytes, DIRNODE_OFF_HEADPTR,
                                   ctx->page_size);
    /* dir_slot is no longer used after this point — release early to
       shrink the live working set. */
    pool_release(&ctx->pool, &dir_slot);

    if (indexRoot != 0) {
        int64_t leafVP = dircontentindex_lookup(&ctx->pool, indexRoot,
                                                target_hash, ctx->page_size);
        if (leafVP != 0) {
            /* Walk the DirContentLink list at this leaf.  Each link
               points to a DirContent in the chain.  Apply the same
               hash-fast-reject + strcmp + read-rule dedup as the
               chain walk below. */
            int64_t linkVP = leafVP;
            int64_t best_child = 0, best_childPtr = 0;
            int64_t best_eff_epoch = 0, best_raw_epoch = 0;
            int best_name_match = 0;
            /* Track the most recent tombstone's childNodeId.  Since the
               chain is HEAD-to-TAIL and new entries (including
               tombstones) are prepended, a tombstone for childId X
               always appears in the chain BEFORE a live entry for the
               SAME childId X.  When we see a live entry, if its
               childId matches tombstoned_childId, we know a tombstone
               has already been processed and this live entry should
               be suppressed. */
            int64_t tombstoned_childId = 0;

            while (linkVP != 0) {
                /* Phase 25: by-value pool slot (read-only). */
                PoolSlot linkSlot = {0};
                pool_acquire(&ctx->pool, linkVP, false, &linkSlot);
                if (linkSlot.vptr == VFS_VPTR_NULL) break;

                int64_t dcVP, nextLinkVP;
                nodes_read_dircontentlink(linkSlot.bytes, &dcVP, &nextLinkVP,
                                          ctx->page_size);
                pool_release(&ctx->pool, &linkSlot);

                /* Phase 25: by-value pool slot (read-only) for dc. */
                PoolSlot dc_slot = {0};
                pool_acquire(&ctx->pool, dcVP, false, &dc_slot);
                if (dc_slot.vptr == VFS_VPTR_NULL) { linkVP = nextLinkVP; continue; }

                uint32_t ce_child, ce_epoch;
                int64_t ce_childPtr, ce_namePtr, ce_next;
                nodes_read_dircontent(dc_slot.bytes, &ce_child, &ce_epoch,
                                      &ce_childPtr, &ce_namePtr, &ce_next,
                                      ctx->page_size);
                pool_release(&ctx->pool, &dc_slot);

                int64_t eff_epoch = (int64_t)ce_epoch;
                if (mapper_table_traversal_apply(&ctx->mapper_table, (int64_t)ce_epoch))
                    eff_epoch = mapper_table_resolve(&ctx->mapper_table, (int64_t)ce_epoch);

                int applies = (eff_epoch == read_epoch) ||
                              (eff_epoch < read_epoch && eff_epoch % 2 == 0);
                if (!applies) { linkVP = nextLinkVP; continue; }

                if (ce_namePtr == 0) {
                    /* Tombstone.  Record this childId so subsequent
                       live entries for the same childId are
                       suppressed.  Don't touch best_child/best_childPtr. */
                    tombstoned_childId = (int64_t)ce_child;
                    linkVP = nextLinkVP;
                    continue;
                }

                /* Live entry.  Skip if same childId as a tombstone we
                   already processed.  Without this check, a
                   delete+recreate sequence (where both have the same
                   name but different childIds) would incorrectly
                   include the OLD tombstoned entry's live record. */
                if ((int64_t)ce_child == tombstoned_childId) {
                    linkVP = nextLinkVP;
                    continue;
                }

                uint64_t entry_hash = nodes_read_name_hash(&ctx->pool, ce_namePtr);
                if (entry_hash != target_hash) {
                    /* Hash mismatch — skip without incrementing
                       s_hash_rejects (the counter is for chain-walk
                       fast-reject, the tree path doesn't need it). */
                    linkVP = nextLinkVP; continue;
                }
                char entry_name[256];
                int nl = nodes_read_name(&ctx->pool, ce_namePtr,
                                          entry_name, (int)sizeof(entry_name));
                if (nl > 0 && strcmp(entry_name, name) == 0) {
                    best_child      = (int64_t)ce_child;
                    best_childPtr   = ce_childPtr;
                    best_eff_epoch  = eff_epoch;
                    best_raw_epoch  = (int64_t)ce_epoch;
                    best_name_match = 1;
                }
                linkVP = nextLinkVP;
            }

            if (best_name_match) {
                *out_childPtr = best_childPtr;
                *out_nodeId   = (uint32_t)best_child;
                if (out_epoch) *out_epoch = (uint32_t)best_raw_epoch;
                return VFS_OK;
            }
            /* Tree exists, leaf found, but no matching name at this epoch.
               Fall through to chain walk — the entry might have been created
               before the tree was built (e.g. legacy files). */
        }
        /* Tree exists but no leaf for this hash, or leaf had no matching
           entry.  Fall through to chain walk as safety net. */
    }

    /* --- Fallback: chain walk (for directories without a tree) --- */
    int64_t best_child = 0, best_childPtr = 0, best_eff_epoch = 0;
    int64_t best_raw_epoch = 0;
    int best_name_match = 0;
    int64_t tombstoned_childId = 0;

    int64_t walk_vp = headPtr;
    while (walk_vp != 0) {
        /* Phase 25: by-value pool slot (read-only). */
        PoolSlot dc_slot = {0};
        pool_acquire(&ctx->pool, walk_vp, false, &dc_slot);
        if (dc_slot.vptr == VFS_VPTR_NULL) break;
        uint32_t ce_child, ce_epoch;
        int64_t ce_childPtr, ce_namePtr, ce_next;
        nodes_read_dircontent(dc_slot.bytes, &ce_child, &ce_epoch, &ce_childPtr,
                              &ce_namePtr, &ce_next, ctx->page_size);

        int64_t eff_epoch = (int64_t)ce_epoch;
        if (mapper_table_traversal_apply(&ctx->mapper_table, (int64_t)ce_epoch))
            eff_epoch = mapper_table_resolve(&ctx->mapper_table, (int64_t)ce_epoch);

        int applies = (eff_epoch == read_epoch) ||
                      (eff_epoch < read_epoch && eff_epoch % 2 == 0);
        if (!applies) { walk_vp = ce_next; pool_release(&ctx->pool, &dc_slot); continue; }

        if (ce_namePtr == 0) {
            /* Tombstone.  Record childId. */
            tombstoned_childId = (int64_t)ce_child;
            walk_vp = ce_next;
            pool_release(&ctx->pool, &dc_slot);
            continue;
        }

        /* Live entry.  Skip if same childId as a tombstone we've seen
           earlier in the chain walk. */
        if ((int64_t)ce_child == tombstoned_childId) {
            walk_vp = ce_next;
            pool_release(&ctx->pool, &dc_slot);
            continue;
        }

        uint64_t entry_hash = nodes_read_name_hash(&ctx->pool, ce_namePtr);
        if (entry_hash != target_hash) {
#ifdef VFS_NAME_HASH_TESTING
                s_hash_rejects++;
#endif
            walk_vp = ce_next;
            pool_release(&ctx->pool, &dc_slot);
            continue;
        }
        char entry_name[256];
        int nl = nodes_read_name(&ctx->pool, ce_namePtr,
                                  entry_name, (int)sizeof(entry_name));
        if (nl > 0 && strcmp(entry_name, name) == 0) {
            best_child    = (int64_t)ce_child;
            best_childPtr = ce_childPtr;
            best_eff_epoch = eff_epoch;
            best_raw_epoch = (int64_t)ce_epoch;
            best_name_match = 1;
        }
        walk_vp = ce_next;
        pool_release(&ctx->pool, &dc_slot);
    }

    if (!best_name_match) return VFS_ERR_NOTFOUND;
    *out_childPtr = best_childPtr;
    *out_nodeId   = (uint32_t)best_child;
    if (out_epoch) *out_epoch = (uint32_t)best_raw_epoch;
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

    /* Phase 25: by-value pool slot, pinned because the shrink path
       modifies the FileNode's SIZEPTR via CAS on the local copy. */
    PoolSlot file_slot = {0};
    pool_acquire(&ctx->pool, file, true, &file_slot);
    if (file_slot.vptr == VFS_VPTR_NULL) {
        ctx->last_error = VFS_ERR_NOTFOUND;
        return VFS_ERR_NOTFOUND;
    }
    if (vfs_rd2_s(file_slot.bytes, FILENODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_FILE) {
        ctx->last_error = VFS_ERR_IO;
        pool_release(&ctx->pool, &file_slot);
        return VFS_ERR_IO;
    }

    int64_t cur_size = vfs_file_size(vfs, file, epoch);
    if (new_size == cur_size) {
        pool_release(&ctx->pool, &file_slot);
        return VFS_OK;
    }

    if (new_size < cur_size) {
        /* Shrink — append a new FileSize entry at epoch with new_size.
           Page reclamation is deferred to GC. */
        while (1) {
            int64_t old_sizePtr = vfs_atomic_load_i64(
                (const int64_t*)(file_slot.bytes + FILENODE_OFF_SIZEPTR));
            int64_t fs_vp = pool_alloc(&ctx->pool);
            if (fs_vp == VFS_VPTR_NULL) {
                ctx->last_error = VFS_ERR_NOMEM;
                pool_release(&ctx->pool, &file_slot);
                return VFS_ERR_NOMEM;
            }
            /* Phase 25: by-value pool slot for the new FileSize page. */
            PoolSlot fs_slot = {0};
            pool_acquire(&ctx->pool, fs_vp, true, &fs_slot);
            if (fs_slot.vptr == VFS_VPTR_NULL) {
                ctx->last_error = VFS_ERR_NOMEM;
                pool_release(&ctx->pool, &file_slot);
                return VFS_ERR_NOMEM;
            }
            nodes_write_filesize(fs_slot.bytes, (uint32_t)epoch, (int64_t)time(NULL),
                                 new_size, old_sizePtr, ctx->page_size);
            /* Release fs_slot so the writeback hits the cache before we
               try to CAS-link the entry into file_slot.SIZEPTR. */
            pool_release(&ctx->pool, &fs_slot);
            vfs_mb_release();
            /* Phase 25: CAS on the local file_slot copy.  vfs_truncate
               does not take the file lock in either OLD or NEW code, so
               concurrent truncates were already racy; the OLD CAS gave
               best-effort cache-level atomicity.  With copy-out the CAS
               is local-only (each thread has its own bytes[]) and the
               cache writeback at release is last-writer-wins.  This is
               the same shape as the vfs_write W5 fix; if a stronger
               guarantee is needed, vfs_truncate should take the file
               lock (out of scope for the C1 migration). */
            int64_t cas_res = vfs_cas_i64(
                (int64_t*)(file_slot.bytes + FILENODE_OFF_SIZEPTR),
                old_sizePtr, fs_vp);
            if (cas_res == old_sizePtr) break;
        }
        ctx->last_error = VFS_OK;
        pool_release(&ctx->pool, &file_slot);
        return VFS_OK;
    }

    /* Grow — write zeros from cur_size to new_size via vfs_write
       (which handles page allocation and FileSize updates internally).
       Release our file_slot first; vfs_write acquires its own. */
    pool_release(&ctx->pool, &file_slot);

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
        int rr_pn = tree_resolve_page(ctx, file, p, epoch, true, &pn_slot);
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

            /* Release barrier then CAS on pn_slot.  Phase 25: CAS on the
               local pn_slot.bytes (per-thread).  On success we release
               pn_slot to write the new VERSIONROOT back to the cache.
               On failure we DISABLE the pin and skip release, so the
               stale local does not overwrite a newer VERSIONROOT that
               another writer may have installed. */
            vfs_mb_release();
            int64_t old_root = vfs_cas_i64(
                (int64_t*)(pn_slot.bytes + PAGENODE_OFF_VERSIONROOT),
                version_root, vp_new);
            if (old_root == version_root) {
                pool_release(&ctx->pool, &pn_slot);  /* write back vp_new */
                break;  /* CAS succeeded — exit retry loop */
            }
            /* CAS failed — discard the local so we don't overwrite the
               cache with our stale VERSIONROOT.  Re-acquire on next iter
               picks up whatever another writer installed. */
            pn_slot.pinnedPage = 0;
            pool_release(&ctx->pool, &pn_slot);
            /* Our orphaned data page and VersionPage will be reclaimed by GC. */
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
        int rr_pn = tree_resolve_page(ctx, file, p, read_epoch, false, &pn_slot);
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
