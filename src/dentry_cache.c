/* Phase 5f: Directory entry cache with read-rule dedup */
#include "dentry_cache.h"
#include "nodes.h"
#include <string.h>

int dentry_cache_build(Pool* pool, Mapper* mapper, int64_t root_vp, int64_t epoch,
                       DentryCache* arr) {
    if (!pool || !mapper) return VFS_ERR_IO;
    uint8_t* dir_slot = pool_resolve(pool, root_vp);
    if (!dir_slot) return VFS_ERR_IO;

    int64_t headPtr = vfs_rd8_s(dir_slot, DIRNODE_OFF_HEADPTR, pool->sb->page_size);
    arr->last_headPtr_page = VFS_VPTR_PAGE(headPtr);
    arr->count = 0;
    int64_t read_epoch = mapper_resolve(mapper, epoch);
    (void)read_epoch;

    /* Temporary array to track best epoch per childNodeId */
    int64_t best_child[DENTRY_CACHE_MAX];
    int64_t best_childPtr[DENTRY_CACHE_MAX];
    int64_t best_effective_epoch[DENTRY_CACHE_MAX];
    int best_name_set[DENTRY_CACHE_MAX];
    int best_count = 0;

    int64_t walk_vp = headPtr;
    while (walk_vp != 0 && best_count < DENTRY_CACHE_MAX) {
        uint8_t* dc_slot = pool_resolve(pool, walk_vp);
        if (!dc_slot) break;

        uint32_t ce_child, ce_epoch;
        int64_t ce_childPtr, ce_namePtr, ce_next;
        nodes_read_dircontent(dc_slot, &ce_child, &ce_epoch, &ce_childPtr,
                              &ce_namePtr, &ce_next, pool->sb->page_size);

        /* Compute effective epoch via mapper remapping */
        int64_t effective_epoch = (int64_t)ce_epoch;
        if (mapper_traversal_apply(mapper, (int64_t)ce_epoch))
            effective_epoch = mapper_resolve(mapper, (int64_t)ce_epoch);

        /* Read-rule: applies if effective_epoch == read_epoch,
           or effective_epoch < read_epoch AND even */
        int applies = (effective_epoch == read_epoch) ||
                      (effective_epoch < read_epoch && effective_epoch % 2 == 0);
        if (!applies) { walk_vp = ce_next; continue; }

        int found = 0;
        for (int i = 0; i < best_count && !found; i++) {
            if (best_child[i] == (int64_t)ce_child) {
                found = 1;
                if (effective_epoch > best_effective_epoch[i]) {
                    best_effective_epoch[i] = effective_epoch;
                    best_childPtr[i] = ce_childPtr;
                    best_name_set[i] = (ce_namePtr != 0);
                }
            }
        }

        if (!found) {
            best_child[best_count] = (int64_t)ce_child;
            best_childPtr[best_count] = ce_childPtr;
            best_effective_epoch[best_count] = effective_epoch;
            best_name_set[best_count] = (ce_namePtr != 0);
            best_count++;
        }

        walk_vp = ce_next;
    }

    for (int i = 0; i < best_count && arr->count < DENTRY_CACHE_MAX; i++) {
        if (!best_name_set[i]) continue;

        DentryEntry* entry = &arr->entries[arr->count];
        entry->childNodeId = best_child[i];
        entry->childPtr = best_childPtr[i];
        entry->name[0] = '\0';
        entry->isDir = false;

        uint8_t* child_slot = pool_resolve(pool, best_childPtr[i]);
        if (child_slot) {
            int16_t type = vfs_rd2_s(child_slot, DIRNODE_OFF_TYPE, pool->sb->page_size);
            entry->isDir = (type == (int16_t)NODE_TYPE_DIR);
        }

        /* Find the name by walking chain for this specific entry */
        walk_vp = headPtr;
        while (walk_vp != 0) {
            uint8_t* dc_slot = pool_resolve(pool, walk_vp);
            if (!dc_slot) break;
            uint32_t dc_child, dc_epoch;
            int64_t dc_childPtrv, dc_namePtr, dc_next;
            nodes_read_dircontent(dc_slot, &dc_child, &dc_epoch, &dc_childPtrv,
                                  &dc_namePtr, &dc_next, pool->sb->page_size);
            (void)dc_childPtrv;

            /* Compute effective epoch for name matching */
            int64_t dc_eff = (int64_t)dc_epoch;
            if (mapper_traversal_apply(mapper, (int64_t)dc_epoch))
                dc_eff = mapper_resolve(mapper, (int64_t)dc_epoch);

            if ((int64_t)dc_child == best_child[i] &&
                dc_eff == best_effective_epoch[i] && dc_namePtr != 0) {
                nodes_read_name(pool, dc_namePtr, entry->name,
                                (int)sizeof(entry->name));
                break;
            }
            walk_vp = dc_next;
        }

        arr->count++;
    }

    arr->valid = true;
    return VFS_OK;
}

bool dentry_cache_is_valid(DentryCache* arr, int64_t headPtr) {
    return arr->valid && VFS_VPTR_PAGE(headPtr) == arr->last_headPtr_page;
}

void dentry_cache_invalidate(DentryCache* arr) {
    arr->valid = false;
}
