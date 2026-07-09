/* Phase 5: Tree Operations — Bootstrap, Init, Superblock I/O */
#include "tree.h"
#include "page_array.h"
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

    uint8_t* root_slot = pool_resolve_rw(&ctx->pool, root_vp);
    if (!root_slot) return VFS_ERR_IO;

    /* Write root DirNode: nodeId=0, no children */
    nodes_write_dirnode(root_slot, 0, 0, 0, ctx->page_size);

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

    uint8_t* root_slot = pool_resolve_ro(&ctx->pool, ctx->rootNodeOffset);
    if (!root_slot) return VFS_ERR_IO;

    /* Verify type is DirNode (0x01) */
    int16_t type = vfs_rd2_s(root_slot, DIRNODE_OFF_TYPE, ctx->page_size);
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
        uint8_t* slot = pool_resolve_ro(&ctx->pool, mapper_vp);
        if (!slot) return VFS_ERR_IO;
        uint32_t fromEpoch, toEpoch;
        uint16_t flags;
        int64_t next;
        nodes_read_mapperentry(slot, &fromEpoch, &toEpoch, &flags, &next, ctx->page_size);
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
 * Returns a pointer to the PageNode slot (via pool_resolve), or NULL on error.
 * --------------------------------------------------------------------------- */

uint8_t* tree_resolve_page(TreeContext* ctx, int64_t file_vp,
                           int64_t logical_page, int64_t epoch, bool is_write) {
    (void)epoch;  /* not yet used — future: segment growth decisions */

    uint32_t seg_size = ctx->segment_size;
    int64_t segment_idx = logical_page / seg_size;
    int64_t page_in_segment = logical_page % seg_size;

    /* Read FileNode to get headPtr (first FileContent) */
    uint8_t* file_slot = pool_resolve_rw(&ctx->pool, file_vp);
    if (!file_slot) return NULL;

    int64_t fc_vp;   /* VirtualPtr to current FileContent */

    uint32_t tmp_nodeId;
    int64_t tmp_headPtr, tmp_sizePtr, tmp_createdAt;

    nodes_read_filenode(file_slot, &tmp_nodeId, &tmp_headPtr, &tmp_sizePtr, &tmp_createdAt, ctx->page_size);
    fc_vp = tmp_headPtr;

    /* Walk FileContent chain to find the target segment */
    int64_t prev_fc_vp = 0;  /* previous FileContent's VirtualPtr, for linking */

    for (int64_t i = 0; i <= segment_idx; i++) {
        if (fc_vp == VFS_VPTR_NULL) {
            /* Segment doesn't exist yet.  If this is the target segment and
             * the caller is writing, allocate FileContent + one PageNode.
             * Otherwise allocate an empty FileContent (or return NULL). */
            if (i == segment_idx && !is_write) return NULL;

            int64_t page_root_vp = VFS_VPTR_NULL;
            if (i == segment_idx && is_write) {
                /* Lazy: allocate exactly one PageNode for the requested page */
                int64_t pn_vp = pool_alloc(&ctx->pool);
                if (pn_vp == VFS_VPTR_NULL) return NULL;
                uint8_t* pn_slot = pool_resolve_rw(&ctx->pool, pn_vp);
                if (!pn_slot) return NULL;
                nodes_write_pagenode(pn_slot, 0, 0, (uint32_t)page_in_segment, ctx->page_size);
                page_root_vp = pn_vp;
            }
            /* else: empty segment (page_root_vp stays 0) */

            /* Allocate FileContent entry */
            int64_t new_fc_vp = pool_alloc(&ctx->pool);
            if (new_fc_vp == VFS_VPTR_NULL) return NULL;
            uint8_t* fc_slot = pool_resolve_rw(&ctx->pool, new_fc_vp);
            if (!fc_slot) return NULL;
            nodes_write_filecontent(fc_slot, page_root_vp, 0, ctx->page_size);

            /* CAS-link into chain with release barrier */
            vfs_mb_release();
            if (i == 0) {
                int64_t expected = 0;
                int64_t desired = new_fc_vp;
                int64_t old = vfs_cas_i64(
                    (int64_t*)(file_slot + FILENODE_OFF_HEADPTR),
                    expected, desired);
                if (old != expected) {
                    fc_vp = old;
                    i--;  /* retry this segment */
                    continue;
                }
                fc_vp = new_fc_vp;
            } else {
                uint8_t* prev_slot = pool_resolve_rw(&ctx->pool, prev_fc_vp);
                if (prev_slot) {
                    int64_t expected = 0;
                    int64_t desired = new_fc_vp;
                    int64_t off = FILECONTENT_OFF_NEXTPTR;
                    int64_t old = vfs_cas_i64(
                        (int64_t*)(prev_slot + off), expected, desired);
                    if (old != expected) {
                        fc_vp = old;
                        i--;  /* retry this segment */
                        continue;
                    }
                }
                fc_vp = new_fc_vp;
            }
            if (i == segment_idx && is_write) {
                vfs_atomic_add_i32((int32_t*)(fc_slot + FILECONTENT_OFF_PAGECOUNT), 1);
                return pool_resolve_ro(&ctx->pool, page_root_vp);
            }
            /* Empty intermediate segment — advance to next */
            prev_fc_vp = fc_vp;
            fc_vp = 0;  /* next segment doesn't exist yet */
            continue;
        }

        uint8_t* fc_slot = pool_resolve_rw(&ctx->pool, fc_vp);
        if (!fc_slot) return NULL;

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

            int64_t fc_page_root = vfs_rd8_s(fc_slot, FILECONTENT_OFF_ROOTPTR, ctx->page_size);
            uint32_t fc_page_count = (uint32_t)vfs_rd4_s(fc_slot, FILECONTENT_OFF_PAGECOUNT, ctx->page_size);
            int64_t cache_key = (file_vp << 20) | (segment_idx & 0xFFFFF);

            /* Check tcache first */
            int cache_slot = -1;
            for (int ci = 0; ci < TCACHE_SIZE; ci++) {
                if (tcache[ci].key == cache_key && tcache[ci].populated) {
                    if (tcache[ci].gen != vfs_atomic_load_i64(&ctx->gc_generation)) {
                        tcache[ci].populated = false;
                    } else {
                        uint8_t* cached = segment_array_resolve(&ctx->pool, &tcache[ci].arr,
                                                                 (uint32_t)page_in_segment);
                        if (cached) return cached;
                        /* Array has NULL for this page — don't invalidate the
                           whole array.  Fall through to chain walk to find or
                           create the PageNode, then patch the array entry. */
                        cache_slot = ci;
                    }
                    break;
                }
            }

            /* Walk the sparse PageNode chain in ascending page_index order.
             * Track prev_vp for sorted insertion. */
            int64_t pn_vp = fc_page_root;
            int64_t prev_vp = 0;
            int total_pages_seen = 0;
            int64_t result_vp = 0;
#ifndef NDEBUG
            int64_t prev_page_index = -1;
#endif
            while (pn_vp != 0) {
                total_pages_seen++;
                uint8_t* pn_slot = pool_resolve_rw(&ctx->pool, pn_vp);
                if (!pn_slot) break;
                uint32_t pn_idx;
                int64_t pn_next;
                int64_t pn_ver_root;
                nodes_read_pagenode(pn_slot, &pn_ver_root, &pn_next, &pn_idx, ctx->page_size);
                (void)pn_ver_root;

#ifndef NDEBUG
                assert(prev_page_index < (int64_t)pn_idx);
                prev_page_index = (int64_t)pn_idx;
#endif

                if ((int64_t)pn_idx == page_in_segment) {
                    result_vp = pn_vp;
                    goto done;
                }

                if ((int64_t)pn_idx > page_in_segment) {
                    /* Found a higher-index node — insert before it */
                    if (!is_write) return NULL;

                    int64_t new_pn_vp = pool_alloc(&ctx->pool);
                    if (new_pn_vp == VFS_VPTR_NULL) return NULL;
                    uint8_t* new_slot = pool_resolve_rw(&ctx->pool, new_pn_vp);
                    if (!new_slot) return NULL;
                    nodes_write_pagenode(new_slot, 0, pn_vp,
                                         (uint32_t)page_in_segment, ctx->page_size);
                    vfs_mb_release();

                    if (prev_vp == 0) {
                        int64_t old_root = vfs_cas_i64(
                            (int64_t*)(fc_slot + FILECONTENT_OFF_ROOTPTR),
                            pn_vp, new_pn_vp);
                        if (old_root != pn_vp) {
                            fc_page_root = old_root;
                            goto retry_walk;
                        }
                    } else {
                        uint8_t* prev_slot = pool_resolve_rw(&ctx->pool, prev_vp);
                        if (!prev_slot) return NULL;
                        int64_t old_next = vfs_cas_i64(
                            (int64_t*)(prev_slot + PAGENODE_OFF_NEXTPTR),
                            pn_vp, new_pn_vp);
                        if (old_next != pn_vp) {
                            fc_page_root = vfs_rd8_s(fc_slot,
                                FILECONTENT_OFF_ROOTPTR, ctx->page_size);
                            goto retry_walk;
                        }
                    }
                    result_vp = new_pn_vp;
                    vfs_atomic_add_i32((int32_t*)(fc_slot + FILECONTENT_OFF_PAGECOUNT), 1);
                    goto done;
                }

                prev_vp = pn_vp;
                pn_vp = pn_next;
            }

            /* Chain ended without finding the page */
            if (!is_write) return NULL;

            /* Append at tail */
            {
                int64_t new_pn_vp = pool_alloc(&ctx->pool);
                if (new_pn_vp == VFS_VPTR_NULL) return NULL;
                uint8_t* new_slot = pool_resolve_rw(&ctx->pool, new_pn_vp);
                if (!new_slot) return NULL;
                nodes_write_pagenode(new_slot, 0, 0,
                                     (uint32_t)page_in_segment, ctx->page_size);
                vfs_mb_release();

                if (prev_vp == 0) {
                    int64_t old_root = vfs_cas_i64(
                        (int64_t*)(fc_slot + FILECONTENT_OFF_ROOTPTR),
                        0, new_pn_vp);
                    if (old_root != 0) {
                        fc_page_root = old_root;
                        goto retry_walk;
                    }
                } else {
                    uint8_t* tail_slot = pool_resolve_rw(&ctx->pool, prev_vp);
                    if (!tail_slot) return NULL;
                    int64_t old_next = vfs_cas_i64(
                        (int64_t*)(tail_slot + PAGENODE_OFF_NEXTPTR),
                        0, new_pn_vp);
                    if (old_next != 0) {
                        fc_page_root = vfs_rd8_s(fc_slot,
                            FILECONTENT_OFF_ROOTPTR, ctx->page_size);
                        goto retry_walk;
                    }
                }
                result_vp = new_pn_vp;
                vfs_atomic_add_i32((int32_t*)(fc_slot + FILECONTENT_OFF_PAGECOUNT), 1);
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
                (const int32_t*)(fc_slot + FILECONTENT_OFF_PAGECOUNT));
            if ((int)fc_page_count >= SPARSE_CACHE_THRESHOLD) {
                int slot = tcache_next % TCACHE_SIZE;
                if (tcache[slot].arr.built)
                    segment_array_destroy(&tcache[slot].arr);
                int rc = segment_array_build(&ctx->pool,
                    vfs_rd8_s(fc_slot, FILECONTENT_OFF_ROOTPTR, ctx->page_size),
                    seg_size, &tcache[slot].arr);
                if (rc == VFS_OK) {
                    tcache[slot].key = cache_key;
                    tcache[slot].populated = true;
                    tcache[slot].gen = vfs_atomic_load_i64(&ctx->gc_generation);
                    tcache_next++;
#ifndef NDEBUG
                    atomic_fetch_add(&tree_resolve_page_cache_builds, 1);
#endif
                }
            }
            return pool_resolve_rw(&ctx->pool, result_vp);

        retry_walk:
            /* CAS failed — re-walk the chain from the (possibly updated) root */
            {
                pn_vp = fc_page_root;
                prev_vp = 0;
                total_pages_seen = 0;
                (void)total_pages_seen;
#ifndef NDEBUG
                prev_page_index = -1;
#endif
                while (pn_vp != 0) {
                    total_pages_seen++;
                    uint8_t* pn_slot = pool_resolve_rw(&ctx->pool, pn_vp);
                    if (!pn_slot) break;
                    uint32_t pn_idx;
                    int64_t pn_next;
                    int64_t pn_ver_root;
                    nodes_read_pagenode(pn_slot, &pn_ver_root, &pn_next, &pn_idx, ctx->page_size);
                    (void)pn_ver_root;
#ifndef NDEBUG
                    assert(prev_page_index < (int64_t)pn_idx);
                    prev_page_index = (int64_t)pn_idx;
#endif
                    if ((int64_t)pn_idx == page_in_segment)
                        return pool_resolve_rw(&ctx->pool, pn_vp);
                    prev_vp = pn_vp;
                    pn_vp = pn_next;
                }
                /* Still not found — retry the whole segment resolution */
                i--;
                continue;
            }
        }

        prev_fc_vp = fc_vp;
        fc_vp = vfs_rd8_s(fc_slot, FILECONTENT_OFF_NEXTPTR, ctx->page_size);
    }

    return NULL;
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

    int64_t vp = versionRootPtr;

    while (vp != 0) {
        uint8_t* vp_slot = pool_resolve_ro(&ctx->pool, vp);
        if (!vp_slot) break;

        uint32_t vp_epoch;
        int64_t vp_dataPage, vp_next;
        nodes_read_versionpage(vp_slot, &vp_epoch, &vp_dataPage,
                               &vp_next, ctx->page_size);

        /* Compute effective epoch via mapper remapping */
        int64_t effective_epoch = (int64_t)vp_epoch;
        if (mapper_table_traversal_apply(&ctx->mapper_table, (int64_t)vp_epoch))
            effective_epoch = mapper_table_resolve(&ctx->mapper_table,
                                                    (int64_t)vp_epoch);

        /* Exact match always wins */
        if (effective_epoch == read_epoch)
            return vp_dataPage;

        /* Even epoch below read_epoch — chains are descending, so the
           first even below read_epoch is the highest such epoch.  Use it
           and stop. */
        if (effective_epoch < read_epoch && effective_epoch % 2 == 0)
            return vp_dataPage;

        vp = vp_next;
    }

    return -1;
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

    int64_t walk_vp = sizePtr;

    while (walk_vp != 0) {
        uint8_t* fs_slot = pool_resolve_ro(&ctx->pool, walk_vp);
        if (!fs_slot) break;

        uint32_t fs_epoch;
        int64_t fs_modified, fs_size, fs_next;
        nodes_read_filesize(fs_slot, &fs_epoch, &fs_modified, &fs_size,
                            &fs_next, ctx->page_size);

        /* Compute effective epoch via mapper remapping */
        int64_t effective_epoch = (int64_t)fs_epoch;
        if (mapper_table_traversal_apply(&ctx->mapper_table, (int64_t)fs_epoch))
            effective_epoch = mapper_table_resolve(&ctx->mapper_table,
                                                    (int64_t)fs_epoch);

        /* Exact match at read_epoch — use it immediately */
        if (effective_epoch == read_epoch) {
            if (out_fileSize) *out_fileSize = fs_size;
            if (out_modifiedAt) *out_modifiedAt = fs_modified;
            return;
        }

        /* Even epoch below read_epoch — chains are descending, so the
           first even below read_epoch is the highest such epoch. */
        if (effective_epoch < read_epoch && effective_epoch % 2 == 0) {
            if (out_fileSize) *out_fileSize = fs_size;
            if (out_modifiedAt) *out_modifiedAt = fs_modified;
            return;
        }

        walk_vp = fs_next;
    }

    /* No match found — return 0/defaults set at top of function. */
}

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

    /* Read parent DirNode, verify type */
    uint8_t* parent_slot = pool_resolve_rw(&ctx->pool, (int64_t)parent);

    if (!parent_slot) { vfs->ctx->last_error = VFS_ERR_NOTFOUND; return VFS_ERR_NOTFOUND; }
    if (vfs_rd2_s(parent_slot, DIRNODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_DIR) {
        vfs->ctx->last_error = VFS_ERR_NOTDIR;
        return VFS_ERR_NOTDIR;
        }

    /* Walk parent's DirContent chain, checking for name collision */
    int64_t headPtr = vfs_rd8_s(parent_slot, DIRNODE_OFF_HEADPTR, ctx->page_size);
    uint64_t target_hash = name_hash_compute(name, (int)strlen(name));
    int64_t walk_vp = headPtr;
    while (walk_vp != 0) {
        uint8_t* dc_slot = pool_resolve_rw(&ctx->pool, walk_vp);
        if (!dc_slot) break;
        uint32_t ce_child, ce_epoch;
        int64_t ce_childPtr, ce_namePtr, ce_next;
        nodes_read_dircontent(dc_slot, &ce_child, &ce_epoch, &ce_childPtr,
                              &ce_namePtr, &ce_next, ctx->page_size);
        (void)ce_child; (void)ce_childPtr;
        if (ce_epoch == (uint32_t)epoch && ce_namePtr != 0) {
            /* Hash fast-reject: skip strcmp if hashes don't match */
            uint64_t entry_hash = nodes_read_name_hash(&ctx->pool, ce_namePtr);
            if (entry_hash != target_hash) {
#ifdef VFS_NAME_HASH_TESTING
            s_hash_rejects++;
#endif
            walk_vp = ce_next; continue; }
            /* Read the name and compare */
            char entry_name[256];
            int name_len = nodes_read_name(&ctx->pool, ce_namePtr,
                                            entry_name, (int)sizeof(entry_name));
            if (name_len > 0 && strcmp(entry_name, name) == 0) {
                vfs->ctx->last_error = VFS_ERR_EXISTS;
                return VFS_ERR_EXISTS;
                }
        }
        walk_vp = ce_next;
    }

    /* Atomically increment nextNodeId */
    uint32_t new_nodeId = (uint32_t)vfs_atomic_add_i32((int32_t*)&ctx->nextNodeId, 1);
    /* nextNodeId starts at 0, first add yields nodeId=1 */
    if (vfs_lock(vfs, (int64_t)new_nodeId, epoch) != VFS_OK) {
        vfs->ctx->last_error = VFS_ERR_IO;
        return VFS_ERR_IO;
    }

    /* Allocate FileNode slot and write it */
    int64_t file_vp = pool_alloc(&ctx->pool);

    if (file_vp == VFS_VPTR_NULL) { vfs_unlock(vfs, (int64_t)new_nodeId, epoch); vfs->ctx->last_error = VFS_ERR_FULL; return VFS_ERR_FULL; }
    uint8_t* file_slot = pool_resolve_rw(&ctx->pool, file_vp);

    if (!file_slot) { vfs_unlock(vfs, (int64_t)new_nodeId, epoch); vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }
    nodes_write_filenode(file_slot, new_nodeId, 0, 0, (int64_t)time(NULL), ctx->page_size);

    /* Allocate NameEntry chain for the file name */
    int64_t name_vp;
    int name_slots = nodes_write_name(&ctx->pool, name, &name_vp);

    if (name_slots == 0) { vfs_unlock(vfs, (int64_t)new_nodeId, epoch); vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }

    /* Allocate DirContent slot outside the CAS loop to avoid leaks on retry */
    int64_t dc_vp = pool_alloc(&ctx->pool);

    if (dc_vp == VFS_VPTR_NULL) { vfs_unlock(vfs, (int64_t)new_nodeId, epoch); vfs->ctx->last_error = VFS_ERR_FULL; return VFS_ERR_FULL; }
    uint8_t* dc_slot = pool_resolve_rw(&ctx->pool, dc_vp);

    if (!dc_slot) { vfs_unlock(vfs, (int64_t)new_nodeId, epoch); vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }

    /* CAS-prepend DirContent to parent's headPtr */
    int64_t old_head;
    do {
        old_head = vfs_atomic_load_i64(
            (const int64_t*)(parent_slot + DIRNODE_OFF_HEADPTR));

        nodes_write_dircontent(dc_slot, new_nodeId, (uint32_t)epoch,
                               file_vp, name_vp, old_head, ctx->page_size);
        vfs_mb_release();
    } while (vfs_cas_i64((int64_t*)(parent_slot + DIRNODE_OFF_HEADPTR),
                         old_head, dc_vp) != old_head);

    dentry_cache_invalidate(&ctx->readdir_cache);
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

    uint8_t* parent_slot = pool_resolve_rw(&ctx->pool, (int64_t)parent);

    if (!parent_slot) { vfs->ctx->last_error = VFS_ERR_NOTFOUND; return VFS_ERR_NOTFOUND; }
    if (vfs_rd2_s(parent_slot, DIRNODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_DIR) {
        vfs->ctx->last_error = VFS_ERR_NOTDIR;
        return VFS_ERR_NOTDIR;
        }

    /* Walk DirContent chain, check for name collision */
    int64_t headPtr = vfs_rd8_s(parent_slot, DIRNODE_OFF_HEADPTR, ctx->page_size);
    uint64_t target_hash = name_hash_compute(name, (int)strlen(name));
    int64_t walk_vp = headPtr;
    while (walk_vp != 0) {
        uint8_t* dc_slot = pool_resolve_rw(&ctx->pool, walk_vp);
        if (!dc_slot) break;
        uint32_t cc, ce;
        int64_t cp, np, nx;
        nodes_read_dircontent(dc_slot, &cc, &ce, &cp, &np, &nx, ctx->page_size);
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
                return VFS_ERR_EXISTS;
                }
        }
        walk_vp = nx;
    }

    uint32_t new_nodeId = (uint32_t)vfs_atomic_add_i32(
        (int32_t*)&ctx->nextNodeId, 1);
    if (vfs_lock(vfs, (int64_t)new_nodeId, epoch) != VFS_OK) {
        vfs->ctx->last_error = VFS_ERR_IO;
        return VFS_ERR_IO;
    }

    int64_t dir_vp = pool_alloc(&ctx->pool);

    if (dir_vp == VFS_VPTR_NULL) { vfs_unlock(vfs, (int64_t)new_nodeId, epoch); vfs->ctx->last_error = VFS_ERR_FULL; return VFS_ERR_FULL; }
    uint8_t* dir_slot = pool_resolve_rw(&ctx->pool, dir_vp);

    if (!dir_slot) { vfs_unlock(vfs, (int64_t)new_nodeId, epoch); vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }
    nodes_write_dirnode(dir_slot, new_nodeId, 0, 0, ctx->page_size);

    int64_t name_vp;
    int name_slots = nodes_write_name(&ctx->pool, name, &name_vp);

    if (name_slots == 0) { vfs_unlock(vfs, (int64_t)new_nodeId, epoch); vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }

    int64_t dc_vp = pool_alloc(&ctx->pool);

    if (dc_vp == VFS_VPTR_NULL) { vfs_unlock(vfs, (int64_t)new_nodeId, epoch); vfs->ctx->last_error = VFS_ERR_FULL; return VFS_ERR_FULL; }
    uint8_t* dc_slot = pool_resolve_rw(&ctx->pool, dc_vp);

    if (!dc_slot) { vfs_unlock(vfs, (int64_t)new_nodeId, epoch); vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }

    int64_t old_head;
    do {
        old_head = vfs_atomic_load_i64(
            (const int64_t*)(parent_slot + DIRNODE_OFF_HEADPTR));
        nodes_write_dircontent(dc_slot, new_nodeId, (uint32_t)epoch,
                               dir_vp, name_vp, old_head, ctx->page_size);
        vfs_mb_release();
    } while (vfs_cas_i64((int64_t*)(parent_slot + DIRNODE_OFF_HEADPTR),
                         old_head, dc_vp) != old_head);

    dentry_cache_invalidate(&ctx->readdir_cache);
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

    uint8_t* parent_slot = pool_resolve_rw(&ctx->pool, (int64_t)parent);
    if (!parent_slot) { vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }

    if (vfs_lock(vfs, (int64_t)found_childId, epoch) != VFS_OK) return VFS_ERR_IO;

    /* Allocate tombstone DirContent slot outside CAS loop */
    int64_t dc_vp = pool_alloc(&ctx->pool);

    if (dc_vp == VFS_VPTR_NULL) { vfs_unlock(vfs, (int64_t)found_childId, epoch); vfs->ctx->last_error = VFS_ERR_FULL; return VFS_ERR_FULL; }
    uint8_t* dc_slot = pool_resolve_rw(&ctx->pool, dc_vp);

    if (!dc_slot) { vfs_unlock(vfs, (int64_t)found_childId, epoch); vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }

    /* CAS-prepend tombstone to parent's headPtr */
    int64_t old_head;
    do {
        old_head = vfs_atomic_load_i64(
            (const int64_t*)(parent_slot + DIRNODE_OFF_HEADPTR));

        /* Tombstone: namePtr=0 means deleted */
        nodes_write_dircontent(dc_slot, found_childId, (uint32_t)epoch,
                               found_childPtr, 0, old_head, ctx->page_size);
        vfs_mb_release();
    } while (vfs_cas_i64((int64_t*)(parent_slot + DIRNODE_OFF_HEADPTR),
                         old_head, dc_vp) != old_head);

    dentry_cache_invalidate(&ctx->readdir_cache);
    vfs_unlock(vfs, (int64_t)found_childId, epoch);
    return VFS_OK;
}

int vfs_rmdir(vfs_t* vfs, int64_t parent, const char* name, int64_t epoch) {
    if (!vfs || !vfs->ctx || !name || name[0] == '\0') return VFS_ERR_IO;
    TreeContext* ctx = vfs->ctx;

    if (!vfs_epoch_is_writable(ctx, (int64_t)epoch)) return VFS_ERR_IO;

    uint8_t* parent_slot = pool_resolve_rw(&ctx->pool, (int64_t)parent);

    if (!parent_slot) { vfs->ctx->last_error = VFS_ERR_NOTFOUND; return VFS_ERR_NOTFOUND; }
    if (vfs_rd2_s(parent_slot, DIRNODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_DIR) {
        vfs->ctx->last_error = VFS_ERR_NOTDIR;
        return VFS_ERR_NOTDIR;
        }

    int64_t headPtr = vfs_rd8_s(parent_slot, DIRNODE_OFF_HEADPTR, ctx->page_size);
    int64_t found_vp = 0;
    uint32_t found_childId = 0;
    int64_t found_childPtr = 0;
    uint64_t target_hash = name_hash_compute(name, (int)strlen(name));

    int64_t walk_vp = headPtr;
    while (walk_vp != 0 && found_vp == 0) {
        uint8_t* dc_slot = pool_resolve_ro(&ctx->pool, walk_vp);
        if (!dc_slot) break;
        uint32_t cc, ce;
        int64_t cp, np, nx;
        nodes_read_dircontent(dc_slot, &cc, &ce, &cp, &np, &nx, ctx->page_size);
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
            }
        }
        walk_vp = nx;
    }

    if (found_vp == 0) { vfs->ctx->last_error = VFS_ERR_NOTFOUND; return VFS_ERR_NOTFOUND; }
    if (vfs_lock(vfs, (int64_t)found_childId, epoch) != VFS_OK) return VFS_ERR_IO;

    uint8_t* child_slot = pool_resolve_ro(&ctx->pool, found_childPtr);

    if (!child_slot) { vfs_unlock(vfs, (int64_t)found_childId, epoch); vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }
    if (vfs_rd2_s(child_slot, DIRNODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_DIR) {
        vfs_unlock(vfs, (int64_t)found_childId, epoch);
        vfs->ctx->last_error = VFS_ERR_NOTDIR;
        return VFS_ERR_NOTDIR;
        }

    /* Check directory is empty using read-rule: for each childNodeId, find the
       entry at the highest epoch ≤ query_epoch.  If any such entry has
       namePtr ≠ 0, the directory is not empty (tombstones with namePtr=0
       indicate deleted entries). */
    int64_t child_head = vfs_rd8_s(child_slot, DIRNODE_OFF_HEADPTR, ctx->page_size);
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
            uint8_t* cs = pool_resolve_ro(&ctx->pool, cw);
            if (!cs) break;
            uint32_t ccc, cce;
            int64_t ccp, cnp, cnx;
            nodes_read_dircontent(cs, &ccc, &cce, &ccp, &cnp, &cnx,
                                  ctx->page_size);
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
                return VFS_ERR_NOTEMPTY;
            }
        }
    }

    int64_t dc_vp = pool_alloc(&ctx->pool);

    if (dc_vp == VFS_VPTR_NULL) { vfs_unlock(vfs, (int64_t)found_childId, epoch); vfs->ctx->last_error = VFS_ERR_FULL; return VFS_ERR_FULL; }
    uint8_t* dc_slot = pool_resolve_rw(&ctx->pool, dc_vp);

    if (!dc_slot) { vfs_unlock(vfs, (int64_t)found_childId, epoch); vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }

    int64_t old_head;
    do {
        old_head = vfs_atomic_load_i64(
            (const int64_t*)(parent_slot + DIRNODE_OFF_HEADPTR));
        nodes_write_dircontent(dc_slot, found_childId, (uint32_t)epoch,
                               found_childPtr, 0, old_head, ctx->page_size);
        vfs_mb_release();
    } while (vfs_cas_i64((int64_t*)(parent_slot + DIRNODE_OFF_HEADPTR),
                         old_head, dc_vp) != old_head);

    dentry_cache_invalidate(&ctx->readdir_cache);
    vfs_unlock(vfs, (int64_t)found_childId, epoch);
    return VFS_OK;
}

/* ---------------------------------------------------------------------------
 * dirchain_list — walk DirContent chain, collect non-tombstone entries
 * at epoch via read-rule dedup by childNodeId, fill vfs_dirent_t array.
 * --------------------------------------------------------------------------- */

int dirchain_list(TreeContext* ctx, int64_t dir_vp, int64_t epoch,
                  vfs_dirent_t* entries, int max) {
    if (!ctx || !entries || max <= 0) return VFS_ERR_IO;

    uint8_t* dir_slot = pool_resolve_ro(&ctx->pool, dir_vp);
    if (!dir_slot) return VFS_ERR_NOTFOUND;
    if (vfs_rd2_s(dir_slot, DIRNODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_DIR)
        return VFS_ERR_NOTDIR;

    int64_t read_epoch = mapper_table_resolve(&ctx->mapper_table, epoch);
    int64_t headPtr = vfs_rd8_s(dir_slot, DIRNODE_OFF_HEADPTR, ctx->page_size);

    /* Temporary per-child tracking arrays (max entries == max output) */
    int64_t best_child[DENTRY_CACHE_MAX];
    int64_t best_childPtr[DENTRY_CACHE_MAX];
    int64_t best_eff_epoch[DENTRY_CACHE_MAX];
    int     best_name_set[DENTRY_CACHE_MAX];
    int64_t best_namePtr[DENTRY_CACHE_MAX];   /* cached name VirtualPtr */
    int best_count = 0;

    int64_t walk_vp = headPtr;
    while (walk_vp != 0 && best_count < DENTRY_CACHE_MAX) {
        uint8_t* dc_slot = pool_resolve_ro(&ctx->pool, walk_vp);
        if (!dc_slot) break;
        uint32_t ce_child, ce_epoch;
        int64_t ce_childPtr, ce_namePtr, ce_next;
        nodes_read_dircontent(dc_slot, &ce_child, &ce_epoch, &ce_childPtr,
                              &ce_namePtr, &ce_next, ctx->page_size);

        int64_t eff_epoch = (int64_t)ce_epoch;
        if (mapper_table_traversal_apply(&ctx->mapper_table, (int64_t)ce_epoch))
            eff_epoch = mapper_table_resolve(&ctx->mapper_table, (int64_t)ce_epoch);

        int applies = (eff_epoch == read_epoch) ||
                      (eff_epoch < read_epoch && eff_epoch % 2 == 0);
        if (!applies) { walk_vp = ce_next; continue; }

        /* Find or insert in tracking arrays */
        int found = -1;
        for (int i = 0; i < best_count; i++) {
            if (best_child[i] == (int64_t)ce_child) { found = i; break; }
        }
        if (found >= 0) {
            if (eff_epoch > best_eff_epoch[found]) {
                best_eff_epoch[found] = eff_epoch;
                best_childPtr[found] = ce_childPtr;
                best_name_set[found] = (ce_namePtr != 0);
                best_namePtr[found] = ce_namePtr;
            }
        } else {
            best_child[best_count] = (int64_t)ce_child;
            best_childPtr[best_count] = ce_childPtr;
            best_eff_epoch[best_count] = eff_epoch;
            best_name_set[best_count] = (ce_namePtr != 0);
            best_namePtr[best_count] = ce_namePtr;
            best_count++;
        }
        walk_vp = ce_next;
    }

    /* Build output from tracking arrays, skipping tombstones */
    int written = 0;
    for (int i = 0; i < best_count && written < max; i++) {
        if (!best_name_set[i]) continue;
        entries[written].vp     = best_childPtr[i];
        entries[written].nodeId = best_child[i];
        entries[written].name[0] = '\0';
        entries[written].isDir = false;

        /* Determine isDir by reading child's type field */
        uint8_t* child_slot = pool_resolve_ro(&ctx->pool, best_childPtr[i]);
        if (child_slot) {
            int16_t ctype = vfs_rd2_s(child_slot, DIRNODE_OFF_TYPE, ctx->page_size);
            entries[written].isDir = (ctype == (int16_t)NODE_TYPE_DIR);
        }

        /* Read name from cached namePtr (collected in first pass) */
        if (best_namePtr[i] != 0)
            nodes_read_name(&ctx->pool, best_namePtr[i],
                            entries[written].name,
                            (int)sizeof(entries[written].name));
        written++;
    }
    return written;
}

int vfs_readdir(vfs_t* vfs, int64_t dir, vfs_dirent_t* entries,
                int max, int64_t epoch) {
    if (!vfs || !vfs->ctx || !entries || max <= 0) return VFS_ERR_IO;
    TreeContext* ctx = vfs->ctx;

    int n = dirchain_list(ctx, dir, epoch, entries, max);
    if (n < 0) {
        if (n == VFS_ERR_NOTFOUND) { vfs->ctx->last_error = VFS_ERR_NOTFOUND; return VFS_ERR_NOTFOUND; }
        if (n == VFS_ERR_NOTDIR)   { vfs->ctx->last_error = VFS_ERR_NOTDIR;   return VFS_ERR_NOTDIR; }
        vfs->ctx->last_error = VFS_ERR_IO;
        return VFS_ERR_IO;
    }
    (void)vfs;  /* unused except for error reporting through dirchain_list */
    return n;
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

    /* Verify both parents are DirNodes */
    uint8_t* src_slot = pool_resolve_rw(&ctx->pool, (int64_t)src_parent);

    if (!src_slot) { vfs->ctx->last_error = VFS_ERR_NOTFOUND; return VFS_ERR_NOTFOUND; }
    if (vfs_rd2_s(src_slot, DIRNODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_DIR) {
        vfs->ctx->last_error = VFS_ERR_NOTDIR;
        return VFS_ERR_NOTDIR;
        }

    uint8_t* dst_slot = pool_resolve_rw(&ctx->pool, (int64_t)dst_parent);

    if (!dst_slot) { vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }
    if (vfs_rd2_s(dst_slot, DIRNODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_DIR) {
        vfs->ctx->last_error = VFS_ERR_NOTDIR;
        return VFS_ERR_NOTDIR;
        }

    /* Find source entry using dirchain_find_child */
    uint32_t rn_childId = 0;
    int64_t rn_childPtr = 0;
    uint32_t found_epoch = 0;
    int r_rn = dirchain_find_child(ctx, src_parent, src, epoch,
                                   &rn_childPtr, &rn_childId, &found_epoch);
    if (r_rn == VFS_ERR_NOTFOUND) { vfs->ctx->last_error = VFS_ERR_NOTFOUND; return VFS_ERR_NOTFOUND; }
    if (r_rn == VFS_ERR_NOTDIR)   { vfs->ctx->last_error = VFS_ERR_NOTDIR;   return VFS_ERR_NOTDIR; }
    if (r_rn != VFS_OK)           { vfs->ctx->last_error = VFS_ERR_IO;       return VFS_ERR_IO; }

    if (vfs_lock(vfs, (int64_t)rn_childId, epoch) != VFS_OK) return VFS_ERR_IO;

    if (src_parent == dst_parent && found_epoch == (uint32_t)epoch) {
        int64_t new_name_vp;
        int ns = nodes_write_name(&ctx->pool, dst, &new_name_vp);
        if (ns == 0) { vfs_unlock(vfs, (int64_t)rn_childId, epoch); vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }

        int64_t walk_vp = vfs_rd8_s(src_slot, DIRNODE_OFF_HEADPTR, ctx->page_size);
        while (walk_vp != 0) {
            uint8_t* dc = pool_resolve_rw(&ctx->pool, walk_vp);
            if (!dc) break;
            uint32_t cc, ce;
            int64_t cp, np, nx;
            nodes_read_dircontent(dc, &cc, &ce, &cp, &np, &nx, ctx->page_size);
            if (cp == rn_childPtr && np != 0 && ce <= (uint32_t)epoch) {
                vfs_mb_release();
                vfs_atomic_store_i64((int64_t*)(dc + DIRCONTENT_OFF_NAMEPTR), new_name_vp);
                dentry_cache_invalidate(&ctx->readdir_cache);
                vfs_unlock(vfs, (int64_t)rn_childId, epoch);
                return VFS_OK;
            }
            walk_vp = nx;
        }
        vfs->ctx->last_error = VFS_ERR_IO;
        vfs_unlock(vfs, (int64_t)rn_childId, epoch);
        return VFS_ERR_IO;
    }

    /* Cross-directory rename: create new entry at dst, tombstone at src.
       For same-directory cross-epoch: skip tombstone — the old entry at lower
       epoch is naturally hidden by read-rule. */
    int cross_dir = (src_parent != dst_parent);
    int64_t dst_name_vp;
    int dst_ns = nodes_write_name(&ctx->pool, dst, &dst_name_vp);

    if (dst_ns == 0) { vfs_unlock(vfs, (int64_t)rn_childId, epoch); vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }

    /* Allocate DirContent for dst */
    int64_t dst_dc_vp = pool_alloc(&ctx->pool);

    if (dst_dc_vp == VFS_VPTR_NULL) { vfs_unlock(vfs, (int64_t)rn_childId, epoch); vfs->ctx->last_error = VFS_ERR_FULL; return VFS_ERR_FULL; }
    uint8_t* dst_dc_slot = pool_resolve_rw(&ctx->pool, dst_dc_vp);

    if (!dst_dc_slot) { vfs_unlock(vfs, (int64_t)rn_childId, epoch); vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }

    /* CAS-prepend to dst_parent's headPtr */
    int64_t dst_old_head;
    do {
        dst_old_head = vfs_atomic_load_i64(
            (const int64_t*)(dst_slot + DIRNODE_OFF_HEADPTR));
        nodes_write_dircontent(dst_dc_slot, rn_childId, (uint32_t)epoch,
                               rn_childPtr, dst_name_vp, dst_old_head,
                               ctx->page_size);
        vfs_mb_release();
    } while (vfs_cas_i64((int64_t*)(dst_slot + DIRNODE_OFF_HEADPTR),
                         dst_old_head, dst_dc_vp) != dst_old_head);

    /* Create tombstone at src (cross-directory only) */
    if (cross_dir) {
        int64_t src_dc_vp = pool_alloc(&ctx->pool);
        if (src_dc_vp == VFS_VPTR_NULL) { vfs_unlock(vfs, (int64_t)rn_childId, epoch); vfs->ctx->last_error = VFS_ERR_FULL; return VFS_ERR_FULL; }
    uint8_t* src_dc_slot = pool_resolve_rw(&ctx->pool, src_dc_vp);

    if (!src_dc_slot) { vfs_unlock(vfs, (int64_t)rn_childId, epoch); vfs->ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO; }

    int64_t src_old_head;
    do {
        src_old_head = vfs_atomic_load_i64(
            (const int64_t*)(src_slot + DIRNODE_OFF_HEADPTR));
        nodes_write_dircontent(src_dc_slot, rn_childId, (uint32_t)epoch,
                               rn_childPtr, 0, src_old_head, ctx->page_size);
        vfs_mb_release();
    } while (vfs_cas_i64((int64_t*)(src_slot + DIRNODE_OFF_HEADPTR),
                         src_old_head, src_dc_vp) != src_old_head);
    } /* cross_dir */

    dentry_cache_invalidate(&ctx->readdir_cache);
    vfs_unlock(vfs, (int64_t)rn_childId, epoch);
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
        uint8_t* slot = pool_resolve_ro(pool, nodeVP);
        if (!slot) return 0;

        uint8_t hashNibble, nodeType;
        int64_t listVP, nextVP;
        nodes_read_dircontentindex(slot, &hashNibble, &nodeType,
                                   &listVP, &nextVP, page_size);

        if (nodeType == NODE_TYPE_INDEX_LEAF) {
            /* Reached the leaf — return its DirContentLink list head */
            return listVP;
        }

        /* INTERNAL node — find the child matching this level's nibble */
        int target = dircontentindex_extract_nibble(nameHash, level);
        int64_t childVP = 0;

        /* Walk the child list at this level (linked via nextVP) */
        int64_t childWalk = listVP;
        while (childWalk != 0) {
            uint8_t* childSlot = pool_resolve_ro(pool, childWalk);
            if (!childSlot) return 0;

            uint8_t childHashNibble, childNodeType;
            int64_t childListVP, childNextVP;
            nodes_read_dircontentindex(childSlot, &childHashNibble,
                                       &childNodeType, &childListVP,
                                       &childNextVP, page_size);

            if (childHashNibble == target) {
                childVP = childListVP;  /* follow into this child's subtree */
                break;
            }
            childWalk = childNextVP;
        }

        if (childVP == 0) return 0;  /* no child for this nibble */
        nodeVP = childVP;
    }

    return 0;  /* exhausted levels without reaching a leaf */
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

    uint8_t* dir_slot = pool_resolve_ro(&ctx->pool, dir_vp);
    if (!dir_slot) return VFS_ERR_NOTFOUND;
    if (vfs_rd2_s(dir_slot, DIRNODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_DIR)
        return VFS_ERR_NOTDIR;

    int64_t read_epoch = mapper_table_resolve(&ctx->mapper_table, epoch);
    int64_t headPtr = vfs_rd8_s(dir_slot, DIRNODE_OFF_HEADPTR, ctx->page_size);

    int64_t best_child = 0, best_childPtr = 0, best_eff_epoch = 0;
    int64_t best_raw_epoch = 0;
    int best_name_match = 0;

    int64_t walk_vp = headPtr;
    while (walk_vp != 0) {
        uint8_t* dc_slot = pool_resolve_ro(&ctx->pool, walk_vp);
        if (!dc_slot) break;
        uint32_t ce_child, ce_epoch;
        int64_t ce_childPtr, ce_namePtr, ce_next;
        nodes_read_dircontent(dc_slot, &ce_child, &ce_epoch, &ce_childPtr,
                              &ce_namePtr, &ce_next, ctx->page_size);

        int64_t eff_epoch = (int64_t)ce_epoch;
        if (mapper_table_traversal_apply(&ctx->mapper_table, (int64_t)ce_epoch))
            eff_epoch = mapper_table_resolve(&ctx->mapper_table, (int64_t)ce_epoch);

        int applies = (eff_epoch == read_epoch) ||
                      (eff_epoch < read_epoch && eff_epoch % 2 == 0);
        if (!applies) { walk_vp = ce_next; continue; }

        if ((int64_t)ce_child == best_child && best_name_match &&
            eff_epoch <= best_eff_epoch)
            { walk_vp = ce_next; continue; }

        if (eff_epoch > best_eff_epoch || (int64_t)ce_child != best_child ||
            (ce_namePtr != 0 && best_name_match == 0 && eff_epoch > best_eff_epoch)) {
            if (ce_namePtr != 0) {
                /* Hash fast-reject: skip strcmp if hashes don't match */
                uint64_t entry_hash = nodes_read_name_hash(&ctx->pool, ce_namePtr);
                if (entry_hash != target_hash) {
#ifdef VFS_NAME_HASH_TESTING
                s_hash_rejects++;
#endif
                walk_vp = ce_next; continue; }
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
            } else {
                if ((int64_t)ce_child != best_child && best_name_match == 0) {
                    best_child    = (int64_t)ce_child;
                    best_childPtr = ce_childPtr;
                    best_eff_epoch = eff_epoch;
                    best_raw_epoch = (int64_t)ce_epoch;
                    best_name_match = 0;
                }
            }
        }
        walk_vp = ce_next;
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

    uint8_t* file_slot = pool_resolve_ro(&ctx->pool, (int64_t)file);
    if (!file_slot) {
        vfs->ctx->last_error = VFS_ERR_NOTFOUND;
        return -1;
    }
    if (vfs_rd2_s(file_slot, FILENODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_FILE) {
        vfs->ctx->last_error = VFS_ERR_IO;
        return -1;
    }

    int64_t sizePtr = vfs_rd8_s(file_slot, FILENODE_OFF_SIZEPTR, ctx->page_size);
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

    uint8_t* file_slot = pool_resolve_ro(&ctx->pool, (int64_t)file);
    if (!file_slot) {
        vfs->ctx->last_error = VFS_ERR_NOTFOUND;
        return -1;
    }
    if (vfs_rd2_s(file_slot, FILENODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_FILE) {
        vfs->ctx->last_error = VFS_ERR_IO;
        return -1;
    }

    int64_t sizePtr = vfs_rd8_s(file_slot, FILENODE_OFF_SIZEPTR, ctx->page_size);
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

    uint8_t* file_slot = pool_resolve_rw(&ctx->pool, file);
    if (!file_slot) {
        ctx->last_error = VFS_ERR_NOTFOUND;
        return VFS_ERR_NOTFOUND;
    }
    if (vfs_rd2_s(file_slot, FILENODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_FILE) {
        ctx->last_error = VFS_ERR_IO;
        return VFS_ERR_IO;
    }

    int64_t cur_size = vfs_file_size(vfs, file, epoch);
    if (new_size == cur_size) return VFS_OK;

    if (new_size < cur_size) {
        /* Shrink — append a new FileSize entry at epoch with new_size.
           Page reclamation is deferred to GC. */
        while (1) {
            int64_t old_sizePtr = vfs_atomic_load_i64(
                (const int64_t*)(file_slot + FILENODE_OFF_SIZEPTR));
            int64_t fs_vp = pool_alloc(&ctx->pool);
            if (fs_vp == VFS_VPTR_NULL) {
                ctx->last_error = VFS_ERR_NOMEM;
                return VFS_ERR_NOMEM;
            }
            uint8_t* fs_slot = pool_resolve_rw(&ctx->pool, fs_vp);
            if (!fs_slot) {
                ctx->last_error = VFS_ERR_NOMEM;
                return VFS_ERR_NOMEM;
            }
            nodes_write_filesize(fs_slot, (uint32_t)epoch, (int64_t)time(NULL),
                                 new_size, old_sizePtr, ctx->page_size);
            vfs_mb_release();
            int64_t cas_res = vfs_cas_i64(
                (int64_t*)(file_slot + FILENODE_OFF_SIZEPTR),
                old_sizePtr, fs_vp);
            if (cas_res == old_sizePtr) break;
        }
        ctx->last_error = VFS_OK;
        return VFS_OK;
    }

    /* Grow — write zeros from cur_size to new_size via vfs_write
       (which handles page allocation and FileSize updates internally).
       Buffer size is one page (8 KiB) to keep stack usage low — FUSE
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

    uint8_t* file_slot = pool_resolve_ro(&ctx->pool, (int64_t)file);
    if (!file_slot) {
        vfs->ctx->last_error = VFS_ERR_NOTFOUND;
        return -1;
    }
    if (vfs_rd2_s(file_slot, FILENODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_FILE) {
        vfs->ctx->last_error = VFS_ERR_IO;
        return -1;
    }
    return nodes_read_filenode_ctime(file_slot, ctx->page_size);
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

    uint8_t* file_slot = pool_resolve_rw(&ctx->pool, file);
    if (!file_slot) {
        vfs->ctx->last_error = VFS_ERR_NOTFOUND;
        return -1;
    }
    if (vfs_rd2_s(file_slot, FILENODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_FILE) {
        vfs->ctx->last_error = VFS_ERR_IO;
        return -1;
    }

    if (vfs_lock(vfs, file, epoch) != VFS_OK) return -1;

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

    uint32_t file_nodeId = (uint32_t)vfs_rd4_s(file_slot, FILENODE_OFF_NODEID, ctx->page_size);

    for (int64_t p = first_page; p <= last_page; p++) {
        /* Resolve or create PageNode for this page */
        uint8_t* pn_slot = tree_resolve_page(ctx, file, p, epoch, true);
        if (!pn_slot) { vfs_unlock(vfs, file, epoch); return -1; }

        /* Compute intra-page offset and count */
        int64_t page_offset = (p == first_page) ? offset % page_size : 0;
        int64_t page_count = (int64_t)page_size - page_offset;
        if (remaining < page_count) page_count = remaining;

        while (1) {  /* retry loop for CAS */
            /* Walk version chain searching for existing write at this epoch */
            int64_t version_root = vfs_atomic_load_i64(
                (const int64_t*)(pn_slot + PAGENODE_OFF_VERSIONROOT));
            int64_t vp = version_root;
            int64_t data_page = -1;
            int found_in_place = 0;

            while (vp != 0) {
                uint8_t* vp_slot = pool_resolve_ro(&ctx->pool, vp);
                if (!vp_slot) break;
                uint32_t vp_epoch;
                int64_t vp_dataPage, vp_next;
                nodes_read_versionpage(vp_slot, &vp_epoch, &vp_dataPage, &vp_next, ctx->page_size);
                if (vp_epoch == (uint32_t)epoch) {
                    data_page = vp_dataPage;
                    found_in_place = 1;
                    break;
                }
                vp = vp_next;
            }

            if (found_in_place) {
                /* In-place write: read current page, overlay, write back */
                uint8_t* page_buf = storage_read(ctx->sb, data_page);
                if (!page_buf) { vfs_unlock(vfs, file, epoch); return -1; }
                memcpy(page_buf + page_offset, src, (size_t)page_count);
                storage_write(ctx->sb, data_page, page_buf, 0);
                break;  /* exit retry loop — in-place succeeded */
            }

            /* COW: find base page (highest even epoch < write_epoch).
               VersionPages are prepended (newest first), so the first
               match is the highest even epoch. */
            int64_t base_page = -1;
            vp = version_root;
            while (vp != 0) {
                uint8_t* vp_slot = pool_resolve_ro(&ctx->pool, vp);
                if (!vp_slot) break;
                uint32_t vp_epoch;
                int64_t vp_dataPage, vp_next;
                nodes_read_versionpage(vp_slot, &vp_epoch, &vp_dataPage, &vp_next, ctx->page_size);
                if (vp_epoch < (uint32_t)epoch && vp_epoch % 2 == 0) {
                    base_page = vp_dataPage;
                    break;
                }
                vp = vp_next;
            }

            /* Allocate new data page */
            int64_t new_dp = storage_allocate(ctx->sb, 1);
            if (new_dp < 0) { vfs_unlock(vfs, file, epoch); return -1; }

            /* Read or zero-fill the full page */
            uint8_t* page_buf = (uint8_t*)malloc((size_t)page_size);
            if (!page_buf) { vfs_unlock(vfs, file, epoch); return -1; }

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

            /* Create VersionPage */
            int64_t vp_new = pool_alloc(&ctx->pool);
            if (vp_new == VFS_VPTR_NULL) { vfs_unlock(vfs, file, epoch); return -1; }
            uint8_t* vp_new_slot = pool_resolve_rw(&ctx->pool, vp_new);
            if (!vp_new_slot) { vfs_unlock(vfs, file, epoch); return -1; }
            nodes_write_versionpage(vp_new_slot, (uint32_t)epoch, new_dp,
                                    version_root, ctx->page_size);

            /* Release barrier then CAS */
            vfs_mb_release();
            int64_t old_root = vfs_cas_i64(
                (int64_t*)(pn_slot + PAGENODE_OFF_VERSIONROOT),
                version_root, vp_new);
            if (old_root == version_root) {
                break;  /* CAS succeeded — exit retry loop */
            }
            /* CAS failed — retry loop will re-read version_root and try again.
               Our orphaned data page and VersionPage will be reclaimed by GC. */
        }

        src += page_count;
        remaining -= page_count;
    }

    /* Update FileSize if file grew */
    if (grew) {
        while (1) {  /* retry loop for CAS */
            int64_t old_sizePtr = vfs_atomic_load_i64(
                (const int64_t*)(file_slot + FILENODE_OFF_SIZEPTR));
            int64_t fs_vp = pool_alloc(&ctx->pool);
            if (fs_vp == VFS_VPTR_NULL) { vfs_unlock(vfs, file, epoch); return -1; }
            uint8_t* fs_slot = pool_resolve_rw(&ctx->pool, fs_vp);
            if (!fs_slot) { vfs_unlock(vfs, file, epoch); return -1; }
            nodes_write_filesize(fs_slot, (uint32_t)epoch, (int64_t)time(NULL),
                                 new_size, old_sizePtr, ctx->page_size);
            vfs_mb_release();
            int64_t cas_res = vfs_cas_i64(
                (int64_t*)(file_slot + FILENODE_OFF_SIZEPTR),
                old_sizePtr, fs_vp);
            if (cas_res == old_sizePtr) break;
            /* CAS failed — retry with fresh old_sizePtr */
        }
    }

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

    uint8_t* file_slot = pool_resolve_ro(&ctx->pool, file);
    if (!file_slot) {
        vfs->ctx->last_error = VFS_ERR_NOTFOUND;
        return -1;
    }
    if (vfs_rd2_s(file_slot, FILENODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_FILE) {
        vfs->ctx->last_error = VFS_ERR_IO;
        return -1;
    }

    int64_t page_size = ctx->page_size;
    int64_t first_page = offset / page_size;
    int64_t last_page  = (offset + count - 1) / page_size;
    uint8_t* dst = (uint8_t*)buf;
    int64_t remaining = count;

    for (int64_t p = first_page; p <= last_page; p++) {
        /* Resolve PageNode for this page */
        uint8_t* pn_slot = tree_resolve_page(ctx, file, p, read_epoch, false);
        if (!pn_slot) {
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
            (const int64_t*)(pn_slot + PAGENODE_OFF_VERSIONROOT));
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
