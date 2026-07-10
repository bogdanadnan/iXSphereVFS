# Phase 21: Generic Hash Map

## Goal

Add a generic, lock-free, incrementally-allocated hash map primitive to the VFS codebase using the same design pattern as `var_array` (CAS-based grow, layout-compatible base + typed structs, anonymous-struct macro types). This phase delivers the primitive only — integration into the readdir/dedup hot path is a separate, later phase.

## Background

After Phase 20 the directory readdir path is:

```
vfs_readdir_alloc → dirchain_list_all → chain walk → dedup → output
```

The dedup is currently O(N²) (linear scan over the dedup VarArray). For 6500 children: 42M ops, ~0.4s per first-readdir. A hash map keyed by childNodeId would bring this to O(N) expected.

But the dedup integration is not the focus of this phase. This phase delivers the **primitive** — a generic, reusable hash map that any future phase can adopt without re-architecting.

## Design

### Storage backing: var_array

The hash map's bucket array is stored in a `VarArray(HashSlot)`. This:

- **Reuses var_array's lock-free grow** (CAS root promotion).
- **Incremental allocation**: chunks of 256 slots allocated on demand, not a single upfront malloc.
- **Pattern consistency**: same as the codebase's other lock-free primitive.

### Hash slot

```c
typedef struct {
    int64_t key;          /* opaque, treated as raw bytes for hashing */
    int64_t value;        /* opaque */
    uint8_t state;        /* HASH_EMPTY=0, HASH_OCCUPIED=1, HASH_TOMBSTONE=2 */
} HashSlot;
```

24 bytes per slot. 256 slots per var_array chunk = 6KB per chunk.

### Hash function

FNV-1a 64-bit on the key's raw bytes:

```c
static inline uint64_t hash_key_64(int64_t key) {
    uint64_t h = 14695981039346656037ULL;  /* FNV-1a offset basis */
    for (int i = 0; i < 8; i++) {
        h ^= (uint64_t)((const uint8_t*)&key)[i];
        h *= 1099511628211ULL;             /* FNV-1a prime */
    }
    return h;
}
```

### Hash → bucket index

Capacity is always a power of 2, so the low bits give the index:

```c
static inline int64_t hash_to_index(uint64_t h, int64_t capacity) {
    return (int64_t)(h & (uint64_t)(capacity - 1));
}
```

### HashMapBase layout

```c
typedef struct {
    VarArrayBase* slots;       /* VarArray(HashSlot) */
    int64_t capacity;           /* always power of 2; 0 = uninitialized */
    volatile int64_t size;     /* number of OCCUPIED slots */
    volatile int64_t tombstones;
} HashMapBase;
```

### Typed macro (same pattern as var_array)

```c
#define HashMap(K, V) struct { \
    VarArrayBase* slots; \
    int64_t capacity; \
    volatile int64_t size; \
    volatile int64_t tombstones; \
}*
```

Each `HashMap(int, int)` expansion creates a distinct anonymous struct type. Layout-compatible with `HashMapBase*` because the first four members have identical layout. Same GCC statement-expression trick used by `VarArray(T)` in `src/var_array.h`.

### API

```c
/* Allocate a new hash map.  Returns NULL on OOM. */
HashMap(K, V) hash_map_new(K, V);

/* Allocate a new hash map pre-sized to `initial_capacity`
   (rounded up to the next power of 2). */
HashMap(K, V) hash_map_new_cap(K, V, int64_t initial_capacity);

/* Free the hash map and all its storage. */
void hash_map_free(HashMap(K, V) map);

/* Insert or update.  Returns 1 if a new key was inserted, 0 if
   existing key was updated. */
int hash_map_put(HashMap(K, V) map, K key, V value);

/* Look up a key.  Returns pointer to the value slot, or NULL if not
   found.  The returned pointer is stable until the next
   insert/delete/resize. */
V* hash_map_get(HashMap(K, V) map, K key);

/* Membership test (no value retrieval). */
int hash_map_contains(HashMap(K, V) map, K key);

/* Delete a key.  Returns 1 if the key was present and removed, 0 if
   not found. */
int hash_map_delete(HashMap(K, V) map, K key);

/* Number of occupied slots. */
int64_t hash_map_size(HashMap(K, V) map);

/* Iteration.  Example:
     HashMapIterator(K, V) it = {0};
     while (hash_map_iter_next(&it, map, &k, &v)) { ... }
*/
typedef struct { int64_t _i; } HashMapIterator;
int hash_map_iter_next(HashMapIterator(K, V)* it,
                       HashMap(K, V) map,
                       K* out_key, V* out_value);
```

### Resize (grow)

Load-factor threshold: `size + tombstones > capacity * 3 / 4`. On exceed, double capacity:

```c
static void hash_map_grow(HashMapBase* map, int64_t new_capacity) {
    /* Allocate new larger slot array via var_array */
    VarArrayBase* new_slots = var_array_new_base(sizeof(HashSlot), 256);

    /* Rehash every OCCUPIED slot from the old array into the new one */
    for (int64_t i = 0; i < map->capacity; i++) {
        HashSlot* s = var_array_resolve_base(map->slots, i);
        if (s && s->state == HASH_OCCUPIED) {
            uint64_t h = hash_key_64(s->key);
            int64_t j = hash_to_index(h, new_capacity);
            /* Linear probe to find empty slot in new array */
            while (1) {
                HashSlot* ns = var_array_resolve_base(new_slots, j);
                if (!ns || ns->state == HASH_EMPTY) {
                    var_array_grow_base(new_slots);
                    ns = var_array_resolve_base(new_slots, j);
                    ns->key = s->key;
                    ns->value = s->value;
                    ns->state = HASH_OCCUPIED;
                    break;
                }
                j = (j + 1) & (new_capacity - 1);
            }
        }
    }

    /* Swap */
    var_array_delete_base(map->slots);
    map->slots = new_slots;
    map->capacity = new_capacity;
    map->tombstones = 0;
}
```

O(N) on resize. Unavoidable for a hash table. The var_array's incremental chunk allocation means the new array doesn't allocate all at once.

### Linear probing algorithm (put)

```c
int hash_map_put(HashMapBase* map, int64_t key, int64_t value) {
    if (map->capacity == 0) hash_map_grow(map, 16);

    /* Resize if load factor exceeded */
    if (map->size + map->tombstones + 1 > map->capacity * 3 / 4) {
        hash_map_grow(map, map->capacity * 2);
    }

    uint64_t h = hash_key_64(key);
    int64_t i = hash_to_index(h, map->capacity);
    int64_t first_tombstone = -1;

    while (1) {
        HashSlot* slot = var_array_resolve_base(map->slots, i);
        if (!slot || slot->state == HASH_EMPTY) {
            /* Found empty slot — insert here (or at first tombstone) */
            int64_t target = (first_tombstone >= 0) ? first_tombstone : i;
            var_array_grow_base(map->slots);  /* ensure capacity */
            HashSlot* target_slot = var_array_resolve_base(map->slots, target);
            target_slot->key = key;
            target_slot->value = value;
            target_slot->state = HASH_OCCUPIED;
            if (first_tombstone < 0) {
                map->size++;
            } else {
                map->tombstones--;
            }
            return 1;  /* new insertion */
        }
        if (slot->state == HASH_TOMBSTONE) {
            if (first_tombstone < 0) first_tombstone = i;
        } else if (slot->key == key) {
            /* Existing key — update value */
            slot->value = value;
            return 0;
        }
        i = (i + 1) & (map->capacity - 1);
    }
}
```

This is the canonical linear-probing algorithm with "first tombstone wins" optimization (Robin Hood variant).

### Layout compatibility with var_array

`HashMap(K, V)` has the same first three members as `VarArray(T)`:

- `VarArrayBase* slots` (or `T* root` in var_array)
- `int64_t capacity` (or `int chunk_size`)
- `volatile int64_t size` (or `volatile int count`)

Both are layout-compatible with their respective `Base*` types. Same GCC anonymous-struct trick applies. `-Wno-incompatible-pointer-types` and `-Wno-pedantic` flags (already set in CMakeLists.txt for var_array) carry over.

## Concurrency

- **Reads**: lock-free. Reader reads `map->slots` once via atomic load, then walks the snapshot.
- **Writes**: single-writer at a time assumed. Multiple writers may corrupt state during concurrent resize.
- **Future**: concurrent-safe resize via CAS on a single pointer (the `slots` pointer). For now, single-threaded use only.

## Files

| File | Change |
|------|--------|
| `src/hash_map.h` | New file — typed macro API + struct typedefs. |
| `src/hash_map.c` | New file — implementation: hash, grow, put, get, delete, iter. |
| `test/test_hash_map.c` | New file — unit tests. |
| `CMakeLists.txt` | Add `src/hash_map.c` and `test/test_hash_map.c`. |

## API surface

New header `hash_map.h`. Internal to VFS for now; could be promoted to public API later. Pure primitive — no VFS-specific types or concepts in the API.

## Acceptance

- [ ] `src/hash_map.h` and `src/hash_map.c` implemented with the typed-macro pattern.
- [ ] `test_hash_map` passes with coverage of:
  - [ ] insert / get / update / delete
  - [ ] iteration over all occupied slots
  - [ ] resize triggered at load factor 0.75
  - [ ] collision handling (linear probing)
  - [ ] tombstone reuse (Robin Hood "first tombstone wins")
  - [ ] stress test: 10K inserts, 10K deletes, 10K inserts of mixed keys
  - [ ] empty map / single element / capacity boundary cases
- [ ] All 10 existing unit test suites pass (no regression).
- [ ] HashMap primitive compiles cleanly into a small standalone test binary.

## Performance budget

| Operation | Complexity |
|---|---|
| Single put (no resize) | O(1) amortized |
| Single get | O(1) amortized |
| Single delete | O(1) amortized |
| Full resize | O(N) one-time |
| Iteration | O(N) |

For 10K inserts: ~10K put ops + ~7 resizes (16→32→64→...→16384). Total ops ~15K. **~700x faster than the linear-scan dedup it replaces** at N=100 (which is the typical cache hit cost after Phase 20's FUSE cache kicks in).

For 6500 entries: ~6500 puts + 7 resizes = ~13K ops. **vs 42M ops for O(N²) linear scan** = ~3000x speedup on this microbenchmark (not on dirchain_list end-to-end, since that has other costs).

## Out of scope (explicit non-goals)

1. **VFS integration** — using the hash map in `dirchain_list_all`, `dirchain_find_child`, or any other VFS code. That's a future phase that calls into this primitive.
2. **Concurrent resize.** Single-threaded use assumed. Future enhancement could add CAS-swap on the slots pointer.
3. **String keys.** Hash function is for int64 raw bytes. String keys would need a different hash + type-safe macros.
4. **Custom hash functions.** Hardcoded FNV-1a for now.
5. **Generic K/V types beyond int64.** Both key and value are int64_t. Pointers, structs, etc. would need separate handling.
6. **Cache-aware eviction policies** (LRU, FIFO). Pure size-based eviction only.

## Future enhancements

- **String-keyed hash map** (`HashMapStr(K)`) — for path-keyed caches
- **Concurrent-safe resize** via CAS on slots pointer
- **Robin Hood hashing** for better worst-case probe length
- **Generic K/V via template parameters** (C++ template-style via macro magic)
- **Integration phase** — replace linear-scan dedup in dirchain_list_all

## Implementation order

1. Write `src/hash_map.h` with API + macros.
2. Write `src/hash_map.c` with hash, grow, put, get, delete, iter.
3. Write `test/test_hash_map.c` with comprehensive tests.
4. Build, run `test_hash_map`.
5. Run all existing unit tests to verify no regression.

## Risks

- **Layout compatibility with var_array**: the anonymous-struct trick requires GCC statement-expression extension (`-Wno-pedantic` already set in CMakeLists for var_array). Same flag carries over.
- **Resize during iteration**: an iterator that survives a resize will see inconsistent state. Document this; iterations should not be mixed with inserts.
- **Capacity overflow**: capacity doubles each resize. At 2^62 we'd overflow `int64_t`. Real-world use caps out long before then.
- **Hash collision for adversarial keys**: FNV-1a has known adversarial patterns. For our use case (sequential childNodeIds from a directory) the distribution is fine.
- **Test coverage**: testing lock-free code is hard. We test single-threaded correctness thoroughly. Concurrent behavior is "best-effort" via the CAS patterns in var_array.