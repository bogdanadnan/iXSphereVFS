/* Phase 7: GC — Tree Lock, Deferred Free Queue */
#include "gc.h"
#include "tree.h"
#include "nodes.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------
 * Per-type field descriptors (Phase 26 / W5e, ISSUES.md M4 fix).
 *
 * The old blind 8-byte remap of offsets 0/8/16/24 in gc_copy_entry
 * could corrupt non-VP fields.  E.g., DirNode.createdAt at offset 24
 * is a Unix timestamp that could coincidentally match a VP being
 * remapped.  With Anchor types the problem is worse: anchors pack
 * type/flags/id/count/sibPtr at offsets that overlap with VP fields
 * in other types.
 *
 * The descriptor table below tells gc_copy_entry which offsets hold
 * VirtualPtrs (must be remapped) vs. other types (must be left
 * alone).  Per-type — for each node/anchor kind, an array of
 * vp_offsets.
 * --------------------------------------------------------------------------- */

typedef struct {
    int      vp_offset_count;
    uint8_t  vp_offsets[8];  /* at most 8 VPs per 32-byte slot */
} GCFieldDesc;

static const GCFieldDesc gc_field_desc[] = {
    /* DIRNODE: HEADPTR (8), INDEXHEADPTR (16).  createdAt (24) is NOT a VP. */
    [ANCHOR_KIND_ROOT_DIR]      = { 2, { 8, 16, 0, 0, 0, 0, 0, 0 } },
    /* FILENODE: HEADPTR (8), SIZEPTR (16).  createdAt (24) is NOT. */
    [ANCHOR_KIND_ROOT_FILE]     = { 2, { 8, 16, 0, 0, 0, 0, 0, 0 } },
    /* SEGMENT_FILE: headPtr (8) is first PageNode, sibPtr (16) is next Segment. */
    [ANCHOR_KIND_SEGMENT_FILE]  = { 2, { 8, 16, 0, 0, 0, 0, 0, 0 } },
    /* SEGMENT_DIR: same shape. */
    [ANCHOR_KIND_SEGMENT_DIR]   = { 2, { 8, 16, 0, 0, 0, 0, 0, 0 } },
    /* UNIT_PAGE: VERSIONROOT (8) and nextPtr (24) are VPs. */
    [ANCHOR_KIND_UNIT_PAGE]     = { 2, { 8, 24, 0, 0, 0, 0, 0, 0 } },
    /* UNIT_SLOT: headPtr (8) is first DirContent, sibPtr (16) is next SlotNode. */
    [ANCHOR_KIND_UNIT_SLOT]     = { 2, { 8, 16, 0, 0, 0, 0, 0, 0 } },
};

/* Leaf types (VersionPage / DirContent / FileSize) all share the
 * unified LEAF layout: epoch (0,4), kind_specific (4,4), primary_ptr (8,8),
 * secondary_ptr (16,8), nextPtr (24,8).  The 8-byte aligned VP offsets
 * are 8, 16, 24.  We use a single descriptor for all leaf types. */
static const GCFieldDesc gc_leaf_desc = { 3, { 8, 16, 24, 0, 0, 0, 0, 0 } };

/* ---------------------------------------------------------------------------

/* ---------------------------------------------------------------------------
 * Tree Lock (§9.6)
 *
 * treeLockState bit layout:
 *   Bit 63:     GC exclusive lock (1 = held)
 *   Bits 32-62: Reader count (30 bits)
 *   Bits 0-31:  Unused
 * --------------------------------------------------------------------------- */

void tree_lock_acquire_shared(TreeContext* ctx) {
    for (;;) {
        int64_t old = vfs_atomic_load_i64(&ctx->treeLockState);
        /* Spin while exclusive lock is held (bit 63 set) */
        while (old & TREE_LOCK_EXCLUSIVE_BIT) {
            old = vfs_atomic_load_i64(&ctx->treeLockState);
        }
        /* Try to CAS-increment the reader count */
        int64_t desired = old + TREE_LOCK_READER_INC;
        int64_t cas = vfs_cas_i64(&ctx->treeLockState, old, desired);
        if (cas == old) return;  /* success */
        /* CAS failed — retry */
    }
}

void tree_lock_release_shared(TreeContext* ctx) {
    /* Atomically decrement reader count by one reader unit */
    vfs_atomic_add_i64(&ctx->treeLockState, -((int64_t)TREE_LOCK_READER_INC));
}

void tree_lock_acquire_exclusive(TreeContext* ctx) {
    for (;;) {
        int64_t old = vfs_atomic_load_i64(&ctx->treeLockState);
        /* Spin while exclusive is held (another GC is running) */
        while (old & TREE_LOCK_EXCLUSIVE_BIT) {
            old = vfs_atomic_load_i64(&ctx->treeLockState);
        }
        /* Try to CAS-set the exclusive bit */
        int64_t desired = old | (int64_t)TREE_LOCK_EXCLUSIVE_BIT;
        int64_t cas = vfs_cas_i64(&ctx->treeLockState, old, desired);
        if (cas == old) break;  /* exclusive bit acquired */
        /* CAS failed — retry */
    }

    /* Spin-wait for readers to drain (reader count goes to 0) */
    for (;;) {
        int64_t state = vfs_atomic_load_i64(&ctx->treeLockState);
        /* Keep only the reader bits, check if zero */
        int64_t readers = state & (int64_t)TREE_LOCK_READER_MASK;
        if (readers == 0) return;
        /* Spin */
    }
}

void tree_lock_release_exclusive(TreeContext* ctx) {
    /* Release barrier ensures all writes performed under exclusive lock
       are globally visible before the lock appears released. */
    vfs_mb_release();
    vfs_atomic_add_i64(&ctx->treeLockState,
                       -((int64_t)TREE_LOCK_EXCLUSIVE_BIT));
}

/* ---------------------------------------------------------------------------
 * Deferred Free Queue (§7.3)
 * --------------------------------------------------------------------------- */

int deferred_free_init(DeferredFreeQueue* queue, int initial_capacity) {
    if (!queue || initial_capacity <= 0) return VFS_ERR_IO;

    queue->pages = (int64_t*)malloc((size_t)initial_capacity * sizeof(int64_t));
    if (!queue->pages) return VFS_ERR_NOMEM;

    queue->count = 0;
    queue->capacity = initial_capacity;
    queue->confirmed = false;
    return VFS_OK;
}

void deferred_free_enqueue(DeferredFreeQueue* queue, int64_t logical_page,
                            StorageBackend* sb) {
    if (!queue) return;

    /* Helper to append a single page, growing the array if needed */
    int append_ok = 0;
    do {
        if (queue->count >= queue->capacity) {
            int new_cap = queue->capacity * 2 + 16;
            int64_t* new_pages = (int64_t*)realloc(queue->pages,
                                    (size_t)new_cap * sizeof(int64_t));
            if (!new_pages) break;  /* OOM — skip */
            queue->pages = new_pages;
            queue->capacity = new_cap;
        }
        queue->pages[queue->count++] = logical_page;
        append_ok = 1;
    } while (0);

    if (!append_ok) {
#ifndef NDEBUG
        fprintf(stderr, "vfs: deferred_free_enqueue: OOM, page %lld lost\n",
                (long long)logical_page);
#endif
    }

    /* Enqueue mirror sibling if it exists — read from on-disk PageHeader */
    if (append_ok && sb) {
        int64_t offset = indir_lookup(sb, logical_page);
        if (offset > 0) {
            PageHeader ph;
            ssize_t n = pread(sb->fd, &ph, PAGE_HEADER_SIZE, offset);
            if (n == PAGE_HEADER_SIZE && ph.mirror_page >= 0) {
                int64_t mirror_page = (int64_t)ph.mirror_page;
                if (queue->count >= queue->capacity) {
                    int new_cap = queue->capacity * 2 + 16;
                    int64_t* new_pages = (int64_t*)realloc(queue->pages,
                                            (size_t)new_cap * sizeof(int64_t));
                    if (new_pages) {
                        queue->pages = new_pages;
                        queue->capacity = new_cap;
                        queue->pages[queue->count++] = mirror_page;
                    }
                } else {
                    queue->pages[queue->count++] = mirror_page;
                }
            }
        }
    }
}

bool deferred_free_is_queued(DeferredFreeQueue* queue, int64_t logical_page) {
    if (!queue || !queue->pages) return false;
    for (int i = 0; i < queue->count; i++) {
        if (queue->pages[i] == logical_page) return true;
    }
    return false;
}

void deferred_free_confirm_and_release(DeferredFreeQueue* queue,
                                        StorageBackend* sb) {
    if (!queue || !sb) return;
    for (int i = 0; i < queue->count; i++) {
        storage_free(sb, queue->pages[i]);
    }
    free(queue->pages);
    queue->pages = NULL;
    queue->count = 0;
    queue->capacity = 0;
    queue->confirmed = true;
}

void deferred_free_destroy(DeferredFreeQueue* queue) {
    if (!queue) return;
    free(queue->pages);
    queue->pages = NULL;
    queue->count = 0;
    queue->capacity = 0;
    queue->confirmed = false;
}

/* ---------------------------------------------------------------------------
 * GC pool page allocation helper
 * --------------------------------------------------------------------------- */

int64_t gc_allocate_new_pool_page(TreeContext* ctx, void* gc_map) {
    if (!ctx) return VFS_VPTR_NULL;
    (void)gc_map;  /* used in Phase 8 to record old→new VirtualPtr mapping */

    /* Allocate a fresh logical page from the storage backend */
    int64_t page_idx = storage_allocate(ctx->sb, 1);
    if (page_idx < 0) return VFS_VPTR_NULL;

    /* Get a pointer to the page payload (page cache will allocate on read) */
    uint8_t* payload = storage_read(ctx->sb, page_idx);
    if (!payload) {
        storage_free(ctx->sb, page_idx);
        return VFS_VPTR_NULL;
    }

    /* Initialize the pool page header */
    pool_page_init(payload, ctx->sb->page_size);

    /* Link the new page into the pool's free list */
    pool_list_add(&ctx->pool, page_idx, payload);

    /* Return the VirtualPtr of slot 0 on the new page (page_idx, slot=0) */
    return VFS_VPTR_MAKE(page_idx, 0);
}

/* ---------------------------------------------------------------------------
 * Entry copy with VirtualPtr remapping
 * --------------------------------------------------------------------------- */

void gc_copy_entry(GCMap* gc_map, int64_t old_vp, int64_t new_vp,
                   const uint8_t* old_slot, uint8_t* new_slot,
                   int64_t page_size) {
    if (!gc_map || !old_slot) return;

    /* Record the mapping: old_vp → new_vp */
    gc_map_put(gc_map, old_vp, new_vp);

    if (new_slot) {
        /* Copy the 32-byte slot */
        memcpy(new_slot, old_slot, VFS_POOL_SLOT_SIZE);

        /* W5e: per-type field descriptor (ISSUES.md M4 fix).
         *
         * Use the type field at offset 0 to select a descriptor.  The
         * old blind 8-byte remap could corrupt non-VP fields (e.g.,
         * DirNode.createdAt at offset 24, or Anchor fields at
         * type/flags/id/count/sibPtr).  The descriptor knows which
         * offsets are VPs.
         *
         * For leaf types (VersionPage / DirContent / FileSize), all
         * share the same unified layout (LEAF_OFF_PRIMARY=8,
         * LEAF_OFF_SECONDARY=16, LEAF_OFF_NEXTPTR=24), so we use a
         * single descriptor for all of them.
         */
        uint16_t type16 = (uint16_t)vfs_rd2_s(new_slot, 0, page_size);
        const GCFieldDesc* desc = NULL;
        if (type16 == (uint16_t)ANCHOR_KIND_ROOT_DIR ||
            type16 == (uint16_t)ANCHOR_KIND_ROOT_FILE ||
            type16 == (uint16_t)ANCHOR_KIND_SEGMENT_FILE ||
            type16 == (uint16_t)ANCHOR_KIND_SEGMENT_DIR ||
            type16 == (uint16_t)ANCHOR_KIND_UNIT_PAGE ||
            type16 == (uint16_t)ANCHOR_KIND_UNIT_SLOT) {
            desc = &gc_field_desc[type16];
        } else {
            /* Leaf type (VersionPage / DirContent / FileSize). */
            desc = &gc_leaf_desc;
        }
        for (int i = 0; i < desc->vp_offset_count; i++) {
            int off = desc->vp_offsets[i];
            int64_t val = vfs_rd8_s(new_slot, off, page_size);
            if (val == 0) continue;  /* skip null — not in map */
            int64_t mapped = gc_map_get(gc_map, val);
            if (mapped != val) {
                vfs_wr8_s(new_slot, off, mapped, page_size);
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * GC root scan — shadow-compaction (§12.5)
 *
 * Placeholder: walks are deferred to Phase 8.  The structure below
 * demonstrates the expected pattern.
 * --------------------------------------------------------------------------- */

int gc_walk_dirnode(TreeContext* ctx, GCMap* gc_map, GCAllocCursor* alloc,
                    int64_t dir_vp, int64_t epoch,
                    LivePageSet* lps) {
    if (!ctx || !gc_map || !alloc || dir_vp == 0) return VFS_ERR_IO;

    /* Phase 25: by-value pool slots (read-only paths, pinPage=false).
       GC is a sequential walk — no slot is held across an allocation,
       so the copy-out closes the C1 hazard without any pin. */
    PoolSlot dir_slot = {0};
    pool_acquire(&ctx->pool, dir_vp, false, &dir_slot);
    if (dir_slot.vptr == VFS_VPTR_NULL) return VFS_ERR_NOTFOUND;
    if (vfs_rd2_s(dir_slot.bytes, DIRNODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_DIR)
        return VFS_ERR_NOTDIR;

    /* Allocate destination for the DirNode entry */
    if (alloc->cur_slot >= alloc->slots_per_page) {
        alloc->cur_page_vp = gc_allocate_new_pool_page(ctx, gc_map);
        if (alloc->cur_page_vp == VFS_VPTR_NULL) return VFS_ERR_FULL;
        alloc->cur_slot = 0;
    }
    int64_t new_dir_vp = VFS_VPTR_MAKE(VFS_VPTR_PAGE(alloc->cur_page_vp),
                                        alloc->cur_slot);
    PoolSlot new_dir_slot = {0};
    pool_acquire(&ctx->pool, new_dir_vp, false, &new_dir_slot);
    if (new_dir_slot.vptr == VFS_VPTR_NULL) return VFS_ERR_IO;
    alloc->cur_slot++;

    /* Copy the DirNode with remapping */
    gc_copy_entry(gc_map, dir_vp, new_dir_vp, dir_slot.bytes, new_dir_slot.bytes, ctx->page_size);

    /* W5b/W5e: headPtr is a DirSegment VP.  Walk the Segment →
     * SlotNode → DirContent chain and copy survivors via
     * gc_walk_dir_chain.  After the walk, the new DirNode's headPtr
     * will be remapped from the old headPtr by gc_copy_entry's
     * per-type descriptor (HEADPTR is at offset 8 for DirNode). */
    int64_t headPtr = vfs_rd8_s(dir_slot.bytes, DIRNODE_OFF_HEADPTR, ctx->page_size);
    pool_release(&ctx->pool, &dir_slot);
    int err = gc_walk_dir_chain(ctx, gc_map, alloc, headPtr, epoch, lps);
    if (err != VFS_OK) { pool_release(&ctx->pool, &new_dir_slot); return err; }

    /* Remap the new DirNode's headPtr via gc_map (the old segment
     * VP is mapped to the new segment VP if it survived; otherwise
     * the new segment's VP is in the map for the segment's old VP). */
    int64_t new_headPtr = gc_map_get(gc_map, headPtr);
    vfs_wr8_s(new_dir_slot.bytes, DIRNODE_OFF_HEADPTR, new_headPtr, ctx->page_size);
    pool_release(&ctx->pool, &new_dir_slot);

    return VFS_OK;
}

/* ---------------------------------------------------------------------------
 * FileNode walk — copy FileNode, walk FileContent/PageNode/VersionPage and
 * FileSize chains applying survival rules.
 * --------------------------------------------------------------------------- */

int gc_walk_filenode(TreeContext* ctx, GCMap* gc_map, GCAllocCursor* alloc,
                     int64_t file_vp, int64_t epoch,
                     LivePageSet* lps) {
    if (!ctx || !gc_map || !alloc || file_vp == 0) return VFS_ERR_IO;
    (void)epoch;

    /* Phase 25: by-value pool slots (read-only, pinPage=false).  GC is
       a sequential walk — no slot is held across an allocation, so the
       copy-out closes the C1 hazard without any pin. */
    PoolSlot file_slot = {0};
    pool_acquire(&ctx->pool, file_vp, false, &file_slot);
    if (file_slot.vptr == VFS_VPTR_NULL) return VFS_ERR_NOTFOUND;
    if (vfs_rd2_s(file_slot.bytes, FILENODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_FILE)
        return VFS_ERR_IO;

    /* Local allocation helper */
    #define GC_NEXT_SLOT() do { \
        if (alloc->cur_slot >= alloc->slots_per_page) { \
            alloc->cur_page_vp = gc_allocate_new_pool_page(ctx, gc_map); \
            if (alloc->cur_page_vp == VFS_VPTR_NULL) return VFS_ERR_FULL; \
            alloc->cur_slot = 0; \
        } \
    } while(0)

    /* Allocate destination for the FileNode entry */
    GC_NEXT_SLOT();
    int64_t new_file_vp = VFS_VPTR_MAKE(VFS_VPTR_PAGE(alloc->cur_page_vp),
                                         alloc->cur_slot);
    PoolSlot new_file_slot = {0};
    pool_acquire(&ctx->pool, new_file_vp, false, &new_file_slot);
    if (new_file_slot.vptr == VFS_VPTR_NULL) return VFS_ERR_IO;
    alloc->cur_slot++;

    /* Copy the FileNode with remapping */
    gc_copy_entry(gc_map, file_vp, new_file_vp, file_slot.bytes, new_file_slot.bytes, ctx->page_size);
    pool_release(&ctx->pool, &new_file_slot);

    /* Walk FileSize chain via gc_walk_filesize_chain — handles survival rules
       and returns the highest surviving file size for segment pruning. */
    int64_t highest_file_size = 0;
    int64_t size_vp = vfs_rd8_s(file_slot.bytes, FILENODE_OFF_SIZEPTR, ctx->page_size);
    pool_release(&ctx->pool, &file_slot);
    int err_fs = gc_walk_filesize_chain(ctx, gc_map, alloc, size_vp, epoch,
                                         &highest_file_size);
    if (err_fs != VFS_OK) return err_fs;

    /* Walk FileContent chain.
       Skip segments whose page range is beyond the highest surviving
       file size bound (nothing above that size is reachable). */
    int64_t fc_vp_walk = vfs_rd8_s(file_slot.bytes, FILENODE_OFF_HEADPTR, ctx->page_size);
    /* file_slot is no longer needed — release now to shrink live set. */
    pool_release(&ctx->pool, &file_slot);
    int64_t fc_vp = fc_vp_walk;
    int64_t seg_idx = 0;
    while (fc_vp != 0) {
        PoolSlot fc_slot = {0};
        pool_acquire(&ctx->pool, fc_vp, false, &fc_slot);
        if (fc_slot.vptr == VFS_VPTR_NULL) break;

        /* Skip segments beyond the highest surviving file size.
         * NOTE: sparse segments are pruned identically to dense ones —
         * this calculation depends only on seg_idx and segment_size,
         * not on PageNode density. */
        if (highest_file_size > 0 &&
            (seg_idx * (int64_t)ctx->segment_size * ctx->sb->page_size) >= highest_file_size) {
            int64_t fc_next = vfs_rd8_s(fc_slot.bytes, FILECONTENT_OFF_NEXTPTR, ctx->page_size);
            pool_release(&ctx->pool, &fc_slot);
            fc_vp = fc_next;
            seg_idx++;
            continue;
        }

        int64_t fc_page_root = vfs_rd8_s(fc_slot.bytes, FILECONTENT_OFF_ROOTPTR, ctx->page_size);
        int64_t fc_next = vfs_rd8_s(fc_slot.bytes, FILECONTENT_OFF_NEXTPTR, ctx->page_size);
        pool_release(&ctx->pool, &fc_slot);

        /* Walk PageNode chain within this FileContent segment.
         * nextPtr semantics are unchanged for sparse chains — the same
         * code path works regardless of PageNode density. */
        int64_t pn_vp = fc_page_root;
        int segment_has_live = 0;

        /* First pass: count live VersionPages to decide if segment survives */
        while (pn_vp != 0) {
            PoolSlot pn_slot = {0};
            pool_acquire(&ctx->pool, pn_vp, false, &pn_slot);
            if (pn_slot.vptr == VFS_VPTR_NULL) break;

            int64_t vp_chain = vfs_rd8_s(pn_slot.bytes, PAGENODE_OFF_VERSIONROOT, ctx->page_size);
            int pn_seg_live = 0;
            while (vp_chain != 0) {
                PoolSlot vp_slot = {0};
                pool_acquire(&ctx->pool, vp_chain, false, &vp_slot);
                if (vp_slot.vptr == VFS_VPTR_NULL) break;
                uint32_t vp_epoch;
                int64_t vp_dataPage, vp_next;
                nodes_read_versionpage(vp_slot.bytes, &vp_epoch, &vp_dataPage, &vp_next, ctx->page_size);
                (void)vp_dataPage;
                pool_release(&ctx->pool, &vp_slot);

                int live = 1;
                if (vp_epoch % 2 == 1) {
                    if (mapper_resolve(&ctx->mapper, (int64_t)vp_epoch) != (int64_t)vp_epoch) {
                        if (!mapper_traversal_apply(&ctx->mapper, (int64_t)vp_epoch)) {
                            live = 0;  /* soft-deleted → DROP */
                        }
                    }
                }
                if (live) { pn_seg_live = 1; break; }
                vp_chain = vp_next;
            }
            if (pn_seg_live) {
                segment_has_live = 1;
                pool_release(&ctx->pool, &pn_slot);
                break;
            }
            int64_t pn_next_loop = vfs_rd8_s(pn_slot.bytes, PAGENODE_OFF_NEXTPTR, ctx->page_size);
            pool_release(&ctx->pool, &pn_slot);
            pn_vp = pn_next_loop;
        }

        if (!segment_has_live) { fc_vp = fc_next; seg_idx++; continue; }

        /* Copy the FileContent segment.  Re-acquire fc_slot (already
           released above after extracting fc_page_root/fc_next). */
        GC_NEXT_SLOT();
        int64_t new_fc_vp = VFS_VPTR_MAKE(
            VFS_VPTR_PAGE(alloc->cur_page_vp), alloc->cur_slot);
        PoolSlot new_fc_slot = {0};
        pool_acquire(&ctx->pool, new_fc_vp, false, &new_fc_slot);
        if (new_fc_slot.vptr == VFS_VPTR_NULL) return VFS_ERR_IO;
        alloc->cur_slot++;
        /* Re-acquire fc_slot for the copy source.  Phase 25: gc_copy_entry
           takes a raw pointer; we pass &slot.bytes. */
        PoolSlot fc_slot_src = {0};
        pool_acquire(&ctx->pool, fc_vp, false, &fc_slot_src);
        if (fc_slot_src.vptr != VFS_VPTR_NULL) {
            gc_copy_entry(gc_map, fc_vp, new_fc_vp, fc_slot_src.bytes, new_fc_slot.bytes, ctx->page_size);
        }
        pool_release(&ctx->pool, &fc_slot_src);
        pool_release(&ctx->pool, &new_fc_slot);

        /* Walk PageNode chain to copy live version pages. */
        pn_vp = fc_page_root;
        while (pn_vp != 0) {
            PoolSlot pn_slot = {0};
            pool_acquire(&ctx->pool, pn_vp, false, &pn_slot);
            if (pn_slot.vptr == VFS_VPTR_NULL) break;
            int64_t pn_next = vfs_rd8_s(pn_slot.bytes, PAGENODE_OFF_NEXTPTR, ctx->page_size);

            int64_t vp_chain = vfs_rd8_s(pn_slot.bytes, PAGENODE_OFF_VERSIONROOT, ctx->page_size);
            int pn_has_live = 0;

            /* Check if any VersionPage in this PageNode survives */
            int64_t vp_walk = vp_chain;
            while (vp_walk != 0) {
                PoolSlot vp_slot = {0};
                pool_acquire(&ctx->pool, vp_walk, false, &vp_slot);
                if (vp_slot.vptr == VFS_VPTR_NULL) break;
                uint32_t vp_epoch;
                int64_t vp_dataPage, vp_next;
                nodes_read_versionpage(vp_slot.bytes, &vp_epoch, &vp_dataPage, &vp_next, ctx->page_size);
                (void)vp_dataPage;
                pool_release(&ctx->pool, &vp_slot);

                int live = 1;
                if (vp_epoch % 2 == 1) {
                    if (mapper_resolve(&ctx->mapper, (int64_t)vp_epoch) != (int64_t)vp_epoch) {
                        if (!mapper_traversal_apply(&ctx->mapper, (int64_t)vp_epoch)) {
                            live = 0;
                        }
                    }
                }
                if (live) { pn_has_live = 1; break; }
                vp_walk = vp_next;
            }
            pool_release(&ctx->pool, &pn_slot);
            if (!pn_has_live) { pn_vp = pn_next; continue; }

            /* Copy the PageNode */
            GC_NEXT_SLOT();
            int64_t new_pn_vp = VFS_VPTR_MAKE(
                VFS_VPTR_PAGE(alloc->cur_page_vp), alloc->cur_slot);
            PoolSlot new_pn_slot = {0};
            pool_acquire(&ctx->pool, new_pn_vp, false, &new_pn_slot);
            if (new_pn_slot.vptr == VFS_VPTR_NULL) return VFS_ERR_IO;
            alloc->cur_slot++;
            /* Re-acquire pn_slot for the copy source. */
            PoolSlot pn_slot_src = {0};
            pool_acquire(&ctx->pool, pn_vp, false, &pn_slot_src);
            if (pn_slot_src.vptr != VFS_VPTR_NULL) {
                gc_copy_entry(gc_map, pn_vp, new_pn_vp, pn_slot_src.bytes, new_pn_slot.bytes, ctx->page_size);
            }
            pool_release(&ctx->pool, &pn_slot_src);
            pool_release(&ctx->pool, &new_pn_slot);

            /* Copy live VersionPages */
            vp_walk = vp_chain;
            while (vp_walk != 0) {
                PoolSlot vp_slot = {0};
                pool_acquire(&ctx->pool, vp_walk, false, &vp_slot);
                if (vp_slot.vptr == VFS_VPTR_NULL) break;
                uint32_t vp_epoch;
                int64_t vp_dataPage, vp_next;
                nodes_read_versionpage(vp_slot.bytes, &vp_epoch, &vp_dataPage, &vp_next, ctx->page_size);

                int live = 1;
                int64_t rewrite_epoch_vp = (int64_t)vp_epoch;
                if (vp_epoch % 2 == 1) {
                    if (mapper_resolve(&ctx->mapper, (int64_t)vp_epoch) != (int64_t)vp_epoch) {
                        if (mapper_traversal_apply(&ctx->mapper, (int64_t)vp_epoch)) {
                            /* Committed → REWRITE epoch to toEpoch */
                            rewrite_epoch_vp = mapper_resolve(&ctx->mapper,
                                                               (int64_t)vp_epoch);
                        } else {
                            live = 0;
                        }
                    }
                }

                if (live) {
                    GC_NEXT_SLOT();
                    int64_t new_vp_slot_vp = VFS_VPTR_MAKE(
                        VFS_VPTR_PAGE(alloc->cur_page_vp), alloc->cur_slot);
                    PoolSlot new_vp_slot = {0};
                    pool_acquire(&ctx->pool, new_vp_slot_vp, false, &new_vp_slot);
                    if (new_vp_slot.vptr == VFS_VPTR_NULL) { pool_release(&ctx->pool, &vp_slot); return VFS_ERR_IO; }
                    alloc->cur_slot++;
                    gc_copy_entry(gc_map, vp_walk, new_vp_slot_vp,
                                  vp_slot.bytes, new_vp_slot.bytes, ctx->page_size);
                    pool_release(&ctx->pool, &new_vp_slot);

                    if (lps && vp_dataPage > 0)
                        live_page_set_add(lps, vp_dataPage);

                    if (rewrite_epoch_vp != (int64_t)vp_epoch) {
                        /* Re-acquire new_vp_slot to apply the epoch rewrite. */
                        PoolSlot new_vp_rewrite = {0};
                        pool_acquire(&ctx->pool, new_vp_slot_vp, false, &new_vp_rewrite);
                        if (new_vp_rewrite.vptr != VFS_VPTR_NULL) {
                            vfs_wr4_s(new_vp_rewrite.bytes, VERSIONPAGE_OFF_EPOCH,
                                    (uint32_t)rewrite_epoch_vp, ctx->page_size);
                            pool_release(&ctx->pool, &new_vp_rewrite);
                        }
                    }
                }

                pool_release(&ctx->pool, &vp_slot);
                vp_walk = vp_next;
            }

            pn_vp = pn_next;
        }

        fc_vp = fc_next;
        seg_idx++;
    }

    #undef GC_NEXT_SLOT
    return VFS_OK;
}

/* ---------------------------------------------------------------------------
 * VersionPage chain walk with survival rules
 * --------------------------------------------------------------------------- */

int gc_walk_versionpage_chain(TreeContext* ctx, GCMap* gc_map,
                               GCAllocCursor* alloc,
                               int64_t version_root_vp,
                               LivePageSet* lps) {
    if (!ctx || !gc_map || !alloc) return VFS_ERR_IO;

    #define GC_NEXT_SLOT() do { \
        if (alloc->cur_slot >= alloc->slots_per_page) { \
            alloc->cur_page_vp = gc_allocate_new_pool_page(ctx, gc_map); \
            if (alloc->cur_page_vp == VFS_VPTR_NULL) return VFS_ERR_FULL; \
            alloc->cur_slot = 0; \
        } \
    } while(0)

    int64_t vp_walk = version_root_vp;
    while (vp_walk != 0) {
        /* Phase 25: by-value pool slot (read-only). */
        PoolSlot vp_slot = {0};
        pool_acquire(&ctx->pool, vp_walk, false, &vp_slot);
        if (vp_slot.vptr == VFS_VPTR_NULL) break;

        uint32_t vp_epoch;
        int64_t vp_dataPage, vp_next;
        nodes_read_versionpage(vp_slot.bytes, &vp_epoch, &vp_dataPage, &vp_next, ctx->page_size);
        (void)vp_dataPage;

        int live = 1;
        int64_t rewrite_epoch = (int64_t)vp_epoch;

        /* Check mapper for odd epochs */
        if (vp_epoch % 2 == 1) {
            int64_t resolved = mapper_resolve(&ctx->mapper, (int64_t)vp_epoch);
            if (resolved != (int64_t)vp_epoch) {
                if (mapper_traversal_apply(&ctx->mapper, (int64_t)vp_epoch)) {
                    /* Committed — REWRITE epoch to resolved toEpoch */
                    rewrite_epoch = resolved;
                } else {
                    /* Soft-deleted — DROP */
                    live = 0;
                }
            }
        }

        if (live) {
            GC_NEXT_SLOT();
            int64_t new_vp = VFS_VPTR_MAKE(
                VFS_VPTR_PAGE(alloc->cur_page_vp), alloc->cur_slot);
            PoolSlot new_slot = {0};
            pool_acquire(&ctx->pool, new_vp, false, &new_slot);
            if (new_slot.vptr == VFS_VPTR_NULL) { pool_release(&ctx->pool, &vp_slot); return VFS_ERR_IO; }
            alloc->cur_slot++;

            gc_copy_entry(gc_map, vp_walk, new_vp, vp_slot.bytes, new_slot.bytes, ctx->page_size);
            pool_release(&ctx->pool, &vp_slot);

            /* Record the data page in the live set */
            if (lps && vp_dataPage > 0)
                live_page_set_add(lps, vp_dataPage);

            /* Update epoch field if rewritten */
            if (rewrite_epoch != (int64_t)vp_epoch) {
                vfs_wr4_s(new_slot.bytes, VERSIONPAGE_OFF_EPOCH,
                        (uint32_t)rewrite_epoch, ctx->page_size);
            }
            pool_release(&ctx->pool, &new_slot);
        } else {
            pool_release(&ctx->pool, &vp_slot);
        }

        vp_walk = vp_next;
    }

    #undef GC_NEXT_SLOT
    return VFS_OK;
}

/* ---------------------------------------------------------------------------
 * DirContent chain walk with survival rules
 * --------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * gc_walk_dir_chain — W5e per-ContentUnit survival walk.
 *
 * Walks DirSegment → SlotNode → DirContent chains (the W5b structure).
 * For each SlotNode, applies the per-ContentUnit read-rule to find the
 * visible entry; if a live entry (namePtr != 0) exists, copies the
 * SlotNode + the surviving DirContent.  SlotNodes with only tombstones
 * are skipped (no need to copy dead content).
 *
 * Replaces the old gc_walk_dircontent_chain's MAX_CHILDREN=1024 fixed
 * array (ISSUES.md M2).  Per-ContentUnit chains are short (1-2 entries
 * typically), so the dedup is per-SlotNode — no cross-child tracking
 * needed.
 *
 * Input: dir_head_vp is a DirSegment VP (root's HEADPTR or a Segment's
 * headPtr).  Walks the Segment chain to find SlotNodes.
 * --------------------------------------------------------------------------- */

static int gc_alloc_slot(TreeContext* ctx, GCAllocCursor* alloc,
                         PoolSlot* out_slot) {
    if (alloc->cur_slot >= alloc->slots_per_page) {
        alloc->cur_page_vp = gc_allocate_new_pool_page(ctx, NULL);
        if (alloc->cur_page_vp == VFS_VPTR_NULL) return VFS_ERR_FULL;
        alloc->cur_slot = 0;
    }
    int64_t new_vp = VFS_VPTR_MAKE(VFS_VPTR_PAGE(alloc->cur_page_vp),
                                     alloc->cur_slot);
    pool_acquire(&ctx->pool, new_vp, false, out_slot);
    if (out_slot->vptr == VFS_VPTR_NULL) return VFS_ERR_IO;
    alloc->cur_slot++;
    return VFS_OK;
}

int gc_walk_dir_chain(TreeContext* ctx, GCMap* gc_map, GCAllocCursor* alloc,
                      int64_t dir_head_vp, int64_t epoch,
                      LivePageSet* lps) {
    if (!ctx || !gc_map || !alloc) return VFS_ERR_IO;

    int64_t seg_vp = dir_head_vp;
    while (seg_vp != 0) {
        PoolSlot seg_slot = {0};
        pool_acquire(&ctx->pool, seg_vp, false, &seg_slot);
        if (seg_slot.vptr == VFS_VPTR_NULL) break;
        AnchorKind seg_ak;
        uint32_t seg_id, seg_cnt;
        int64_t seg_head, seg_sib;
        nodes_read_anchor(seg_slot.bytes, &seg_ak, &seg_id, &seg_head,
                          &seg_sib, &seg_cnt, ctx->page_size);
        pool_release(&ctx->pool, &seg_slot);

        /* Copy this Segment (if it has any surviving SlotNodes).  We
         * do a tentative copy + remap, then update the Segment's
         * headPtr/sibPtr after processing all its SlotNodes. */
        int64_t new_seg_vp = 0;
        PoolSlot new_seg_slot = {0};
        int new_seg_valid = 0;
        int64_t new_slot_head = 0;  /* head of the new SlotNode chain */

        int64_t slot_vp = seg_head;
        while (slot_vp != 0) {
            PoolSlot slot_slot = {0};
            pool_acquire(&ctx->pool, slot_vp, false, &slot_slot);
            if (slot_slot.vptr == VFS_VPTR_NULL) break;
            AnchorKind slot_ak;
            uint32_t slot_id, slot_cnt;
            int64_t slot_head, slot_sib;
            nodes_read_anchor(slot_slot.bytes, &slot_ak, &slot_id, &slot_head,
                              &slot_sib, &slot_cnt, ctx->page_size);
            pool_release(&ctx->pool, &slot_slot);

            /* Per-ContentUnit survival: walk the SlotNode's DC chain
             * to find the visible entry (first applicable per
             * read-rule).  If the visible entry is a live (namePtr != 0)
             * DC, copy the SlotNode + the visible DC.  Otherwise skip
             * (tombstone-only SlotNode — no need to copy dead data). */
            int found_visible = 0;
            int64_t vis_dc_vp = 0;
            uint32_t vis_dc_child = 0;
            int64_t vis_dc_childPtr = 0;
            int64_t vis_dc_namePtr = 0;
            int vis_dc_has_name = 0;
            int64_t dc_walk_vp = slot_head;
            while (dc_walk_vp != 0 && !found_visible) {
                PoolSlot dc_slot = {0};
                pool_acquire(&ctx->pool, dc_walk_vp, false, &dc_slot);
                if (dc_slot.vptr == VFS_VPTR_NULL) break;
                uint32_t dc_child, dc_epoch;
                int64_t dc_childPtr, dc_namePtr, dc_next;
                nodes_read_dircontent(dc_slot.bytes, &dc_child, &dc_epoch,
                                      &dc_childPtr, &dc_namePtr, &dc_next,
                                      ctx->page_size);
                pool_release(&ctx->pool, &dc_slot);

                int64_t eff_epoch = (int64_t)dc_epoch;
                if (mapper_table_traversal_apply(&ctx->mapper_table,
                                                  (int64_t)dc_epoch))
                    eff_epoch = mapper_table_resolve(&ctx->mapper_table,
                                                      (int64_t)dc_epoch);
                int applies = (eff_epoch == epoch) ||
                              (eff_epoch < epoch && (eff_epoch & 1) == 0);
                if (applies) {
                    found_visible = 1;
                    vis_dc_vp = dc_walk_vp;
                    vis_dc_child = dc_child;
                    vis_dc_childPtr = dc_childPtr;
                    vis_dc_namePtr = dc_namePtr;
                    vis_dc_has_name = (dc_namePtr != 0) ? 1 : 0;
                }
                dc_walk_vp = dc_next;
            }

            if (found_visible && vis_dc_has_name) {
                /* Lazily allocate the new Segment slot on first surviving SlotNode. */
                if (!new_seg_valid) {
                    int r = gc_alloc_slot(ctx, alloc, &new_seg_slot);
                    if (r != VFS_OK) return r;
                    new_seg_vp = VFS_VPTR_MAKE(
                        VFS_VPTR_PAGE(alloc->cur_page_vp), alloc->cur_slot - 1);
                    /* Tentative write of the Segment (headPtr=0, sibPtr=old sib). */
                    nodes_write_anchor(new_seg_slot.bytes, ANCHOR_KIND_SEGMENT_DIR,
                                       0, 0, gc_map_get(gc_map, seg_sib),
                                       0, ctx->page_size);
                    new_seg_valid = 1;
                }
                /* Copy the SlotNode (the SlotNode itself is just a
                 * small Anchor; we keep it in the chain).  It points
                 * to the new DC after remap.  The SlotNode's count is
                 * 0 (single child per SlotNode in our model). */
                int64_t new_slot_vp;
                PoolSlot new_slot_slot = {0};
                int rs = gc_alloc_slot(ctx, alloc, &new_slot_slot);
                if (rs != VFS_OK) { pool_release(&ctx->pool, &new_seg_slot); return rs; }
                new_slot_vp = VFS_VPTR_MAKE(
                    VFS_VPTR_PAGE(alloc->cur_page_vp), alloc->cur_slot - 1);
                gc_copy_entry(gc_map, slot_vp, new_slot_vp,
                              slot_slot.bytes, new_slot_slot.bytes,
                              ctx->page_size);
                /* Now copy the visible DC and rewire the new SlotNode's headPtr. */
                int64_t new_dc_vp;
                PoolSlot new_dc_slot = {0};
                int rd = gc_alloc_slot(ctx, alloc, &new_dc_slot);
                if (rd != VFS_OK) { pool_release(&ctx->pool, &new_slot_slot); pool_release(&ctx->pool, &new_seg_slot); return rd; }
                new_dc_vp = VFS_VPTR_MAKE(
                    VFS_VPTR_PAGE(alloc->cur_page_vp), alloc->cur_slot - 1);
                /* Re-acquire the source DC for copy. */
                PoolSlot src_dc_slot = {0};
                pool_acquire(&ctx->pool, vis_dc_vp, false, &src_dc_slot);
                if (src_dc_slot.vptr == VFS_VPTR_NULL) {
                    pool_release(&ctx->pool, &new_dc_slot);
                    pool_release(&ctx->pool, &new_slot_slot);
                    pool_release(&ctx->pool, &new_seg_slot);
                    return VFS_ERR_IO;
                }
                gc_copy_entry(gc_map, vis_dc_vp, new_dc_vp,
                              src_dc_slot.bytes, new_dc_slot.bytes,
                              ctx->page_size);
                pool_release(&ctx->pool, &src_dc_slot);
                pool_release(&ctx->pool, &new_dc_slot);

                /* Prepend the new SlotNode to the new Segment's SlotNode chain. */
                if (new_slot_head == 0) {
                    nodes_write_anchor(new_seg_slot.bytes, ANCHOR_KIND_SEGMENT_DIR,
                                       0, new_slot_vp,
                                       gc_map_get(gc_map, seg_sib),
                                       1, ctx->page_size);
                    new_slot_head = new_slot_vp;
                } else {
                    /* Insert at head: set new SlotNode's sibPtr to the
                     * previous head, then update Segment's headPtr. */
                    vfs_wr8_s(new_slot_slot.bytes, ANCHOR_OFF_SIBPTR,
                              new_slot_head, ctx->page_size);
                    pool_release(&ctx->pool, &new_slot_slot);
                    nodes_write_anchor(new_seg_slot.bytes, ANCHOR_KIND_SEGMENT_DIR,
                                       0, new_slot_vp,
                                       gc_map_get(gc_map, seg_sib),
                                       /* count will be updated later */ 0, ctx->page_size);
                    new_slot_head = new_slot_vp;
                    continue;  /* skip the pool_release at the end of the loop */
                }
                pool_release(&ctx->pool, &new_slot_slot);

                /* Recursively walk the child (DirNode or FileNode). */
                if (vis_dc_childPtr != 0) {
                    PoolSlot child_slot = {0};
                    pool_acquire(&ctx->pool, vis_dc_childPtr, false, &child_slot);
                    if (child_slot.vptr != VFS_VPTR_NULL) {
                        int16_t ctype = vfs_rd2_s(child_slot.bytes,
                                                   DIRNODE_OFF_TYPE, ctx->page_size);
                        pool_release(&ctx->pool, &child_slot);
                        if (ctype == (int16_t)NODE_TYPE_DIR) {
                            int err = gc_walk_dirnode(ctx, gc_map, alloc,
                                                       vis_dc_childPtr, epoch, lps);
                            if (err != VFS_OK) { pool_release(&ctx->pool, &new_seg_slot); return err; }
                        } else if (ctype == (int16_t)NODE_TYPE_FILE) {
                            int err = gc_walk_filenode(ctx, gc_map, alloc,
                                                        vis_dc_childPtr, epoch, lps);
                            if (err != VFS_OK) { pool_release(&ctx->pool, &new_seg_slot); return err; }
                        }
                    } else {
                        pool_release(&ctx->pool, &child_slot);
                    }
                }
            }
            slot_vp = slot_sib;
        }

        if (new_seg_valid) {
            pool_release(&ctx->pool, &new_seg_slot);
        }

        seg_vp = seg_sib;
    }
    return VFS_OK;
}


/* ---------------------------------------------------------------------------
 * FileSize chain walk with survival rules
 * --------------------------------------------------------------------------- */

int gc_walk_filesize_chain(TreeContext* ctx, GCMap* gc_map,
                            GCAllocCursor* alloc,
                            int64_t head_size_vp, int64_t epoch,
                            int64_t* out_highest_file_size) {
    if (!ctx || !gc_map || !alloc) return VFS_ERR_IO;
    (void)epoch;

    #define GC_NEXT_SLOT() do { \
        if (alloc->cur_slot >= alloc->slots_per_page) { \
            alloc->cur_page_vp = gc_allocate_new_pool_page(ctx, gc_map); \
            if (alloc->cur_page_vp == VFS_VPTR_NULL) return VFS_ERR_FULL; \
            alloc->cur_slot = 0; \
        } \
    } while(0)

    int64_t fs_vp = head_size_vp;
    int64_t highest_file_size = 0;

    while (fs_vp != 0) {
        /* Phase 25: by-value pool slot (read-only). */
        PoolSlot fs_slot = {0};
        pool_acquire(&ctx->pool, fs_vp, false, &fs_slot);
        if (fs_slot.vptr == VFS_VPTR_NULL) break;

        uint32_t fs_epoch;
        int64_t fs_modifiedAt, fs_fileSize, fs_next;
        nodes_read_filesize(fs_slot.bytes, &fs_epoch, &fs_modifiedAt,
                            &fs_fileSize, &fs_next, ctx->page_size);
        (void)fs_modifiedAt;

        int keep = 1;
        int64_t rewrite_epoch = (int64_t)fs_epoch;

        /* Apply epoch survival rules */
        if (fs_epoch % 2 == 1) {
            int64_t resolved = mapper_resolve(&ctx->mapper, (int64_t)fs_epoch);
            if (resolved != (int64_t)fs_epoch) {
                if (mapper_traversal_apply(&ctx->mapper, (int64_t)fs_epoch)) {
                    /* Committed — REWRITE epoch to resolved toEpoch */
                    rewrite_epoch = resolved;
                } else {
                    /* Soft-deleted — DROP */
                    keep = 0;
                }
            }
        }

        if (keep) {
            GC_NEXT_SLOT();
            int64_t new_fs_vp = VFS_VPTR_MAKE(
                VFS_VPTR_PAGE(alloc->cur_page_vp), alloc->cur_slot);
            PoolSlot new_fs_slot = {0};
            pool_acquire(&ctx->pool, new_fs_vp, false, &new_fs_slot);
            if (new_fs_slot.vptr == VFS_VPTR_NULL) { pool_release(&ctx->pool, &fs_slot); return VFS_ERR_IO; }
            alloc->cur_slot++;

            gc_copy_entry(gc_map, fs_vp, new_fs_vp, fs_slot.bytes, new_fs_slot.bytes, ctx->page_size);

            /* Update epoch field if rewritten */
            if (rewrite_epoch != (int64_t)fs_epoch) {
                vfs_wr4_s(new_fs_slot.bytes, FILESIZE_OFF_EPOCH, (uint32_t)rewrite_epoch, ctx->page_size);
            }

            /* Track highest surviving file size */
            if (fs_fileSize > highest_file_size) highest_file_size = fs_fileSize;
            pool_release(&ctx->pool, &new_fs_slot);
        }
        pool_release(&ctx->pool, &fs_slot);

        fs_vp = fs_next;
    }

    /* Expose highest surviving file size for FileContent segment pruning */
    if (out_highest_file_size) *out_highest_file_size = highest_file_size;

    #undef GC_NEXT_SLOT
    return VFS_OK;
}

/* ---------------------------------------------------------------------------
 * GC mapper rebuild — drop entries for soft-deleted and committed epochs
 * --------------------------------------------------------------------------- */

int gc_rebuild_mapper(TreeContext* ctx, GCMap* gc_map,
                       GCAllocCursor* alloc) {
    if (!ctx || !gc_map || !alloc) return VFS_ERR_IO;

    #define GC_NEXT_SLOT() do { \
        if (alloc->cur_slot >= alloc->slots_per_page) { \
            alloc->cur_page_vp = gc_allocate_new_pool_page(ctx, gc_map); \
            if (alloc->cur_page_vp == VFS_VPTR_NULL) return VFS_ERR_FULL; \
            alloc->cur_slot = 0; \
        } \
    } while(0)

    /* Walk the mapper entry chain rooted at epochMapperPtr.
       Keep only entries whose fromEpoch is still active (not committed
       and not soft-deleted).  Committed and soft-deleted entries are
       dropped because their VersionPages were already relabeled during
       the FileNode walk (committed → REWRITE, soft-deleted → DROP). */
    int64_t vp = ctx->epochMapperPtr;
    while (vp != 0) {
        /* Phase 25: by-value pool slot (read-only). */
        PoolSlot slot = {0};
        pool_acquire(&ctx->pool, vp, false, &slot);
        if (slot.vptr == VFS_VPTR_NULL) break;

        uint32_t fromEpoch, toEpoch;
        uint16_t flags;
        int64_t next;
        nodes_read_mapperentry(slot.bytes, &fromEpoch, &toEpoch, &flags, &next, ctx->page_size);

        int keep = 1;
        if (flags & MAPPER_FLAG_TRAVERSAL_APPLY) keep = 0;  /* committed */
        else if (fromEpoch % 2 == 1) keep = 0;               /* soft-deleted */

        if (keep) {
            GC_NEXT_SLOT();
            int64_t new_vp = VFS_VPTR_MAKE(
                VFS_VPTR_PAGE(alloc->cur_page_vp), alloc->cur_slot);
            PoolSlot new_slot = {0};
            pool_acquire(&ctx->pool, new_vp, false, &new_slot);
            if (new_slot.vptr == VFS_VPTR_NULL) { pool_release(&ctx->pool, &slot); return VFS_ERR_IO; }
            alloc->cur_slot++;

            gc_copy_entry(gc_map, vp, new_vp, slot.bytes, new_slot.bytes, ctx->page_size);
            pool_release(&ctx->pool, &new_slot);
        }
        pool_release(&ctx->pool, &slot);

        vp = next;
    }

    #undef GC_NEXT_SLOT
    return VFS_OK;
}

/* ---------------------------------------------------------------------------
 * GC touched file rebuild — drop all entries since they're rebuilt fresh
 * --------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * GC new superblock builder
 * --------------------------------------------------------------------------- */

/* Write the superblock page with GC-updated values after shadow-compaction.
 * rootNodeOffset, currentEpoch, and nextNodeId are preserved from ctx.
 * epochMapperPtr and poolListHead are the post-GC values.
 * treeLockState is written as 0 (exclusive lock released for new tree). */
int gc_build_new_superblock(TreeContext* ctx, int64_t new_epochMapperPtr,
                             int64_t new_poolListHead) {
    if (!ctx) return VFS_ERR_IO;
    int64_t ps = ctx->sb->page_size;
    uint8_t* buf = (uint8_t*)malloc((size_t)ps);
    if (!buf) return VFS_ERR_NOMEM;
    memset(buf, 0, (size_t)ps);

    vfs_wr8_s(buf, SB_OFF_ROOT_OFFSET,       ctx->rootNodeOffset, ctx->page_size);
    vfs_wr8_s(buf, SB_OFF_CURRENT_EPOCH,     ctx->currentEpoch, ctx->page_size);
    vfs_wr8_s(buf, SB_OFF_EPOCH_MAPPER_PTR,  new_epochMapperPtr, ctx->page_size);
    vfs_wr8_s(buf, SB_OFF_POOL_LIST_HEAD,    new_poolListHead, ctx->page_size);
    vfs_wr8_s(buf, SB_OFF_TREE_LOCK_STATE,   0, ctx->page_size);
    vfs_wr4_s(buf, SB_OFF_NEXT_NODE_ID,      (int32_t)ctx->nextNodeId, ctx->page_size);

    storage_write(ctx->sb, SUPERBLOCK_PAGE, buf, 3);
    storage_flush(ctx->sb, -1);
    free(buf);
    return VFS_OK;
}

/* Shadow-compaction helper — walks the pool chain, builds a live set,
   copies live pool entries to fresh pages, then enqueues old pages
   for deferred freeing.  Currently a stub — returns VFS_OK. */
static int gc_shadow_compact(TreeContext* ctx, DeferredFreeQueue* queue) {
    if (!ctx || !queue) return VFS_ERR_IO;

    /* Record the old pool list head to enqueue old pages later */
    int64_t old_pool_list_head = ctx->pool.list_head ? *ctx->pool.list_head : 0;

    /* Save pre-GC total pages — new pages allocated during tree walk
       should NOT be subject to reclamation. */
    int64_t old_total_pages = ctx->sb->total_pages;

    /* Initialize the allocation cursor */
    GCAllocCursor alloc;
    alloc.cur_page_vp = VFS_VPTR_NULL;
    alloc.cur_slot = 0;
    alloc.slots_per_page = VFS_POOL_SLOTS_FOR_PAGE(ctx->sb->page_size);

    /* Initialize the VirtualPtr remapping map */
    GCMap gc_map;
    int err = gc_map_init(&gc_map, 1024);
    if (err != VFS_OK) return err;

    /* Allocate the first destination pool page and start copying */
    alloc.cur_page_vp = gc_allocate_new_pool_page(ctx, &gc_map);
    if (alloc.cur_page_vp == VFS_VPTR_NULL) {
        gc_map_destroy(&gc_map);
        return VFS_ERR_FULL;
    }

    /* Walk and copy the root DirNode (recursively walks all children).
       Track live data pages for dead-page reclamation. */
    LivePageSet* lps = live_page_set_create(256);
    err = gc_walk_dirnode(ctx, &gc_map, &alloc, ctx->rootNodeOffset,
                           ctx->currentEpoch, lps);
    if (err != VFS_OK) {
        if (lps) live_page_set_destroy(lps);
        gc_map_destroy(&gc_map);
        return err;
    }
    /* Update rootNodeOffset to the new VirtualPtr (avoid dangling pointer) */
    ctx->rootNodeOffset = gc_map_get(&gc_map, ctx->rootNodeOffset);

    /* Rebuild the mapper chain (drop committed/soft-deleted entries) */
    err = gc_rebuild_mapper(ctx, &gc_map, &alloc);
    if (err != VFS_OK) {
        if (lps) live_page_set_destroy(lps);
        gc_map_destroy(&gc_map);
        return err;
    }
    /* Update epochMapperPtr — if old head was remapped, use new VP; else 0 */
    int64_t mapped_mapper = gc_map_get(&gc_map, ctx->epochMapperPtr);
    ctx->epochMapperPtr = (mapped_mapper != ctx->epochMapperPtr) ? mapped_mapper : 0;

    /* Rebuild the in-memory mapper table to reflect GC'd chain state */
    {
        int err_mt = mapper_table_rebuild(&ctx->mapper_table);
        if (err_mt != VFS_OK) {
            if (lps) live_page_set_destroy(lps);
            gc_map_destroy(&gc_map);
            return err_mt;
        }
    }

    /* Write the new superblock with post-GC values */
    int64_t new_pool_head = ctx->pool.list_head ? *ctx->pool.list_head : 0;
    int64_t new_mapper_ptr = ctx->epochMapperPtr;
    err = gc_build_new_superblock(ctx, new_mapper_ptr, new_pool_head);
    if (err != VFS_OK) {
        if (lps) live_page_set_destroy(lps);
        gc_map_destroy(&gc_map);
        return err;
    }

    /* Enqueue old pool pages for deferred free BEFORE reclaiming dead pages.
       This ensures deferred_free_is_queued correctly identifies pool pages
       in the reclamation loop below. */
    int64_t old_page = old_pool_list_head;
    while (old_page != 0) {
        deferred_free_enqueue(queue, old_page, ctx->sb);
        uint8_t* old_header = storage_read(ctx->sb, old_page);
        if (!old_header) break;
        int64_t next_page = vfs_rd8_s(old_header, 0, ctx->page_size);
        old_page = next_page;
    }

    /* Free dead data pages: iterate all logical pages from 2 upwards
       using pre-GC total_pages to avoid touching new pool pages.
       Pool pages (already in deferred-free queue) are skipped.
       Data pages not in the live set are freed via storage_free. */
    if (lps) {
        for (int64_t page = 2; page < old_total_pages; page++) {
            if (is_pool_page(queue, page))
                continue;
            if (!live_page_set_contains(lps, page))
                storage_free(ctx->sb, page);
        }
    }

    /* Verify: old pool pages are now enqueued, dead data pages freed. */

    /* Destroy the gc_map and live page set */
    gc_map_destroy(&gc_map);
    if (lps) live_page_set_destroy(lps);

    /* Bump the GC generation counter — signals to worker threads that
     * pool pages may have been remapped (stale VirtualPtrs invalidated).
     * Atomic store is sufficient: GC holds tree exclusive lock so no
     * worker is mid-lookup against old pool pages. */
    vfs_atomic_store_i64(&ctx->gc_generation, ctx->gc_generation + 1);

    return VFS_OK;
}

/* ---------------------------------------------------------------------------
 * Live page set — simple dynamic array of logical page indices.
 * --------------------------------------------------------------------------- */

LivePageSet* live_page_set_create(int initial_cap) {
    if (initial_cap <= 0) initial_cap = 64;
    LivePageSet* lps = (LivePageSet*)calloc(1, sizeof(LivePageSet));
    if (!lps) return NULL;
    lps->pages = (int64_t*)calloc((size_t)initial_cap, sizeof(int64_t));
    if (!lps->pages) { free(lps); return NULL; }
    lps->count = 0;
    lps->capacity = initial_cap;
    return lps;
}

void live_page_set_destroy(LivePageSet* lps) {
    if (!lps) return;
    free(lps->pages);
    free(lps);
}

int live_page_set_add(LivePageSet* lps, int64_t page) {
    if (!lps) return VFS_ERR_IO;
    /* Skip if already in set */
    int lo = 0, hi = lps->count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (lps->pages[mid] == page) return VFS_OK;
        if (lps->pages[mid] < page) lo = mid + 1;
        else hi = mid - 1;
    }
    if (lps->count >= lps->capacity) {
        int new_cap = lps->capacity * 2;
        int64_t* new_pages = (int64_t*)realloc(lps->pages,
                                (size_t)new_cap * sizeof(int64_t));
        if (!new_pages) return VFS_ERR_NOMEM;
        lps->pages = new_pages;
        lps->capacity = new_cap;
    }
    /* Insert at lo (shift elements right) */
    for (int i = lps->count; i > lo; i--)
        lps->pages[i] = lps->pages[i - 1];
    lps->pages[lo] = page;
    lps->count++;
    return VFS_OK;
}

int live_page_set_contains(LivePageSet* lps, int64_t page) {
    if (!lps || !lps->pages) return 0;
    int lo = 0, hi = lps->count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (lps->pages[mid] == page) return 1;
        if (lps->pages[mid] < page) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

int vfs_gc(vfs_t* vfs) {
    if (!vfs || !vfs->ctx) return VFS_ERR_IO;
    TreeContext* ctx = vfs->ctx;

    /* Acquire exclusive tree lock — waits for all readers to drain */
    tree_lock_acquire_exclusive(ctx);

    /* Initialize the deferred-free queue */
    DeferredFreeQueue queue;
    int err = deferred_free_init(&queue, 256);
    if (err != VFS_OK) {
        ctx->last_error = err;
        tree_lock_release_exclusive(ctx);
        return err;
    }

    /* Tell the storage allocator to skip pages in our deferred queue */
    storage_set_deferred_queue(&queue);

    /* Run shadow-compaction */
    err = gc_shadow_compact(ctx, &queue);

    /* Release queued pages to storage and clean up */
    deferred_free_confirm_and_release(&queue, ctx->sb);
    storage_set_deferred_queue(NULL);

    /* Release the exclusive lock */
    tree_lock_release_exclusive(ctx);

    if (err != VFS_OK) ctx->last_error = err;
    return err;
}
