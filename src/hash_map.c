/* hash_map.c — generic hash map, thin layer over Phase 22's sparse VarArray.
 *
 * Storage: VarArray(HashSlot) with chunks of 2^granularity slots each.
 * Capacity: 2^scale slots (fixed at construction, no resize).
 * Hash:     FNV-1a 64-bit, modulo capacity (power of 2 -> bitwise &).
 * Collision: linear probe with Robin Hood tombstone reuse.
 *
 * Phase 23 changes from Phase 21:
 *   - No hash_map_grow (no resize). Capacity is fixed at construction.
 *   - Storage uses var_array_set (path allocation handled by var_array).
 *   - Capacity/granularity configurable via scale/granularity log2 params.
 *   - hash_map_base_new_cap takes (scale, granularity) instead of
 *     a linear initial_capacity.
 */

#include "hash_map.h"
#include <stdlib.h>
#include <string.h>

/* Defaults: scale=12 (capacity 2^12=4096), granularity=8 (chunk 256).
 *
 * Tuned via microbench on typical dedup workloads.  scale=12 gives
 * 4096 slots which fits most directories with low load.  granularity=8
 * matches the smaller capacity (chunk_size^2 = 65536, so 4096 fits
 * comfortably in one root chunk).
 *
 * For larger directories (>4096 unique entries), the hash map will
 * saturate and put returns -1; callers should use hash_map_new_cap
 * explicitly with a higher scale in that case.
 */
#define HASH_MAP_DEFAULT_SCALE        12
#define HASH_MAP_DEFAULT_GRANULARITY   8
#define HASH_MAP_MIN_SCALE             1
#define HASH_MAP_MAX_SCALE            32

/* Granularity limits.  granularity in [1..16] => chunk_size in [2..2^16]. */
#define HASH_MAP_MIN_GRANULARITY       1
#define HASH_MAP_MAX_GRANULARITY      16

/* ---------------------------------------------------------------------------
 * Hash function — FNV-1a 64-bit on the key's raw bytes.
 *
 * Same primitive used in fuse_dir_cache.c (Phase 20).  Not
 * cryptographic — just needs to spread integer keys uniformly.
 * --------------------------------------------------------------------------- */

uint64_t hash_key_64(int64_t key) {
    uint64_t h = 14695981039346656037ULL;  /* FNV-1a offset basis */
    const uint8_t* p = (const uint8_t*)&key;
    for (int i = 0; i < 8; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;             /* FNV-1a prime */
    }
    return h;
}

/* Hash → bucket index.  Capacity must be a power of 2 (bitwise &).
 * Cast to uint64_t ensures the shift amount is well-defined and the
 * result fits in int64_t (capacity is at most 2^32 per scale cap). */
static inline int64_t hash_to_index(uint64_t h, int64_t capacity) {
    return (int64_t)(h & (uint64_t)(capacity - 1));
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------------------- */

/* Validate scale and granularity; clamp to legal range.  Returns 0 on
 * success, -1 if the inputs are nonsensical. */
static int clamp_scale_granularity(int* scale, int* granularity) {
    if (!scale || !granularity) return -1;
    if (*scale < HASH_MAP_MIN_SCALE)        *scale = HASH_MAP_MIN_SCALE;
    if (*scale > HASH_MAP_MAX_SCALE)        *scale = HASH_MAP_MAX_SCALE;
    if (*granularity < HASH_MAP_MIN_GRANULARITY)  *granularity = HASH_MAP_MIN_GRANULARITY;
    if (*granularity > HASH_MAP_MAX_GRANULARITY)  *granularity = HASH_MAP_MAX_GRANULARITY;
    return 0;
}

HashMapBase* hash_map_base_new(void) {
    return hash_map_base_new_cap(HASH_MAP_DEFAULT_SCALE,
                                 HASH_MAP_DEFAULT_GRANULARITY);
}

HashMapBase* hash_map_base_new_cap(int scale, int granularity) {
    if (clamp_scale_granularity(&scale, &granularity) != 0) return NULL;

    int64_t capacity = (int64_t)1 << scale;
    int chunk_size   = 1 << granularity;

    HashMapBase* map = (HashMapBase*)calloc(1, sizeof(HashMapBase));
    if (!map) return NULL;

    map->slots = var_array_new_base(sizeof(HashSlot), chunk_size);
    if (!map->slots) { free(map); return NULL; }

    map->capacity   = capacity;
    map->chunk_size = chunk_size;
    map->size       = 0;
    map->tombstones = 0;
    return map;
}

void hash_map_base_free(HashMapBase* map) {
    if (!map) return;
    if (map->slots) var_array_delete_base(map->slots);
    free(map);
}

/* ---------------------------------------------------------------------------
 * Lookup / Insert / Delete
 *
 * All three use linear probing.  Tombstones (HASH_TOMBSTONE state) are
 * kept for delete correctness — without them, the probe chain would
 * prematurely stop at a deleted slot, missing later entries.
 *
 * For the dedup use case (read-only, no deletes), tombstones never
 * appear and the probe chain is fast.
 * --------------------------------------------------------------------------- */

int64_t* hash_map_base_get(HashMapBase* map, int64_t key) {
    if (!map || map->capacity == 0) return NULL;

    uint64_t h = hash_key_64(key);
    int64_t i = hash_to_index(h, map->capacity);
    int64_t probes = 0;

    while (probes < map->capacity) {
        HashSlot* s = hash_map_slot_ptr(map->slots, i);
        if (!s || s->state == HASH_EMPTY) {
            return NULL;
        }
        if (s->state == HASH_OCCUPIED && s->key == key) {
            return &s->value;
        }
        i = (i + 1) & (map->capacity - 1);
        probes++;
    }
    return NULL;  /* not found within one full lap */
}

int hash_map_base_contains(HashMapBase* map, int64_t key) {
    return hash_map_base_get(map, key) != NULL;
}

int hash_map_base_put(HashMapBase* map, int64_t key, int64_t value) {
    if (!map) return -1;
    if (map->capacity == 0) return -1;  /* uninitialized */

    uint64_t h = hash_key_64(key);
    int64_t i = hash_to_index(h, map->capacity);
    int64_t first_tombstone = -1;
    int64_t probes = 0;

    while (probes < map->capacity) {
        HashSlot* s = hash_map_slot_ptr(map->slots, i);
        if (!s || s->state == HASH_EMPTY) {
            /* Found empty slot — insert here (or at first tombstone if any) */
            int64_t target = (first_tombstone >= 0) ? first_tombstone : i;
            HashSlot entry = { .key = key, .value = value, .state = HASH_OCCUPIED };
            var_array_set_base(map->slots, target, &entry);
            map->size++;
            if (first_tombstone >= 0) {
                /* Reused a tombstone slot — net tombstones decrease by 1. */
                map->tombstones--;
            }
            return 1;  /* new insertion */
        }
        if (s->state == HASH_TOMBSTONE) {
            if (first_tombstone < 0) first_tombstone = i;
        } else if (s->key == key) {
            /* Existing key — update value. */
            s->value = value;
            return 0;
        }
        i = (i + 1) & (map->capacity - 1);
        probes++;
    }

    /* Table is full (no EMPTY or matching OCCUPIED found within one
       full lap).  Caller should grow the table or accept that no
       more entries can be inserted.  Phase 23 has no auto-grow, so
       this returns -1. */
    return -1;
}

int hash_map_base_delete(HashMapBase* map, int64_t key) {
    if (!map || map->capacity == 0) return 0;

    uint64_t h = hash_key_64(key);
    int64_t i = hash_to_index(h, map->capacity);
    int64_t probes = 0;

    while (probes < map->capacity) {
        HashSlot* s = hash_map_slot_ptr(map->slots, i);
        if (!s || s->state == HASH_EMPTY) {
            return 0;  /* not found */
        }
        if (s->state == HASH_OCCUPIED && s->key == key) {
            /* Mark as tombstone by overwriting the slot.  The path is
               already allocated, so var_array_set is a simple write. */
            HashSlot tombstone = { .key = key, .value = 0, .state = HASH_TOMBSTONE };
            var_array_set_base(map->slots, i, &tombstone);
            map->size--;
            map->tombstones++;
            return 1;
        }
        i = (i + 1) & (map->capacity - 1);
        probes++;
    }
    return 0;  /* not found within one full lap */
}

int64_t hash_map_base_size(HashMapBase* map) {
    return map ? map->size : 0;
}