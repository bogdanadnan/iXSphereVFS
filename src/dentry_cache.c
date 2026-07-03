/* Phase 5f: Directory entry cache with read-rule dedup */
#include "dentry_cache.h"
#include "nodes.h"
#include <string.h>

int dentry_cache_build(Pool* pool, int64_t root_vp, int64_t epoch,
                       DentryCache* arr) {
    uint8_t* dir_slot = pool_resolve(pool, root_vp);
    if (!dir_slot) return VFS_ERR_IO;

    /* Read headPtr and record its page for validity checks */
    int64_t headPtr = vfs_rd8_s(dir_slot, DIRNODE_OFF_HEADPTR, VFS_PAGE_SIZE);
    arr->last_headPtr_page = VFS_VPTR_PAGE(headPtr);
    arr->count = 0;
    uint32_t query_epoch = (uint32_t)epoch;

    /* Temporary array to track best epoch per childNodeId */
    int64_t best_child[DENTRY_CACHE_MAX];
    int64_t best_childPtr[DENTRY_CACHE_MAX];
    uint32_t best_epoch[DENTRY_CACHE_MAX];
    int best_name_set[DENTRY_CACHE_MAX]; /* 1 if name was set (not tombstone) */
    int best_count = 0;

    /* Walk the DirContent chain (descending epoch order) */
    int64_t walk_vp = headPtr;
    while (walk_vp != 0 && best_count < DENTRY_CACHE_MAX) {
        uint8_t* dc_slot = pool_resolve(pool, walk_vp);
        if (!dc_slot) break;

        uint32_t ce_child, ce_epoch;
        int64_t ce_childPtr, ce_namePtr, ce_next;
        nodes_read_dircontent(dc_slot, &ce_child, &ce_epoch, &ce_childPtr,
                              &ce_namePtr, &ce_next, VFS_PAGE_SIZE);

        /* Read-rule: entry applies if epoch == query_epoch,
           or epoch < query_epoch AND even */
        int applies = (ce_epoch == query_epoch) ||
                      (ce_epoch < query_epoch && ce_epoch % 2 == 0);

        if (!applies) { walk_vp = ce_next; continue; }

        /* Check if we already have this childNodeId */
        int found = 0;
        for (int i = 0; i < best_count && !found; i++) {
            if (best_child[i] == (int64_t)ce_child) {
                found = 1;
                /* Keep the entry with higher epoch */
                if (ce_epoch > best_epoch[i]) {
                    best_epoch[i] = ce_epoch;
                    best_childPtr[i] = ce_childPtr;
                    best_name_set[i] = (ce_namePtr != 0);
                }
            }
        }

        if (!found) {
            /* New childNodeId — add to tracking array */
            best_child[best_count] = (int64_t)ce_child;
            best_childPtr[best_count] = ce_childPtr;
            best_epoch[best_count] = ce_epoch;
            best_name_set[best_count] = (ce_namePtr != 0);
            best_count++;
        }

        walk_vp = ce_next;
    }

    /* Build final entries array from tracking data, skipping tombstones */
    for (int i = 0; i < best_count && arr->count < DENTRY_CACHE_MAX; i++) {
        if (!best_name_set[i]) continue;  /* tombstone — skip */

        DentryEntry* entry = &arr->entries[arr->count];
        entry->childNodeId = best_child[i];
        entry->childPtr = best_childPtr[i];
        entry->name[0] = '\0';
        entry->isDir = false;

        /* Determine isDir by reading the child's type field */
        uint8_t* child_slot = pool_resolve(pool, best_childPtr[i]);
        if (child_slot) {
            int16_t type = vfs_rd2_s(child_slot, DIRNODE_OFF_TYPE, VFS_PAGE_SIZE);
            entry->isDir = (type == (int16_t)NODE_TYPE_DIR);
        }

        /* Find the name: walk the chain again for this specific entry.
           We need the namePtr from the highest-epoch entry. */
        walk_vp = headPtr;
        while (walk_vp != 0) {
            uint8_t* dc_slot = pool_resolve(pool, walk_vp);
            if (!dc_slot) break;
            uint32_t dc_child, dc_epoch;
            int64_t dc_childPtrv, dc_namePtr, dc_next;
            nodes_read_dircontent(dc_slot, &dc_child, &dc_epoch, &dc_childPtrv,
                                  &dc_namePtr, &dc_next, VFS_PAGE_SIZE);
            (void)dc_childPtrv;

            if ((int64_t)dc_child == best_child[i] &&
                dc_epoch == best_epoch[i] && dc_namePtr != 0) {
                /* Found the matching DirContent — resolve name */
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
