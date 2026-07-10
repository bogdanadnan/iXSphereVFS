/* hash_map.h — generic, lock-free-friendly, incrementally-allocated
 *               hash table for the VFS codebase.
 *
 * Same design pattern as var_array:
 *   - Layout-compatible base struct + typed struct macros
 *   - Storage backed by var_array (chunks of 256 slots)
 *   - Open addressing with linear probing
 *   - FNV-1a 64-bit hash on raw key bytes
 *   - Load factor 0.75, capacity always power of 2
 *   - Single-writer / lock-free-reads (no concurrent resize)
 *
 * GNU extensions required: statement expressions ({ ... }) and typeof().
 * Same compile flags as var_array apply.
 *
 * Usage:
 *   HashMap(int64_t, int64_t) map = hash_map_new(int64_t, int64_t);
 *   hash_map_put(map, 42, 100);
 *   int64_t* v = hash_map_get(map, 42);
 *   if (v) printf("value=%lld\n", (long long)*v);
 *   hash_map_free(map);
 */

#ifndef VFS_HASH_MAP_H
#define VFS_HASH_MAP_H

#include "var_array.h"
#include <stdint.h>
#include <stddef.h>

/* Default initial capacity for hash_map_new (no pre-size). */
#define HASH_MAP_DEFAULT_INITIAL_CAPACITY  16

/* Load factor threshold: resize when (size + tombstones) > capacity * 3 / 4.
   Stored as 3/4 = 0.75; using integer arithmetic (capacity * 3 / 4). */

/* Hash slot states.  0 is "empty" so a freshly allocated var_array (which
 * zero-initializes slots) appears as empty until written. */
#define HASH_EMPTY     0
#define HASH_OCCUPIED  1
#define HASH_TOMBSTONE 2

/* ---------------------------------------------------------------------------
 * HashSlot — one entry in the bucket array.
 *
 * 24 bytes: 8 (key) + 8 (value) + 1 (state) + 7 (padding) = 24 bytes.
 * 256 slots per var_array chunk = 6KB per chunk.
 * --------------------------------------------------------------------------- */
typedef struct {
    int64_t key;
    int64_t value;
    uint8_t state;
} HashSlot;

/* ---------------------------------------------------------------------------
 * HashMapBase — base struct for a typed HashMap.
 *
 * Layout-compatible with the typed HashMap(K, V)* because all four
 * members match by position and type.  Same anonymous-struct trick
 * used by VarArray(T) in var_array.h.
 *
 * Slots are stored in a VarArray(HashSlot).  HashSlot is always 24 bytes;
 * the var_array's element size is sizeof(HashSlot).
 *
 * size: number of OCCUPIED slots (excludes tombstones).
 * tombstones: number of HASH_TOMBSTONE slots.  Used to decide when to
 * resize: (size + tombstones) > capacity * 3/4.
 * --------------------------------------------------------------------------- */
typedef struct {
    VarArrayBase* slots;
    int64_t       capacity;
    volatile int64_t size;
    volatile int64_t tombstones;
} HashMapBase;

/* ---------------------------------------------------------------------------
 * Typed convenience macro — layout-compatible with HashMapBase*.
 *
 *   HashMap(int64_t, int64_t) m = hash_map_new(int64_t, int64_t);
 *
 * Each (K, V) pair produces a distinct anonymous struct, so the cast
 * from HashMap(K, V)* to HashMapBase* preserves field layout.  Requires
 * -Wno-incompatible-pointer-types (same flag as var_array).
 * --------------------------------------------------------------------------- */
#define HashMap(K, V) struct { \
    VarArrayBase* slots; \
    int64_t       capacity; \
    volatile int64_t size; \
    volatile int64_t tombstones; \
}*

/* ---------------------------------------------------------------------------
 * Base (untyped) API — operates on HashMapBase*.  All typed macros
 * below ultimately call these.  Kept public so future callers with
 * unusual types can bypass the macro layer if needed.
 * --------------------------------------------------------------------------- */

/* Allocate a new hash map with default initial capacity.  Returns NULL
   on OOM. */
HashMapBase* hash_map_base_new(void);

/* Allocate a new hash map pre-sized to (at least) `initial_capacity`
   slots.  The actual capacity is rounded up to the next power of 2
   (minimum 16).  Returns NULL on OOM. */
HashMapBase* hash_map_base_new_cap(int64_t initial_capacity);

/* Free the hash map and all its storage.  Idempotent (safe on NULL). */
void hash_map_base_free(HashMapBase* map);

/* Insert or update.  Returns 1 if a new key was inserted, 0 if an
   existing key was updated, -1 on error. */
int hash_map_base_put(HashMapBase* map, int64_t key, int64_t value);

/* Look up a key.  Returns pointer to the value slot, or NULL if not
   found.  The returned pointer is stable until the next
   put/delete/resize. */
int64_t* hash_map_base_get(HashMapBase* map, int64_t key);

/* Membership test (no value retrieval). */
int hash_map_base_contains(HashMapBase* map, int64_t key);

/* Delete a key.  Returns 1 if the key was present and removed, 0 if
   not found. */
int hash_map_base_delete(HashMapBase* map, int64_t key);

/* Number of occupied slots. */
int64_t hash_map_base_size(HashMapBase* map);

/* Hash function — FNV-1a 64-bit on the key's raw bytes.  Exposed for
   callers that want to pre-compute or test. */
uint64_t hash_key_64(int64_t key);

/* Resolve slot `target` to a pointer to the HashSlot within a chunk.
 *
 * var_array_resolve_base returns the chunk struct (whose second field
 * is the entries array).  We cast to a local struct that mirrors
 * VarArrayChunk's layout (volatile int height; HashSlot* entries).
 * Returns NULL if target is out of range.
 *
 * Inline so the iterator (in the header) can use it. */
typedef struct {
    volatile int height;
    HashSlot*    entries;
} HashMapChunk;

static inline HashSlot* hash_map_slot_ptr(VarArrayBase* a, int64_t target) {
    if (!a || target < 0 || target >= a->count) return NULL;
    void* chunk = var_array_resolve_base(a, target);
    if (!chunk) return NULL;
    return ((HashMapChunk*)chunk)->entries + (target % a->chunk_size);
}

/* ---------------------------------------------------------------------------
 * Iterator — walks all OCCUPIED slots in insertion order.
 *
 * Usage:
 *   HashMapIterator(int64_t, int64_t) it = {0};
 *   int64_t key, value;
 *   while (hash_map_iter_next(&it, map, &key, &value)) {
 *       printf("k=%lld v=%lld\n", (long long)key, (long long)value);
 *   }
 *
 * NOT safe across concurrent put/delete/resize.  Use the iterator
 * only when the map is otherwise quiescent.
 * --------------------------------------------------------------------------- */
typedef struct { int64_t _i; } HashMapIterator;

static inline void hash_map_iter_init(HashMapIterator* it) {
    it->_i = 0;
}

static inline int hash_map_iter_next(HashMapIterator* it,
                                    HashMapBase* map,
                                    int64_t* out_key,
                                    int64_t* out_value) {
    while (it->_i < map->slots->count) {
        HashSlot* s = hash_map_slot_ptr(map->slots, it->_i);
        it->_i++;
        if (s && s->state == HASH_OCCUPIED) {
            *out_key = s->key;
            *out_value = s->value;
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Typed macro wrappers — convenience layer over the base API.
 * Each macro expands with the same (K, V) pair that parameterizes the
 * HashMap(K, V) type, so the cast to HashMapBase* is always valid.
 * --------------------------------------------------------------------------- */

#define hash_map_new(K, V) \
    ((HashMap(K, V))hash_map_base_new())

#define hash_map_new_cap(K, V, cap) \
    ((HashMap(K, V))hash_map_base_new_cap(cap))

#define hash_map_free(map) \
    hash_map_base_free((HashMapBase*)(map))

#define hash_map_put(map, key, value) \
    hash_map_base_put((HashMapBase*)(map), (int64_t)(key), (int64_t)(value))

#define hash_map_get(map, key) \
    ((typeof((map)->size)*)hash_map_base_get((HashMapBase*)(map), (int64_t)(key)))

#define hash_map_contains(map, key) \
    hash_map_base_contains((HashMapBase*)(map), (int64_t)(key))

#define hash_map_delete(map, key) \
    hash_map_base_delete((HashMapBase*)(map), (int64_t)(key))

#define hash_map_size(map) \
    hash_map_base_size((HashMapBase*)(map))

#endif /* VFS_HASH_MAP_H */