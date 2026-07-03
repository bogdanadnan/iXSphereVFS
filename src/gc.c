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
    pool_page_init(payload, VFS_PAGE_SIZE);

    /* Link the new page into the pool's free list */
    pool_list_add(&ctx->pool, page_idx, payload);

    /* Return the VirtualPtr of slot 0 on the new page (page_idx, slot=0) */
    return VFS_VPTR_MAKE(page_idx, 0);
}

/* ---------------------------------------------------------------------------
 * Entry copy with VirtualPtr remapping
 * --------------------------------------------------------------------------- */

void gc_copy_entry(GCMap* gc_map, int64_t old_vp, int64_t new_vp,
                   const uint8_t* old_slot, uint8_t* new_slot) {
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
            int64_t val = vfs_rd8(new_slot, off);
            if (val == 0) continue;  /* skip null — not in map */
            int64_t mapped = gc_map_get(gc_map, val);
            if (mapped != val) {
                vfs_wr8(new_slot, off, mapped);
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
                    int64_t dir_vp, int64_t epoch) {
    if (!ctx || !gc_map || !alloc || dir_vp == 0) return VFS_ERR_IO;

    uint8_t* dir_slot = pool_resolve(&ctx->pool, dir_vp);
    if (!dir_slot) return VFS_ERR_NOTFOUND;
    if (vfs_rd2(dir_slot, DIRNODE_OFF_TYPE) != (int16_t)NODE_TYPE_DIR)
        return VFS_ERR_NOTDIR;

    /* Helper: allocate a destination slot, advancing to a new page if needed */
    #define GC_NEXT_SLOT() do { \
        if (alloc->cur_slot >= alloc->slots_per_page) { \
            alloc->cur_page_vp = gc_allocate_new_pool_page(ctx, gc_map); \
            if (alloc->cur_page_vp == VFS_VPTR_NULL) return VFS_ERR_FULL; \
            alloc->cur_slot = 0; \
        } \
    } while(0)

    /* Allocate destination for the DirNode entry */
    GC_NEXT_SLOT();
    int64_t new_dir_vp = VFS_VPTR_MAKE(VFS_VPTR_PAGE(alloc->cur_page_vp),
                                        alloc->cur_slot);
    uint8_t* new_dir_slot = pool_resolve(&ctx->pool, new_dir_vp);
    if (!new_dir_slot) return VFS_ERR_IO;
    alloc->cur_slot++;

    /* Copy the DirNode with remapping */
    gc_copy_entry(gc_map, dir_vp, new_dir_vp, dir_slot, new_dir_slot);

    /* Walk the DirContent chain applying survival rules */
    int64_t headPtr = vfs_rd8(dir_slot, DIRNODE_OFF_HEADPTR);
    int64_t walk_vp = headPtr;
    while (walk_vp != 0) {
        uint8_t* dc_slot = pool_resolve(&ctx->pool, walk_vp);
        if (!dc_slot) break;

        uint32_t dc_child, dc_epoch;
        int64_t dc_childPtr, dc_namePtr, dc_next;
        nodes_read_dircontent(dc_slot, &dc_child, &dc_epoch, &dc_childPtr,
                              &dc_namePtr, &dc_next);

        /* Survival rule: keep DirContent if epoch is still live.
           TODO: full epoch-rule remapping in Phase 8 — currently keeps
           all entries at or above the threshold. */
        int keep = 1;
        if (dc_epoch > (uint32_t)epoch) keep = 0; /* future epoch — skip */

        if (keep) {
            GC_NEXT_SLOT();
            int64_t new_dc_vp = VFS_VPTR_MAKE(
                VFS_VPTR_PAGE(alloc->cur_page_vp), alloc->cur_slot);
            uint8_t* new_dc_slot = pool_resolve(&ctx->pool, new_dc_vp);
            if (!new_dc_slot) return VFS_ERR_IO;
            alloc->cur_slot++;

            gc_copy_entry(gc_map, walk_vp, new_dc_vp, dc_slot, new_dc_slot);

            /* Recursively walk child DirNodes */
            if (dc_namePtr != 0 && dc_childPtr != 0) {
                uint8_t* child_slot = pool_resolve(&ctx->pool, dc_childPtr);
                if (child_slot && vfs_rd2(child_slot, DIRNODE_OFF_TYPE)
                                  == (int16_t)NODE_TYPE_DIR) {
                    int err = gc_walk_dirnode(ctx, gc_map, alloc,
                                               dc_childPtr, epoch);
                    if (err != VFS_OK) return err;
                }
            }
            /* TODO Phase 8: walk child FileNodes to copy VersionPage chains */
        }

        walk_vp = dc_next;
    }

    return VFS_OK;
}

#undef GC_NEXT_SLOT

/* ---------------------------------------------------------------------------
 * FileNode walk — copy FileNode, walk FileContent/PageNode/VersionPage and
 * FileSize chains applying survival rules.
 * --------------------------------------------------------------------------- */

int gc_walk_filenode(TreeContext* ctx, GCMap* gc_map, GCAllocCursor* alloc,
                     int64_t file_vp, int64_t epoch) {
    if (!ctx || !gc_map || !alloc || file_vp == 0) return VFS_ERR_IO;
    (void)epoch;

    uint8_t* file_slot = pool_resolve(&ctx->pool, file_vp);
    if (!file_slot) return VFS_ERR_NOTFOUND;
    if (vfs_rd2(file_slot, FILENODE_OFF_TYPE) != (int16_t)NODE_TYPE_FILE)
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
    gc_copy_entry(gc_map, file_vp, new_file_vp, file_slot, new_file_slot);

    /* Walk FileSize chain (tracked via FileNode.sizePtr) */
    int64_t size_vp = vfs_rd8(file_slot, FILENODE_OFF_SIZEPTR);
    while (size_vp != 0) {
        uint8_t* fs_slot = pool_resolve(&ctx->pool, size_vp);
        if (!fs_slot) break;

        uint32_t fs_epoch;
        int64_t fs_modifiedAt, fs_fileSize, fs_next;
        nodes_read_filesize(fs_slot, &fs_epoch, &fs_modifiedAt,
                            &fs_fileSize, &fs_next);
        (void)fs_modifiedAt; (void)fs_fileSize;

        /* Survival rule: soft-deleted → DROP; committed → REWRITE; else KEEP */
        int keep = 1;
        int64_t rewrite_epoch = (int64_t)fs_epoch;
        if (fs_epoch % 2 == 1) {
            if (mapper_resolve(&ctx->mapper, (int64_t)fs_epoch) != (int64_t)fs_epoch) {
                if (mapper_traversal_apply(&ctx->mapper, (int64_t)fs_epoch)) {
                    /* Committed → REWRITE epoch to toEpoch */
                    rewrite_epoch = mapper_resolve(&ctx->mapper, (int64_t)fs_epoch);
                } else {
                    /* Soft-deleted → DROP */
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

            gc_copy_entry(gc_map, size_vp, new_fs_vp, fs_slot, new_fs_slot);

            /* Update the epoch field if rewritten */
            if (rewrite_epoch != (int64_t)fs_epoch) {
                vfs_wr4(new_fs_slot, FILESIZE_OFF_EPOCH, (uint32_t)rewrite_epoch);
            }
        }

        size_vp = fs_next;
    }

    /* Walk FileContent chain */
    int64_t fc_vp = vfs_rd8(file_slot, FILENODE_OFF_HEADPTR);
    while (fc_vp != 0) {
        uint8_t* fc_slot = pool_resolve(&ctx->pool, fc_vp);
        if (!fc_slot) break;

        int64_t fc_page_root = vfs_rd8(fc_slot, FILECONTENT_OFF_ROOTPTR);
        int64_t fc_next = vfs_rd8(fc_slot, FILECONTENT_OFF_NEXTPTR);

        /* Walk PageNode chain within this FileContent segment */
        int64_t pn_vp = fc_page_root;
        int segment_has_live = 0;

        /* First pass: count live VersionPages to decide if segment survives */
        while (pn_vp != 0) {
            uint8_t* pn_slot = pool_resolve(&ctx->pool, pn_vp);
            if (!pn_slot) break;

            int64_t vp_chain = vfs_rd8(pn_slot, PAGENODE_OFF_VERSIONROOT);
            while (vp_chain != 0) {
                uint8_t* vp_slot = pool_resolve(&ctx->pool, vp_chain);
                if (!vp_slot) break;
                uint32_t vp_epoch;
                int64_t vp_dataPage, vp_next;
                nodes_read_versionpage(vp_slot, &vp_epoch, &vp_dataPage, &vp_next);
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
            pn_vp = vfs_rd8(pn_slot, PAGENODE_OFF_NEXTPTR);
        }

        if (!segment_has_live) { fc_vp = fc_next; continue; }

        /* Copy the FileContent segment */
        GC_NEXT_SLOT();
        int64_t new_fc_vp = VFS_VPTR_MAKE(
            VFS_VPTR_PAGE(alloc->cur_page_vp), alloc->cur_slot);
        uint8_t* new_fc_slot = pool_resolve(&ctx->pool, new_fc_vp);
        if (!new_fc_slot) return VFS_ERR_IO;
        alloc->cur_slot++;
        gc_copy_entry(gc_map, fc_vp, new_fc_vp, fc_slot, new_fc_slot);

        /* Walk PageNode chain to copy live version pages */
        pn_vp = fc_page_root;
        while (pn_vp != 0) {
            uint8_t* pn_slot = pool_resolve(&ctx->pool, pn_vp);
            if (!pn_slot) break;
            int64_t pn_next = vfs_rd8(pn_slot, PAGENODE_OFF_NEXTPTR);

            int64_t vp_chain = vfs_rd8(pn_slot, PAGENODE_OFF_VERSIONROOT);
            int pn_has_live = 0;

            /* Check if any VersionPage in this PageNode survives */
            int64_t vp_walk = vp_chain;
            while (vp_walk != 0) {
                uint8_t* vp_slot = pool_resolve(&ctx->pool, vp_walk);
                if (!vp_slot) break;
                uint32_t vp_epoch;
                int64_t vp_dataPage, vp_next;
                nodes_read_versionpage(vp_slot, &vp_epoch, &vp_dataPage, &vp_next);
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
            gc_copy_entry(gc_map, pn_vp, new_pn_vp, pn_slot, new_pn_slot);

            /* Copy live VersionPages */
            vp_walk = vp_chain;
            while (vp_walk != 0) {
                uint8_t* vp_slot = pool_resolve(&ctx->pool, vp_walk);
                if (!vp_slot) break;
                uint32_t vp_epoch;
                int64_t vp_dataPage, vp_next;
                nodes_read_versionpage(vp_slot, &vp_epoch, &vp_dataPage, &vp_next);

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
                                  vp_slot, new_vp_slot);

                    if (rewrite_epoch_vp != (int64_t)vp_epoch) {
                        vfs_wr4(new_vp_slot, VERSIONPAGE_OFF_EPOCH,
                                (uint32_t)rewrite_epoch_vp);
                    }
                }

                vp_walk = vp_next;
            }

            pn_vp = pn_next;
        }

        fc_vp = fc_next;
    }

    #undef GC_NEXT_SLOT
    return VFS_OK;
}

/* Shadow-compaction helper — walks the pool chain, builds a live set,
   copies live pool entries to fresh pages, then enqueues old pages
   for deferred freeing.  Currently a stub — returns VFS_OK. */
static int gc_shadow_compact(TreeContext* ctx, DeferredFreeQueue* queue) {
    (void)ctx;
    (void)queue;
    /* Phase 8: walk pool chain, dentry cache, mapper chain, touched file
       chain to build live set; allocate fresh pool pages; copy live entries;
       CAS-switch pool list head; enqueue old pages for deferred free. */
    return VFS_OK;
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

    /* Flush superblock to persist any pool changes */
    tree_superblock_write(ctx);

    return err;
}
