# Phase 21: Generic Hash Map

## Goal

Add a generic, lock-free, incrementally-allocated hash map to the VFS codebase using the same design pattern as `var_array` (CAS-based grow, layout-compatible base + typed structs, anonymous-struct macro types). Use it to replace O(N²) linear scans in directory-listing dedup, bringing uncached readdir of large directories from O(N²) to O(N).

## Background

After Phase 20 the directory readdir path is:

```
vfs_readdir_alloc → dirchain_list_all → chain walk → dedup (O(N²)) → output
```

The dedup is a linear scan over the dedup VarArray:

```c
for (int i = 0; i < dedup->count; i++) {
    DirchainDedupEntry* e = var_array_lookup(dedup, i);
    if (e && e->childNodeId == (int64_t)ce_child) { found = 1; break; }
}
```

For N unique children this is O(N²). For 6500 children (VSCode extract), that's 42M ops and ~0.4s per readdir. With the FUSE-side cache this happens once per `ls`, but it still dominates uncached readdir.

A hash map keyed by `childNodeId` reduces this to O(N) expected.

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

The slot stores 24 bytes. 256 slots per var_array chunk = 6KB per chunk.

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

Each `HashMap(int, int)` expansion creates a distinct anonymous struct type. Layout-compatible with `HashMapBase*` because the first four members have identical layout.

### API

```c
/* Allocate a new hash map.  Returns NULL on OOM. */
HashMap(K, V) hash_map_new(K, V);

/* Allocate a new hash map pre-sized to `initial_capacity` (rounded up to power of 2). */
HashMap(K, V) hash_map_new_cap(K, V, int64_t initial_capacity);

/* Free the hash map and all its storage. */
void hash_map_free(HashMap(K, V) map);

/* Insert or update.  Returns 1 if a new key was inserted, 0 if existing key updated. */
int hash_map_put(HashMap(K, V) map, K key, V value);

/* Look up a key.  Returns pointer to the value slot, or NULL if not found.
   The returned pointer is stable until the next insert/delete/resize. */
V* hash_map_get(HashMap(K, V) map, K key);

/* Membership test (no value retrieval).  Returns 1 if present, 0 otherwise. */
int hash_map_contains(HashMap(K, V) map, K key);

/* Delete a key.  Returns 1 if the key was present and removed, 0 if not found. */
int hash_map_delete(HashMap(K, V) map, K key);

/* Number of occupied slots. */
int64_t hash_map_size(HashMap(K, V) map);

/* Iteration.  Example:
     HashMapIterator(K, V) it = {0};
     while (hash_map_iter_next(&it, map, &key, &value)) { ... }
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
        HashSlot* s = var_array_lookup_base(map->slots, i);
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

This is O(N) on resize. Unavoidable for a hash table. The var_array's incremental chunk allocation means the new array doesn't allocate all at once.

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

HashMap(K, V) has the same first three members as VarArray(T):
- `VarArrayBase* slots` (or `T* root` in var_array)
- `int64_t capacity` (or `int chunk_size`)
- `volatile int64_t size` (or `volatile int count`)

Both are layout-compatible with their respective `Base*` types. Same GCC anonymous-struct trick applies.

## Concurrency

- **Reads**: lock-free. Reader reads `map->slots` once via atomic load, then walks the snapshot.
- **Writes**: single-writer at a time assumed. Multiple writers may corrupt state during concurrent resize.
- **Future**: concurrent-safe resize via CAS on a single pointer (the `slots` pointer). For now, single-threaded use only.

The dedup use case in `dirchain_list` is single-threaded (FUSE callbacks per mount are serialized). So single-writer semantics suffice for the immediate goal.

## Files

| File | Change |
|------|--------|
| `src/hash_map.h` | New file — typed macro API + struct typedefs. |
| `src/hash_map.c` | New file — implementation: hash, grow, put, get, delete, iter. |
| `test/test_hash_map.c` | New file — unit tests covering insert/get/delete/iterate/resize/concurrent. |
| `src/tree.c` | Replace linear scan in `dirchain_list_all` with `HashMap(int64_t, int64_t)` for `childNodeId` dedup. |
| `CMakeLists.txt` | Add `src/hash_map.c` and `test/test_hash_map.c`. |

## API surface

New public-style header `hash_map.h`. Internal to VFS for now; could be promoted to public API later.

## Acceptance

- [ ] `src/hash_map.h` and `src/hash_map.c` implemented with the typed-macro pattern.
- [ ] `test_hash_map` passes with coverage of:
  - [ ] insert / get / update / delete
  - [ ] iteration over all occupied slots
  - [ ] resize triggered at load factor 0.75
  - [ ] collision handling (linear probing)
  - [ ] tombstone reuse
  - [ ] stress test: 10K inserts, 10K deletes, 10K inserts of mixed keys
- [ ] `dirchain_list_all` replaced with hash-map-based dedup. Same behavior, O(N) instead of O(N²).
- [ ] All 10 existing unit test suites pass.
- [ ] `bench_ditto.py`: extract throughput improves (or at minimum stays same).
- [ ] Sampled FUSE scenarios: no regression.

## Performance budget

For N unique children:

| Operation | Before (linear scan) | After (hash map) |
|---|---|---|
| Single dedup (per chain entry) | O(N) | O(1) amortized |
| Full dedup pass | O(N²) | O(N) |
| Resize | N/A | O(N) one-time |

For 6500 entries (VSCode extract dir):
- Before: 6500² = 42M ops, ~0.4s
- After: 6500 ops, ~10µs
- **~40000x speedup on dedup pass**

The first readdir of a large directory (uncached) drops from 0.4s to ~10µs for dedup. Total readdir cost becomes I/O bound (chain walk), not CPU bound.

## Out of scope (explicit non-goals)

1. **Concurrent resize.** Single-threaded use assumed. Future phase could add CAS-swap on the slots pointer.
2. **String keys.** Hash function is for int64 raw bytes. String keys need a different hash (FNV-1a on bytes works, but API needs separate type-safe macros).
3. **Custom hash functions.** Hardcoded FNV-1a for now. Could be parameterized.
4. **Generic K/V types beyond int64.** Both key and value are int64_t. Pointers, structs, etc. would need separate handling.
5. **Delete-then-reinsert correctness.** Verified by unit tests but not stress-tested.

## Implementation order

1. Write `src/hash_map.h` with API + macros.
2. Write `src/hash_map.c` with hash, grow, put, get, delete, iter.
3. Write `test/test_hash_map.c` with comprehensive tests.
4. Build, run tests.
5. Replace linear scan in `dirchain_list_all` with hash map.
6. Build, run all unit tests + bench + sampled FUSE scenarios.

## Risks

- **Layout compatibility with var_array**: the anonymous-struct trick requires GCC statement-expression extension (`-Wno-pedantic` already set in CMakeLists for var_array). Same flag carries over.
- **Resize during iteration**: an iterator that survives a resize will see inconsistent state. Document this; iterations should not be mixed with inserts.
- **Capacity overflow**: capacity doubles each resize. At 2^62 we'd overflow `int64_t`. Real-world use caps out long before then.
- **Hash collision for adversarial keys**: FNV-1a has known adversarial patterns. For our use case (sequential childNodeIds from a directory) the distribution is fine.

## Future enhancements

- String-keyed hash map (`HashMapStr(K)`)
- Concurrent-safe resize via CAS on slots pointer
- Robin Hood hashing for better worst-case probe length
- Generic K/V via template parameters (C++ template-style via macro magic)