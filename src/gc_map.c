/* Phase 7: GC VirtualPtr remapping hash map — open-addressing with tombstones */
#include "gc_map.h"
#include <stdlib.h>
#include <string.h>

static int round_pow2(int n) {
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

int gc_map_init(GCMap* map, int initial_capacity) {
    if (!map || initial_capacity <= 0) return VFS_ERR_IO;
    int cap = round_pow2(initial_capacity);
    map->entries = (GCMapEntry*)calloc((size_t)cap, sizeof(GCMapEntry));
    if (!map->entries) return VFS_ERR_NOMEM;
    map->count = 0;
    map->capacity = cap;
    return VFS_OK;
}

/* Probe for old_vp: return slot index if found, or first available slot
   (empty or tombstone) if not found.  Skip tombstones during search —
   only stop at empty (0) or match. */
static int gc_map_probe(GCMap* map, int64_t old_vp) {
    if (!map->entries || old_vp == 0) return -1;
    int mask = map->capacity - 1;
    int idx = (int)((uint64_t)old_vp >> 3) & mask;
    int first_tomb = -1;
    for (int i = 0; i < map->capacity; i++) {
        int slot = (idx + i) & mask;
        if (map->entries[slot].old_vp == old_vp) return slot;
        if (map->entries[slot].old_vp == GC_MAP_TOMB && first_tomb < 0)
            first_tomb = slot;
        if (map->entries[slot].old_vp == 0)
            return first_tomb >= 0 ? first_tomb : slot;
    }
    return first_tomb >= 0 ? first_tomb : -1;
}

int gc_map_put(GCMap* map, int64_t old_vp, int64_t new_vp) {
    if (!map || old_vp == 0) return VFS_ERR_IO;

    /* Grow if load factor exceeds 75% */
    if (map->count * 4 >= map->capacity * 3) {
        int new_cap = map->capacity * 2;
        GCMapEntry* new_entries = (GCMapEntry*)calloc((size_t)new_cap,
                                                      sizeof(GCMapEntry));
        if (!new_entries) return VFS_ERR_NOMEM;

        int new_mask = new_cap - 1;
        for (int i = 0; i < map->capacity; i++) {
            int64_t ovp = map->entries[i].old_vp;
            if (ovp == 0 || ovp == GC_MAP_TOMB) continue;
            int hidx = (int)((uint64_t)ovp >> 3) & new_mask;
            for (int j = 0; j < new_cap; j++) {
                int slot = (hidx + j) & new_mask;
                if (new_entries[slot].old_vp == 0) {
                    new_entries[slot].old_vp = ovp;
                    new_entries[slot].new_vp = map->entries[i].new_vp;
                    break;
                }
            }
        }
        free(map->entries);
        map->entries = new_entries;
        map->capacity = new_cap;
    }

    int slot = gc_map_probe(map, old_vp);
    if (slot < 0) return VFS_ERR_FULL;

    if (map->entries[slot].old_vp == 0 ||
        map->entries[slot].old_vp == GC_MAP_TOMB) {
        map->entries[slot].old_vp = old_vp;
        map->count++;
    }
    map->entries[slot].new_vp = new_vp;
    return VFS_OK;
}

int64_t gc_map_get(GCMap* map, int64_t old_vp) {
    if (!map || !map->entries || old_vp == 0) return old_vp;
    int mask = map->capacity - 1;
    int idx = (int)((uint64_t)old_vp >> 3) & mask;
    for (int i = 0; i < map->capacity; i++) {
        int slot = (idx + i) & mask;
        if (map->entries[slot].old_vp == old_vp)
            return map->entries[slot].new_vp;
        if (map->entries[slot].old_vp == 0)
            return old_vp;
        /* GC_MAP_TOMB → continue probing */
    }
    return old_vp;
}

bool gc_map_remove(GCMap* map, int64_t old_vp) {
    int slot = gc_map_probe(map, old_vp);
    if (slot < 0 || map->entries[slot].old_vp != old_vp) return false;
    map->entries[slot].old_vp = GC_MAP_TOMB;
    map->entries[slot].new_vp = 0;
    map->count--;
    return true;
}

void gc_map_destroy(GCMap* map) {
    if (!map) return;
    free(map->entries);
    map->entries = NULL;
    map->count = 0;
    map->capacity = 0;
}
