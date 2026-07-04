/* Phase 6: Epoch mapper — single-hop epoch mappings with invariant enforcement */
#include "mapper.h"
#include <stdlib.h>

void mapper_init(Mapper* m, Pool* pool, int64_t* epochMapperPtr) {
    m->pool = pool;
    m->epochMapperPtr = epochMapperPtr;
}

int mapper_insert(Mapper* m, uint32_t fromEpoch, uint32_t toEpoch,
                  uint16_t flags) {
    if (!m->pool || !m->epochMapperPtr) return VFS_ERR_IO;

    /* Walk the chain to enforce single-hop invariant */
    int64_t vp = *m->epochMapperPtr;
    while (vp != 0) {
        uint8_t* slot = pool_resolve(m->pool, vp);
        if (!slot) return VFS_ERR_IO;

        uint32_t entry_from, entry_to;
        uint16_t entry_flags;
        int64_t entry_next;
        nodes_read_mapperentry(slot, &entry_from, &entry_to,
                               &entry_flags, &entry_next, m->pool->sb->page_size);

        /* Check if fromEpoch is already used as a source */
        if (entry_from == fromEpoch) return VFS_ERR_EXISTS;
        /* Check if toEpoch is already used as a target */
        if (entry_to == toEpoch) return VFS_ERR_EXISTS;
        /* Check if toEpoch is already used as a source (would create chain) */
        if (entry_from == toEpoch) return VFS_ERR_EXISTS;
        /* Check if fromEpoch is already used as a target (would create chain) */
        if (entry_to == fromEpoch) return VFS_ERR_EXISTS;

        vp = entry_next;
    }

    /* Allocate pool slot for new MapperEntry */
    int64_t new_vp = pool_alloc(m->pool);
    if (new_vp == VFS_VPTR_NULL) return VFS_ERR_FULL;

    uint8_t* new_slot = pool_resolve(m->pool, new_vp);
    if (!new_slot) return VFS_ERR_IO;

    /* CAS-prepend to chain head */
    int64_t old_head;
    do {
        old_head = vfs_atomic_load_i64(m->epochMapperPtr);
        nodes_write_mapperentry(new_slot, fromEpoch, toEpoch, flags, old_head, m->pool->sb->page_size);
        vfs_mb_release();
    } while (vfs_cas_i64(m->epochMapperPtr, old_head, new_vp) != old_head);

    return VFS_OK;
}

int64_t mapper_resolve(Mapper* m, int64_t epoch) {
    if (!m->pool || !m->epochMapperPtr) return epoch;

    uint32_t query = (uint32_t)epoch;
    int64_t vp = *m->epochMapperPtr;
    while (vp != 0) {
        uint8_t* slot = pool_resolve(m->pool, vp);
        if (!slot) break;

        uint32_t entry_from, entry_to;
        uint16_t entry_flags;
        int64_t entry_next;
        nodes_read_mapperentry(slot, &entry_from, &entry_to,
                               &entry_flags, &entry_next, m->pool->sb->page_size);
        (void)entry_flags;

        if (entry_from == query) return (int64_t)entry_to;
        vp = entry_next;
    }

    return epoch;  /* no mapping found — identity */
}

bool mapper_traversal_apply(Mapper* m, int64_t epoch) {
    if (!m->pool || !m->epochMapperPtr) return false;

    uint32_t query = (uint32_t)epoch;
    int64_t vp = *m->epochMapperPtr;
    while (vp != 0) {
        uint8_t* slot = pool_resolve(m->pool, vp);
        if (!slot) break;

        uint32_t entry_from, entry_to;
        uint16_t entry_flags;
        int64_t entry_next;
        nodes_read_mapperentry(slot, &entry_from, &entry_to,
                               &entry_flags, &entry_next, m->pool->sb->page_size);
        (void)entry_to;

        if (entry_from == query) return (entry_flags & MAPPER_FLAG_TRAVERSAL_APPLY) != 0;
        vp = entry_next;
    }

    return false;
}

int mapper_validate(Mapper* m) {
    if (!m->pool || !m->epochMapperPtr) return VFS_ERR_IO;

    int count = 0;
    int64_t vp = *m->epochMapperPtr;
    while (vp != 0) {
        uint8_t* slot = pool_resolve(m->pool, vp);
        if (!slot) return VFS_ERR_IO;

        uint32_t entry_from, entry_to;
        uint16_t entry_flags;
        int64_t entry_next;
        nodes_read_mapperentry(slot, &entry_from, &entry_to,
                               &entry_flags, &entry_next, m->pool->sb->page_size);
        (void)entry_from; (void)entry_to; (void)entry_flags;

        count++;
        vp = entry_next;
    }

    return count;
}

/* ---------------------------------------------------------------------------
 * MapperTable — in-memory snapshot for query-heavy workloads
 * --------------------------------------------------------------------------- */

int mapper_table_init(MapperTable* tbl, Pool* pool, int64_t* epochMapperPtr) {
    if (!tbl || !pool) return VFS_ERR_IO;
    tbl->pool = pool;
    tbl->epochMapperPtr = epochMapperPtr;
    tbl->count = 0;
    tbl->capacity = MAPPER_TABLE_INITIAL_CAPACITY;
    tbl->entries = (MapperEntryRow*)calloc((size_t)tbl->capacity, sizeof(MapperEntryRow));
    if (!tbl->entries) return VFS_ERR_NOMEM;

    /* Walk pool chain, populate entries */
    if (epochMapperPtr) {
        int64_t vp = *epochMapperPtr;
        while (vp != 0) {
            uint8_t* slot = pool_resolve(pool, vp);
            if (!slot) return VFS_ERR_IO;
            uint32_t fromE, toE;
            uint16_t flags;
            int64_t next;
            nodes_read_mapperentry(slot, &fromE, &toE, &flags, &next, pool->sb->page_size);

            /* Grow array if full */
            if (tbl->count >= tbl->capacity) {
                int new_cap = tbl->capacity * 2;
                MapperEntryRow* new_entries = (MapperEntryRow*)realloc(
                    tbl->entries, (size_t)new_cap * sizeof(MapperEntryRow));
                if (!new_entries) return VFS_ERR_NOMEM;
                tbl->entries = new_entries;
                tbl->capacity = new_cap;
            }

            tbl->entries[tbl->count].fromEpoch = fromE;
            tbl->entries[tbl->count].toEpoch = toE;
            tbl->entries[tbl->count].traversalApply =
                (flags & MAPPER_FLAG_TRAVERSAL_APPLY) != 0;
            tbl->count++;

            vp = next;
        }
    }
    return VFS_OK;
}

void mapper_table_destroy(MapperTable* tbl) {
    if (tbl) {
        free(tbl->entries);
        tbl->entries = NULL;
        tbl->count = 0;
        tbl->capacity = 0;
    }
}

int64_t mapper_table_resolve(MapperTable* tbl, int64_t epoch) {
    if (!tbl || !tbl->entries) return epoch;
    uint32_t query = (uint32_t)epoch;
    for (int i = 0; i < tbl->count; i++) {
        if (tbl->entries[i].fromEpoch == query)
            return (int64_t)tbl->entries[i].toEpoch;
    }
    return epoch;
}

bool mapper_table_traversal_apply(MapperTable* tbl, int64_t epoch) {
    if (!tbl || !tbl->entries) return false;
    uint32_t query = (uint32_t)epoch;
    for (int i = 0; i < tbl->count; i++) {
        if (tbl->entries[i].fromEpoch == query)
            return tbl->entries[i].traversalApply;
    }
    return false;
}

int mapper_table_insert(MapperTable* tbl, uint32_t fromEpoch, uint32_t toEpoch,
                         bool traversalApply) {
    if (!tbl || !tbl->pool || !tbl->entries) return VFS_ERR_IO;

    uint16_t flags = traversalApply ? MAPPER_FLAG_TRAVERSAL_APPLY : 0;
    {
        Mapper m;
        mapper_init(&m, tbl->pool, tbl->epochMapperPtr);
        int err = mapper_insert(&m, fromEpoch, toEpoch, flags);
        if (err != VFS_OK) return err;
    }

    if (tbl->count >= tbl->capacity) {
        int new_cap = tbl->capacity * 2;
        MapperEntryRow* new_entries = (MapperEntryRow*)realloc(
            tbl->entries, (size_t)new_cap * sizeof(MapperEntryRow));
        if (!new_entries) return VFS_ERR_NOMEM;
        tbl->entries = new_entries;
        tbl->capacity = new_cap;
    }

    tbl->entries[tbl->count].fromEpoch = fromEpoch;
    tbl->entries[tbl->count].toEpoch = toEpoch;
    tbl->entries[tbl->count].traversalApply = traversalApply;
    vfs_mb_release();
    tbl->count++;

    return VFS_OK;
}
