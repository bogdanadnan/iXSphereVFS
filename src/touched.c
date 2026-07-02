/* Phase 6d: TouchedFile chain management for commit conflict detection */
#include "touched.h"
#include <stdlib.h>
#include <string.h>

int touchedfile_add(Pool* pool, int64_t* touchedFilesPtr,
                    uint32_t epoch, uint32_t nodeId) {
    if (!pool || !touchedFilesPtr) return VFS_ERR_IO;

    /* Walk the chain to check for an existing entry (epoch, nodeId) */
    int64_t vp = *touchedFilesPtr;
    while (vp != 0) {
        uint8_t* slot = pool_resolve(pool, vp);
        if (!slot) return VFS_ERR_IO;

        uint32_t entry_epoch, entry_nodeId;
        int64_t entry_next;
        nodes_read_touchedfile(slot, &entry_epoch, &entry_nodeId, &entry_next);

        if (entry_epoch == epoch && entry_nodeId == nodeId)
            return VFS_OK;  /* already exists — idempotent */

        vp = entry_next;
    }

    /* Allocate pool slot for new TouchedFile entry */
    int64_t new_vp = pool_alloc(pool);
    if (new_vp == VFS_VPTR_NULL) return VFS_ERR_FULL;

    uint8_t* new_slot = pool_resolve(pool, new_vp);
    if (!new_slot) return VFS_ERR_IO;

    /* CAS-prepend to chain head */
    int64_t old_head;
    do {
        old_head = vfs_atomic_load_i64(touchedFilesPtr);
        nodes_write_touchedfile(new_slot, epoch, nodeId, old_head);
        vfs_mb_release();
    } while (vfs_cas_i64(touchedFilesPtr, old_head, new_vp) != old_head);

    return VFS_OK;
}

int touchedfile_collect(Pool* pool, int64_t touchedFilesPtr,
                        uint32_t epoch, uint32_t* out_nodeIds, int max_count) {
    if (!pool || !out_nodeIds || max_count <= 0) return 0;

    int count = 0;
    int64_t vp = touchedFilesPtr;
    while (vp != 0 && count < max_count) {
        uint8_t* slot = pool_resolve(pool, vp);
        if (!slot) break;

        uint32_t entry_epoch, entry_nodeId;
        int64_t entry_next;
        nodes_read_touchedfile(slot, &entry_epoch, &entry_nodeId, &entry_next);

        if (entry_epoch == epoch) {
            /* Dedup: check if this nodeId was already collected */
            int already = 0;
            for (int i = 0; i < count; i++) {
                if (out_nodeIds[i] == entry_nodeId) { already = 1; break; }
            }
            if (!already) {
                out_nodeIds[count++] = entry_nodeId;
            }
        }

        vp = entry_next;
    }

    return count;
}

void touchedfile_drop(Pool* pool, int64_t* touchedFilesPtr, uint32_t epoch) {
    (void)pool;
    (void)touchedFilesPtr;
    (void)epoch;
    /* Entries are reclaimed by GC during the next garbage collection cycle.
       No action needed here. */
}
