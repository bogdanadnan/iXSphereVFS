/* Phase 7: GC — Tree Lock, Deferred Free Queue */
#include "gc.h"
#include "tree.h"
#include <stdlib.h>
#include <string.h>

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

    /* Enqueue mirror sibling if it exists */
    if (append_ok && sb && (uint64_t)logical_page < (uint64_t)sb->mirror_cap) {
        int32_t mirror = sb->mirror_pages[logical_page];
        if (mirror >= 0) {
            int64_t mirror_page = (int64_t)mirror;
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

        /* Remap VirtualPtrs within the slot.
           Pool entries contain VirtualPtrs at offsets 0, 8, 16, 24.
           Not all of these are VirtualPtrs (offset 0 may be a type field),
           but only values that exist as keys in gc_map will be remapped,
           which correctly filters out non-pointer fields. */
        for (int off = 0; off < VFS_POOL_SLOT_SIZE; off += 8) {
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

    uint8_t* dir_slot = pool_resolve(&ctx->pool, dir_vp);
    if (!dir_slot) return VFS_ERR_NOTFOUND;
    if (vfs_rd2_s(dir_slot, DIRNODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_DIR)
        return VFS_ERR_NOTDIR;

    /* Allocate destination for the DirNode entry */
    if (alloc->cur_slot >= alloc->slots_per_page) {
        alloc->cur_page_vp = gc_allocate_new_pool_page(ctx, gc_map);
        if (alloc->cur_page_vp == VFS_VPTR_NULL) return VFS_ERR_FULL;
        alloc->cur_slot = 0;
    }
    int64_t new_dir_vp = VFS_VPTR_MAKE(VFS_VPTR_PAGE(alloc->cur_page_vp),
                                        alloc->cur_slot);
    uint8_t* new_dir_slot = pool_resolve(&ctx->pool, new_dir_vp);
    if (!new_dir_slot) return VFS_ERR_IO;
    alloc->cur_slot++;

    /* Copy the DirNode with remapping */
    gc_copy_entry(gc_map, dir_vp, new_dir_vp, dir_slot, new_dir_slot, ctx->page_size);

    /* Read the old headPtr and delegate DirContent chain walk to the
       dedicated function which applies full survival rules. */
    int64_t headPtr = vfs_rd8_s(dir_slot, DIRNODE_OFF_HEADPTR, ctx->page_size);
    int err = gc_walk_dircontent_chain(ctx, gc_map, alloc, headPtr, epoch, lps);
    if (err != VFS_OK) return err;

    /* Remap the new DirNode's headPtr from the old headPtr.
       gc_walk_dircontent_chain copies surviving entries and records their
       old→new VirtualPtr mappings in gc_map.  We look up the old headPtr
       to get the new headPtr for the first DirContent entry. */
    int64_t new_headPtr = gc_map_get(gc_map, headPtr);
    vfs_wr8_s(new_dir_slot, DIRNODE_OFF_HEADPTR, new_headPtr, ctx->page_size);

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

    uint8_t* file_slot = pool_resolve(&ctx->pool, file_vp);
    if (!file_slot) return VFS_ERR_NOTFOUND;
    if (vfs_rd2_s(file_slot, FILENODE_OFF_TYPE, ctx->page_size) != (int16_t)NODE_TYPE_FILE)
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
    uint8_t* new_file_slot = pool_resolve(&ctx->pool, new_file_vp);
    if (!new_file_slot) return VFS_ERR_IO;
    alloc->cur_slot++;

    /* Copy the FileNode with remapping */
    gc_copy_entry(gc_map, file_vp, new_file_vp, file_slot, new_file_slot, ctx->page_size);

    /* Walk FileSize chain via gc_walk_filesize_chain — handles survival rules
       and returns the highest surviving file size for segment pruning. */
    int64_t highest_file_size = 0;
    int64_t size_vp = vfs_rd8_s(file_slot, FILENODE_OFF_SIZEPTR, ctx->page_size);
    int err_fs = gc_walk_filesize_chain(ctx, gc_map, alloc, size_vp, epoch,
                                         &highest_file_size);
    if (err_fs != VFS_OK) return err_fs;

    /* Walk FileContent chain.
       Skip segments whose page range is beyond the highest surviving
       file size bound (nothing above that size is reachable). */
    int64_t fc_vp = vfs_rd8_s(file_slot, FILENODE_OFF_HEADPTR, ctx->page_size);
    int64_t seg_idx = 0;
    while (fc_vp != 0) {
        uint8_t* fc_slot = pool_resolve(&ctx->pool, fc_vp);
        if (!fc_slot) break;

        /* Skip segments beyond the highest surviving file size */
        if (highest_file_size > 0 &&
            (seg_idx * (int64_t)ctx->segment_size * ctx->sb->page_size) >= highest_file_size) {
            int64_t fc_next = vfs_rd8_s(fc_slot, FILECONTENT_OFF_NEXTPTR, ctx->page_size);
            fc_vp = fc_next;
            seg_idx++;
            continue;
        }

        int64_t fc_page_root = vfs_rd8_s(fc_slot, FILECONTENT_OFF_ROOTPTR, ctx->page_size);
        int64_t fc_next = vfs_rd8_s(fc_slot, FILECONTENT_OFF_NEXTPTR, ctx->page_size);

        /* Walk PageNode chain within this FileContent segment */
        int64_t pn_vp = fc_page_root;
        int segment_has_live = 0;

        /* First pass: count live VersionPages to decide if segment survives */
        while (pn_vp != 0) {
            uint8_t* pn_slot = pool_resolve(&ctx->pool, pn_vp);
            if (!pn_slot) break;

            int64_t vp_chain = vfs_rd8_s(pn_slot, PAGENODE_OFF_VERSIONROOT, ctx->page_size);
            while (vp_chain != 0) {
                uint8_t* vp_slot = pool_resolve(&ctx->pool, vp_chain);
                if (!vp_slot) break;
                uint32_t vp_epoch;
                int64_t vp_dataPage, vp_next;
                nodes_read_versionpage(vp_slot, &vp_epoch, &vp_dataPage, &vp_next, ctx->page_size);
                (void)vp_dataPage;

                int live = 1;
                if (vp_epoch % 2 == 1) {
                    if (mapper_resolve(&ctx->mapper, (int64_t)vp_epoch) != (int64_t)vp_epoch) {
                        if (!mapper_traversal_apply(&ctx->mapper, (int64_t)vp_epoch)) {
                            live = 0;  /* soft-deleted → DROP */
                        }
                    }
                }
                if (live) { segment_has_live = 1; break; }
                vp_chain = vp_next;
            }
            if (segment_has_live) break;
            pn_vp = vfs_rd8_s(pn_slot, PAGENODE_OFF_NEXTPTR, ctx->page_size);
        }

        if (!segment_has_live) { fc_vp = fc_next; seg_idx++; continue; }

        /* Copy the FileContent segment */
        GC_NEXT_SLOT();
        int64_t new_fc_vp = VFS_VPTR_MAKE(
            VFS_VPTR_PAGE(alloc->cur_page_vp), alloc->cur_slot);
        uint8_t* new_fc_slot = pool_resolve(&ctx->pool, new_fc_vp);
        if (!new_fc_slot) return VFS_ERR_IO;
        alloc->cur_slot++;
        gc_copy_entry(gc_map, fc_vp, new_fc_vp, fc_slot, new_fc_slot, ctx->page_size);

        /* Walk PageNode chain to copy live version pages */
        pn_vp = fc_page_root;
        while (pn_vp != 0) {
            uint8_t* pn_slot = pool_resolve(&ctx->pool, pn_vp);
            if (!pn_slot) break;
            int64_t pn_next = vfs_rd8_s(pn_slot, PAGENODE_OFF_NEXTPTR, ctx->page_size);

            int64_t vp_chain = vfs_rd8_s(pn_slot, PAGENODE_OFF_VERSIONROOT, ctx->page_size);
            int pn_has_live = 0;

            /* Check if any VersionPage in this PageNode survives */
            int64_t vp_walk = vp_chain;
            while (vp_walk != 0) {
                uint8_t* vp_slot = pool_resolve(&ctx->pool, vp_walk);
                if (!vp_slot) break;
                uint32_t vp_epoch;
                int64_t vp_dataPage, vp_next;
                nodes_read_versionpage(vp_slot, &vp_epoch, &vp_dataPage, &vp_next, ctx->page_size);
                (void)vp_dataPage;

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
            if (!pn_has_live) { pn_vp = pn_next; continue; }

            /* Copy the PageNode */
            GC_NEXT_SLOT();
            int64_t new_pn_vp = VFS_VPTR_MAKE(
                VFS_VPTR_PAGE(alloc->cur_page_vp), alloc->cur_slot);
            uint8_t* new_pn_slot = pool_resolve(&ctx->pool, new_pn_vp);
            if (!new_pn_slot) return VFS_ERR_IO;
            alloc->cur_slot++;
            gc_copy_entry(gc_map, pn_vp, new_pn_vp, pn_slot, new_pn_slot, ctx->page_size);

            /* Copy live VersionPages */
            vp_walk = vp_chain;
            while (vp_walk != 0) {
                uint8_t* vp_slot = pool_resolve(&ctx->pool, vp_walk);
                if (!vp_slot) break;
                uint32_t vp_epoch;
                int64_t vp_dataPage, vp_next;
                nodes_read_versionpage(vp_slot, &vp_epoch, &vp_dataPage, &vp_next, ctx->page_size);

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
                    uint8_t* new_vp_slot = pool_resolve(&ctx->pool, new_vp_slot_vp);
                    if (!new_vp_slot) return VFS_ERR_IO;
                    alloc->cur_slot++;
                    gc_copy_entry(gc_map, vp_walk, new_vp_slot_vp,
                                  vp_slot, new_vp_slot, ctx->page_size);

                    if (lps && vp_dataPage > 0)
                        live_page_set_add(lps, vp_dataPage);

                    if (rewrite_epoch_vp != (int64_t)vp_epoch) {
                        vfs_wr4_s(new_vp_slot, VERSIONPAGE_OFF_EPOCH,
                                (uint32_t)rewrite_epoch_vp, ctx->page_size);
                    }
                }

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
        uint8_t* vp_slot = pool_resolve(&ctx->pool, vp_walk);
        if (!vp_slot) break;

        uint32_t vp_epoch;
        int64_t vp_dataPage, vp_next;
        nodes_read_versionpage(vp_slot, &vp_epoch, &vp_dataPage, &vp_next, ctx->page_size);
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
            uint8_t* new_slot = pool_resolve(&ctx->pool, new_vp);
            if (!new_slot) return VFS_ERR_IO;
            alloc->cur_slot++;

            gc_copy_entry(gc_map, vp_walk, new_vp, vp_slot, new_slot, ctx->page_size);

            /* Record the data page in the live set */
            if (lps && vp_dataPage > 0)
                live_page_set_add(lps, vp_dataPage);

            /* Update epoch field if rewritten */
            if (rewrite_epoch != (int64_t)vp_epoch) {
                vfs_wr4_s(new_slot, VERSIONPAGE_OFF_EPOCH,
                        (uint32_t)rewrite_epoch, ctx->page_size);
            }
        }

        vp_walk = vp_next;
    }

    #undef GC_NEXT_SLOT
    return VFS_OK;
}

/* ---------------------------------------------------------------------------
 * DirContent chain walk with survival rules
 * --------------------------------------------------------------------------- */

int gc_walk_dircontent_chain(TreeContext* ctx, GCMap* gc_map,
                              GCAllocCursor* alloc,
                              int64_t head_content_vp, int64_t epoch,
                              LivePageSet* lps) {
    if (!ctx || !gc_map || !alloc) return VFS_ERR_IO;

    #define GC_NEXT_SLOT() do { \
        if (alloc->cur_slot >= alloc->slots_per_page) { \
            alloc->cur_page_vp = gc_allocate_new_pool_page(ctx, gc_map); \
            if (alloc->cur_page_vp == VFS_VPTR_NULL) return VFS_ERR_FULL; \
            alloc->cur_slot = 0; \
        } \
    } while(0)

    /* First pass: collect unique childNodeIds, track best epoch and
       whether ANY entry for each child is a survivor (not deleted, name≠0). */
    #define MAX_CHILDREN 1024
    uint32_t child_ids[MAX_CHILDREN];
    uint32_t child_best_epoch[MAX_CHILDREN];
    int child_has_survivor[MAX_CHILDREN];  /* 1 = ANY entry is live */
    int child_count = 0;

    int64_t walk_vp = head_content_vp;
    while (walk_vp != 0 && child_count < MAX_CHILDREN) {
        uint8_t* dc_slot = pool_resolve(&ctx->pool, walk_vp);
        if (!dc_slot) break;

        uint32_t dc_child, dc_epoch;
        int64_t dc_childPtr, dc_namePtr, dc_next;
        nodes_read_dircontent(dc_slot, &dc_child, &dc_epoch, &dc_childPtr,
                              &dc_namePtr, &dc_next, ctx->page_size);
        (void)dc_childPtr;

        int epoch_deleted = 0;
        if (dc_epoch % 2 == 1) {
            int64_t resolved = mapper_resolve(&ctx->mapper, (int64_t)dc_epoch);
            if (resolved != (int64_t)dc_epoch &&
                !mapper_traversal_apply(&ctx->mapper, (int64_t)dc_epoch)) {
                epoch_deleted = 1;
            }
        }

        int found_idx = -1;
        for (int i = 0; i < child_count; i++) {
            if (child_ids[i] == dc_child) { found_idx = i; break; }
        }

        int this_live = (!epoch_deleted && dc_namePtr != 0) ? 1 : 0;

        if (found_idx >= 0) {
            if (dc_epoch > child_best_epoch[found_idx])
                child_best_epoch[found_idx] = dc_epoch;
            if (this_live) child_has_survivor[found_idx] = 1;
        } else {
            child_ids[child_count] = dc_child;
            child_best_epoch[child_count] = dc_epoch;
            child_has_survivor[child_count] = this_live;
            child_count++;
        }

        walk_vp = dc_next;
    }

    /* Early return for empty chain — avoids calloc(0, ...) edge cases */
    if (child_count == 0) {
        #undef GC_NEXT_SLOT
        return VFS_OK;
    }

    /* Second pass: copy surviving entries.
       After copy, track which children had at least one kept entry. */
    int* child_has_kept = (int*)calloc((size_t)child_count, sizeof(int));
    if (!child_has_kept) return VFS_ERR_NOMEM;

    walk_vp = head_content_vp;
    while (walk_vp != 0) {
        uint8_t* dc_slot = pool_resolve(&ctx->pool, walk_vp);
        if (!dc_slot) break;

        uint32_t dc_child, dc_epoch;
        int64_t dc_childPtr, dc_namePtr, dc_next;
        nodes_read_dircontent(dc_slot, &dc_child, &dc_epoch, &dc_childPtr,
                              &dc_namePtr, &dc_next, ctx->page_size);

        int epoch_deleted = 0;
        if (dc_epoch % 2 == 1) {
            int64_t resolved = mapper_resolve(&ctx->mapper, (int64_t)dc_epoch);
            if (resolved != (int64_t)dc_epoch &&
                !mapper_traversal_apply(&ctx->mapper, (int64_t)dc_epoch)) {
                epoch_deleted = 1;
            }
        }

        /* Find child index (match on childNodeId only) */
        int child_idx = -1;
        for (int i = 0; i < child_count; i++) {
            if (child_ids[i] == dc_child) { child_idx = i; break; }
        }

        /* Apply survival rules */
        int keep = 1;
        if (dc_namePtr == 0 && epoch_deleted) keep = 0;  /* tombstone for deleted */
        if (epoch_deleted && child_idx >= 0 && !child_has_survivor[child_idx])
            keep = 0;  /* deleted epoch, child has no survivor at any epoch */

        if (keep) {
            /* Inline allocation to free child_has_kept on OOM */
            if (alloc->cur_slot >= alloc->slots_per_page) {
                alloc->cur_page_vp = gc_allocate_new_pool_page(ctx, gc_map);
                if (alloc->cur_page_vp == VFS_VPTR_NULL) {
                    free(child_has_kept);
                    return VFS_ERR_FULL;
                }
                alloc->cur_slot = 0;
            }
            int64_t new_dc_vp = VFS_VPTR_MAKE(
                VFS_VPTR_PAGE(alloc->cur_page_vp), alloc->cur_slot);
            uint8_t* new_dc_slot = pool_resolve(&ctx->pool, new_dc_vp);
            if (!new_dc_slot) { free(child_has_kept); return VFS_ERR_IO; }
            alloc->cur_slot++;

            gc_copy_entry(gc_map, walk_vp, new_dc_vp, dc_slot, new_dc_slot, ctx->page_size);

            if (child_idx >= 0) child_has_kept[child_idx] = 1;
        }

        walk_vp = dc_next;
    }

    /* Third pass: recursively walk child nodes for any surviving entry */
    walk_vp = head_content_vp;
    while (walk_vp != 0) {
        uint8_t* dc_slot = pool_resolve(&ctx->pool, walk_vp);
        if (!dc_slot) break;

        uint32_t dc_child, dc_epoch;
        int64_t dc_childPtr, dc_namePtr, dc_next;
        nodes_read_dircontent(dc_slot, &dc_child, &dc_epoch, &dc_childPtr,
                              &dc_namePtr, &dc_next, ctx->page_size);

        /* Check if this child has any kept entry */
        int cidx = -1;
        for (int i = 0; i < child_count; i++) {
            if (child_ids[i] == dc_child) { cidx = i; break; }
        }

        if (cidx >= 0 && child_has_kept[cidx] && dc_namePtr != 0 && dc_childPtr != 0) {
            child_has_kept[cidx] = 0;  /* walk each child exactly once */
            uint8_t* child_slot = pool_resolve(&ctx->pool, dc_childPtr);
            if (child_slot) {
                int16_t ctype = vfs_rd2_s(child_slot, DIRNODE_OFF_TYPE, ctx->page_size);
                if (ctype == (int16_t)NODE_TYPE_DIR) {
                    int err = gc_walk_dirnode(ctx, gc_map, alloc,
                                               dc_childPtr, epoch, lps);
                    if (err != VFS_OK) { free(child_has_kept); return err; }
                } else if (ctype == (int16_t)NODE_TYPE_FILE) {
                    int err = gc_walk_filenode(ctx, gc_map, alloc,
                                                dc_childPtr, epoch, lps);
                    if (err != VFS_OK) { free(child_has_kept); return err; }
                }
            }
        }

        walk_vp = dc_next;
    }

    free(child_has_kept);
    #undef GC_NEXT_SLOT
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
        uint8_t* fs_slot = pool_resolve(&ctx->pool, fs_vp);
        if (!fs_slot) break;

        uint32_t fs_epoch;
        int64_t fs_modifiedAt, fs_fileSize, fs_next;
        nodes_read_filesize(fs_slot, &fs_epoch, &fs_modifiedAt,
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
            uint8_t* new_fs_slot = pool_resolve(&ctx->pool, new_fs_vp);
            if (!new_fs_slot) return VFS_ERR_IO;
            alloc->cur_slot++;

            gc_copy_entry(gc_map, fs_vp, new_fs_vp, fs_slot, new_fs_slot, ctx->page_size);

            /* Update epoch field if rewritten */
            if (rewrite_epoch != (int64_t)fs_epoch) {
                vfs_wr4_s(new_fs_slot, FILESIZE_OFF_EPOCH, (uint32_t)rewrite_epoch, ctx->page_size);
            }

            /* Track highest surviving file size */
            if (fs_fileSize > highest_file_size) highest_file_size = fs_fileSize;
        }

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
        uint8_t* slot = pool_resolve(&ctx->pool, vp);
        if (!slot) break;

        uint32_t fromEpoch, toEpoch;
        uint16_t flags;
        int64_t next;
        nodes_read_mapperentry(slot, &fromEpoch, &toEpoch, &flags, &next, ctx->page_size);

        int keep = 1;
        if (flags & MAPPER_FLAG_TRAVERSAL_APPLY) keep = 0;  /* committed */
        else if (fromEpoch % 2 == 1) keep = 0;               /* soft-deleted */

        if (keep) {
            GC_NEXT_SLOT();
            int64_t new_vp = VFS_VPTR_MAKE(
                VFS_VPTR_PAGE(alloc->cur_page_vp), alloc->cur_slot);
            uint8_t* new_slot = pool_resolve(&ctx->pool, new_vp);
            if (!new_slot) return VFS_ERR_IO;
            alloc->cur_slot++;

            gc_copy_entry(gc_map, vp, new_vp, slot, new_slot, ctx->page_size);
        }

        vp = next;
    }

    #undef GC_NEXT_SLOT
    return VFS_OK;
}

/* ---------------------------------------------------------------------------
 * GC touched file rebuild — drop all entries since they're rebuilt fresh
 * --------------------------------------------------------------------------- */

void gc_rebuild_touchedfiles(TreeContext* ctx) {
    if (!ctx) return;
    /* All existing TouchedFile entries become stale after GC because the
       VersionPage chains they reference have been rewritten.  Setting the
       chain head to 0 discards them; pool slots are reclaimed by GC. */
    ctx->touchedFilesPtr = 0;
}

/* ---------------------------------------------------------------------------
 * GC new superblock builder
 * --------------------------------------------------------------------------- */

/* Write the superblock page with GC-updated values after shadow-compaction.
 * rootNodeOffset, currentEpoch, and nextNodeId are preserved from ctx.
 * epochMapperPtr and poolListHead are the post-GC values.
 * treeLockState is written as 0 (exclusive lock released for new tree).
 * touchedFilesPtr is written as 0 (rebuilt for active epochs only). */
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
    vfs_wr8_s(buf, SB_OFF_TOUCHED_FILES_PTR, 0, ctx->page_size);

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

    /* Drop all TouchedFile entries */
    gc_rebuild_touchedfiles(ctx);

    /* Write the new superblock with post-GC values */
    int64_t new_pool_head = ctx->pool.list_head ? *ctx->pool.list_head : 0;
    int64_t new_mapper_ptr = ctx->epochMapperPtr;
    err = gc_build_new_superblock(ctx, new_mapper_ptr, new_pool_head);
    if (err != VFS_OK) {
        if (lps) live_page_set_destroy(lps);
        gc_map_destroy(&gc_map);
        return err;
    }

    /* Free dead data pages: iterate all logical pages from 2 upwards.
       Pool pages (already in deferred-free queue) are skipped.
       Data pages not in the live set are freed via storage_free. */
    if (lps) {
        int64_t total = ctx->sb->total_pages;
        for (int64_t page = 2; page < total; page++) {
            if (deferred_free_is_queued(queue, page))
                continue;
            if (!live_page_set_contains(lps, page))
                storage_free(ctx->sb, page);
        }
    }

    /* Enqueue old pool pages for deferred free.
       Walk the old pool list from the pre-GC chain head. */
    int64_t old_page = old_pool_list_head;
    while (old_page != 0) {
        deferred_free_enqueue(queue, old_page, ctx->sb);
        /* Read next page from the old chain by resolving the old page.
           Pool pages have a header with next-page pointer at offset 0. */
        uint8_t* old_header = storage_read(ctx->sb, old_page);
        if (!old_header) break;
        int64_t next_page = vfs_rd8_s(old_header, 0, ctx->page_size);
        old_page = next_page;
    }

    /* Destroy the gc_map and live page set */
    gc_map_destroy(&gc_map);
    if (lps) live_page_set_destroy(lps);

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
