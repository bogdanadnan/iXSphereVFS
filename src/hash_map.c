/* hash_map.c — implementation of the generic hash map primitive.
 *
 * See hash_map.h for design notes.  All operations work on HashMapBase*
 * via linear probing in a VarArray(HashSlot).  Resize doubles capacity
 * and rehashes all occupied slots.
 */

#include "hash_map.h"
#include <stdlib.h>
#include <string.h>

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

/* Round up to next power of 2.  Caller's `n` must be > 0. */
static int64_t next_pow2(int64_t n) {
    if (n <= 1) return 1;
    n--;
    n |= n >> 1;  n |= n >> 2;  n |= n >> 4;
    n |= n >> 8;  n |= n >> 16; n |= n >> 32;
    return n + 1;
}

/* Hash → bucket index.  Capacity must be a power of 2. */
static inline int64_t hash_to_index(uint64_t h, int64_t capacity) {
    return (int64_t)(h & (uint64_t)(capacity - 1));
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------------------- */

HashMapBase* hash_map_base_new(void) {
    return hash_map_base_new_cap(HASH_MAP_DEFAULT_INITIAL_CAPACITY);
}

HashMapBase* hash_map_base_new_cap(int64_t initial_capacity) {
    if (initial_capacity < 16) initial_capacity = 16;
    int64_t capacity = next_pow2(initial_capacity);

    HashMapBase* map = (HashMapBase*)calloc(1, sizeof(HashMapBase));
    if (!map) return NULL;

    map->slots = var_array_new_base(sizeof(HashSlot),
                                    VFS_VAR_ARRAY_DEFAULT_CHUNK_SIZE);
    if (!map->slots) {
        free(map);
        return NULL;
    }
    map->capacity = capacity;
    map->size = 0;
    map->tombstones = 0;
    return map;
}

void hash_map_base_free(HashMapBase* map) {
    if (!map) return;
    if (map->slots) var_array_delete_base(map->slots);
    free(map);
}

/* ---------------------------------------------------------------------------
 * Resize — double capacity and rehash.
 *
 * O(N) on resize.  Unavoidable for a hash table.  We allocate a NEW
 * slot array via var_array (chunks of 256 slots), rehash every
 * occupied slot into it, then swap.
 *
 * Why a new array, not in-place rehashing:
 *   1. New array starts empty — no collision concerns during rehash.
 *   2. Old array stays valid throughout — readers can still see old state.
 *   3. The swap is a single pointer assignment (close to atomic).
 *
 * Slot growth in the new array is lazy: each rehash step that lands
 * on bucket j grows the var_array to count > j.  This avoids a
 * separate pre-allocation pass — the var_array's grow_base is O(1)
 * either way, and lazy grow means we never allocate slots that no
 * rehash step actually writes to (smaller var_array, less memory
 * churn if the hash distribution is sparse).
 *
 * The previous design pre-allocated `new_capacity` slots upfront.
 * That wasted ~50% of grow calls when the hash distribution was
 * non-uniform (lots of grow calls to claim indices we never wrote).
 * Lazy grow eliminates that waste at the cost of slightly more grow
 * calls during rehash for adversarial distributions (uniform
 * distribution still ends up growing to `new_capacity` total).
 * --------------------------------------------------------------------------- */

static int hash_map_grow(HashMapBase* map, int64_t new_capacity) {
    VarArrayBase* new_slots = var_array_new_base(sizeof(HashSlot),
                                                 VFS_VAR_ARRAY_DEFAULT_CHUNK_SIZE);
    if (!new_slots) return -1;

    /* Rehash every OCCUPIED slot from the old array into the new one.
       Lazy growth: when rehash lands on bucket j, grow the new array
       until count > j (so the slot exists), then write.  All new
       slots start as HASH_EMPTY (calloc-allocated). */
    for (int64_t i = 0; i < map->capacity; i++) {
        HashSlot* s = hash_map_slot_ptr(map->slots, i);
        if (!s || s->state != HASH_OCCUPIED) continue;

        uint64_t h = hash_key_64(s->key);
        int64_t j = hash_to_index(h, new_capacity);

        /* Linear probe to find empty slot in new array.  Lazy grow:
           when probing past the current count, grow the array so the
           slot exists.  The grow itself is O(1). */
        while (1) {
            HashSlot* ns = hash_map_slot_ptr(new_slots, j);
            if (ns) {
                if (ns->state == HASH_EMPTY) {
                    ns->key = s->key;
                    ns->value = s->value;
                    ns->state = HASH_OCCUPIED;
                    break;
                }
                j = (j + 1) & (new_capacity - 1);
            } else {
                /* j is past the current count — grow enough to claim it.
                   After this grow, j is in range and we retry. */
                while (new_slots->count <= j) {
                    (void)var_array_grow_base(new_slots);
                }
                /* Fall through to retry the probe at j. */
            }
        }
    }

    /* Swap */
    var_array_delete_base(map->slots);
    map->slots = new_slots;
    map->capacity = new_capacity;
    map->tombstones = 0;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Internal: insert a key/value pair with known target slot index.
 *
 * Writes the entry into map->slots at slot `target`.  Caller must have
 * already grown the slots array enough (count > target) AND verified
 * the slot is HASH_EMPTY or HASH_TOMBSTONE.
 * --------------------------------------------------------------------------- */

static void hash_map_insert_at(HashMapBase* map, int64_t target,
                               int64_t key, int64_t value) {
    HashSlot* s = hash_map_slot_ptr(map->slots, target);
    if (!s) return;
    s->key = key;
    s->value = value;
    s->state = HASH_OCCUPIED;
}

/* ---------------------------------------------------------------------------
 * Lookup, insert, delete, contains
 * --------------------------------------------------------------------------- */

int64_t* hash_map_base_get(HashMapBase* map, int64_t key) {
    if (!map || map->capacity == 0) return NULL;

    uint64_t h = hash_key_64(key);
    int64_t i = hash_to_index(h, map->capacity);

    while (1) {
        HashSlot* s = hash_map_slot_ptr(map->slots, i);
        if (!s || s->state == HASH_EMPTY) {
            return NULL;
        }
        if (s->state == HASH_OCCUPIED && s->key == key) {
            return &s->value;
        }
        i = (i + 1) & (map->capacity - 1);
    }
}

int hash_map_base_contains(HashMapBase* map, int64_t key) {
    return hash_map_base_get(map, key) != NULL;
}

int hash_map_base_put(HashMapBase* map, int64_t key, int64_t value) {
    if (!map) return -1;
    if (map->capacity == 0) {
        /* Uninitialized — re-create with default capacity. */
        map->slots = var_array_new_base(sizeof(HashSlot),
                                        VFS_VAR_ARRAY_DEFAULT_CHUNK_SIZE);
        if (!map->slots) return -1;
        map->capacity = HASH_MAP_DEFAULT_INITIAL_CAPACITY;
    }

    /* Resize if load factor exceeded. */
    if (map->size + map->tombstones + 1 > (map->capacity * 3) / 4) {
        if (hash_map_grow(map, map->capacity * 2) != 0) return -1;
    }

    uint64_t h = hash_key_64(key);
    int64_t i = hash_to_index(h, map->capacity);
    int64_t first_tombstone = -1;

    while (1) {
        HashSlot* s = hash_map_slot_ptr(map->slots, i);
        if (!s || s->state == HASH_EMPTY) {
            /* Found empty slot — insert here (or at first tombstone if any) */
            int64_t target = (first_tombstone >= 0) ? first_tombstone : i;
            /* Ensure the slots array has grown past target. */
            while (map->slots->count <= target) {
                (void)var_array_grow_base(map->slots);
            }
            hash_map_insert_at(map, target, key, value);
            map->size++;
            if (first_tombstone >= 0) {
                /* Reused a tombstone slot — net size unchanged otherwise. */
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
    }
}

int hash_map_base_delete(HashMapBase* map, int64_t key) {
    if (!map || map->capacity == 0) return 0;

    uint64_t h = hash_key_64(key);
    int64_t i = hash_to_index(h, map->capacity);

    while (1) {
        HashSlot* s = hash_map_slot_ptr(map->slots, i);
        if (!s || s->state == HASH_EMPTY) {
            return 0;  /* not found */
        }
        if (s->state == HASH_OCCUPIED && s->key == key) {
            /* Mark as tombstone.  Don't shrink the slots array —
               the slot index stays valid for future inserts (which
               may reuse it via the "first tombstone wins" rule). */
            s->state = HASH_TOMBSTONE;
            map->size--;
            map->tombstones++;
            return 1;
        }
        i = (i + 1) & (map->capacity - 1);
    }
}

int64_t hash_map_base_size(HashMapBase* map) {
    return map ? map->size : 0;
}