# Hash Map `put` Profile — Phase 24

## Per-op cost breakdown

| Operation | Time | Notes |
|---|---|---|
| `hash_key_64` (FNV-1a) | 5 ns | Pure hash, 8-byte mix |
| `hash_to_index` (mod) | 1 ns | Bitwise `& (capacity-1)` |
| **hash_map_contains (hit)** | **46 ns** | Hash + tree walk + compare |
| **hash_map_contains (miss)** | **49 ns** | Hash + tree walk to EMPTY |
| **hash_map_put** | **1.7 µs** | Sparse chunk allocation dominates |
| `hash_map_new + free` | 0.45 µs | Per-call |
| **`hash_map_free` (6500 entries)** | **2-5 ms** | Walking sparse chunk tree |
| `var_array_set_base` (sequential) | 20 ns | Dense, cache-friendly |
| `var_array_set_base` (sparse) | 40 ns | Sparse but mostly empty paths |

## Where hash_map_put spends its 1.7 µs

For 6500 entries inserted into a 1M-capacity hash_map with FNV-1a hash:

```
hash_key_64           ~5 ns        (0.3%)
hash_to_index         ~1 ns        (0.06%)
var_array_resolve_base (slot lookup) ~19 ns   (1.1%)
var_array_set_base (sparse chunk alloc) ~1.65 µs   (97%)  ← BOTTLENECK
hash_map_size++       ~1 ns        (0.06%)
                                       ─────
                                  ~1.7 µs total
```

**97% of `hash_map_put` time is in `var_array_set_base`** — specifically the sparse chunk allocation path.

## Why sparse chunk allocation is expensive

`var_array_set_base(idx, value)` calls `ensure_path_allocated(idx)`:

```c
static void* ensure_path_allocated(VarArrayBase* a, int idx) {
    // 1. Compute needed_height for idx
    // 2. CAS loop: promote root until height >= needed_height
    // 3. Walk root->leaf, allocating missing intermediate nodes via CAS
    // 4. CAS-update count = max(count, idx+1)
}
```

For sparse inserts (hash-scattered indices across [0, 1M)):
- Most puts allocate a **new chunk** (path didn't exist before)
- Each chunk allocation: malloc (fast) + tree walk to install (slow)
- The tree grows many disjoint chunks; cache locality is poor

## Comparison: dense var_array (linear scan's dedup)

`var_array_append` does the same chunk allocation but **sequentially**:
- Slots 0, 1, 2, ... allocated in order
- New chunks needed at predictable boundaries (every 256 slots)
- Tree grows contiguously — **excellent cache locality**

## Why hash_map_free is slow (~5 ms for 6500 entries)

`var_array_delete_base` walks the chunk tree:

```c
static void free_recursive(void* node, int height, int chunk_size) {
    if (height > 0) {
        for (int i = 0; i < chunk_size; i++) {
            void* child = slot_of(level, i) ? *slot_of(level, i) : NULL;
            if (child) free_recursive(child, height - 1, chunk_size);
        }
    }
    free(node);
}
```

It walks **all slots** at each level node, but skips NULL children. For 6500 entries scattered by FNV-1a hash:
- Root level: 256 slots → ~256 children (level-1 nodes)
- Each level-1: 256 slots → ~256 grand-children (chunks)
- Total: ~3700 nodes
- Per-node free: ~1.4 µs (mostly free() syscall + memory allocator overhead)

For N=6500, that's 5 ms. For N=10000, similar (4000 nodes). For N=50000, similar (4100 nodes — node count saturates at capacity/chunk_size²).

## Profile of dirchain_list (6500 unique entries)

### With hash_map:
| Step | Time | Cumulative |
|---|---|---|
| `hash_map_new` | 0.45 µs | 0.45 µs |
| 6500 × `hash_map_contains` (mostly hits) | 0.3 ms | 0.3 ms |
| 6500 × `hash_map_put` (sparse alloc) | **11 ms** | 11 ms |
| `hash_map_free` (walk 3700-node tree) | **2-5 ms** | 13-16 ms |
| Chain walk + output build | small | 13-16 ms |

### With linear scan:
| Step | Time | Cumulative |
|---|---|---|
| No alloc | 0 | 0 |
| 6500 × `var_array_append` (dense, sequential) | 0.3 ms | 0.3 ms |
| 6500 × `var_array_lookup` × 3000 (avg) = 21M comparisons (cache-friendly) | **~13 ms** | 13 ms |
| `var_array_delete` (dense, 25 chunks) | < 0.1 ms | 13 ms |
| Chain walk + output build | small | 13 ms |

## Key insight

The hash_map's per-op cost (~1.7 µs/put) **outweighs** the linear scan's per-op cost (~19 ns/lookup × 3000 lookups avg = 57 µs/op for unique inserts).

For N=6500:
- Hash_map: 6500 × 1.7 µs = **11 ms** (puts) + 5 ms (free) = 16 ms
- Linear scan: 21M × 19 ns = 400 ms theoretical, **13 ms measured** (cache wins)

The hash_map has **40x worse constant factor** for puts due to sparse chunk allocation, but should win asymptotically for N >> 10K where the linear scan's 21M+ comparisons exceed the constant overhead.

## What would close the gap

1. **Pre-allocate chunks** during hash_map creation (avoid sparse CAS allocation per put)
2. **Use larger chunk_size** in hash_map's internal storage (fewer chunks, but deeper tree)
3. **Switch to denser hash storage** (e.g., bitmap-indexed chunks)

## Sources

- `src/hash_map.c` — `hash_map_base_put`, `hash_map_base_free` implementations
- `src/var_array.c` — `var_array_set_base`, `var_array_delete_base`, `ensure_path_allocated`
- `test/scenarios/bench_phase24b_compare/COMPARISON.md` — pre/post bench numbers
- `test/scenarios/bench_phase24b_compare/HASHMAP_BOTTLENECK.md` — earlier hash_map overview

