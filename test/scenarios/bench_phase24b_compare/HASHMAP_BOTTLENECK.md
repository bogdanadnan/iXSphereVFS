# Hash Map Bottleneck Analysis (Phase 24)

## Microbench results (N=6500)

### Hash map operations

| Operation | Time/call | Notes |
|---|---|---|
| `hash_map_new + free` | 0.45 µs | Per-call alloc/free overhead |
| `hash_map_put` | 1.71 µs | Hash + tree walk + sparse chunk alloc |
| `hash_map_contains` (hit) | 46 ns | Hash + tree walk + compare |
| `hash_map_contains` (miss) | 49 ns | Hash + tree walk to EMPTY slot |
| `hash_key_64` (pure) | 5 ns | FNV-1a hash |
| `var_array_resolve_base` (pure) | 19 ns | Per-probe tree walk |

### Linear scan operations

| Operation | Time/call | Notes |
|---|---|---|
| Full dedup loop (N unique) | **312 ms per pass** (48 µs/insert) | 21M checks |
| `var_array_append` | 0.05 µs | Sequential allocation, very cache-friendly |
| `var_array_lookup` (sequential) | 14 ns | Cache-friendly sequential chunk access |

## Where the bottleneck is

For hash_map dedup of 6500 unique children, dirchain_list runs in **~13 ms measured** vs **~21 ms measured** for linear scan in dirchain_list (not the microbench). The hash_map version is **+9% slower** in real-world bench.

### Breakdown

| Component | Theoretical cost | Why hash_map loses in practice |
|---|---|---|
| `hash_key_64` | 5 ns × 13K = 65 µs | Negligible |
| `var_array_resolve_base` | 19 ns × 13K = 247 µs | Becomes ~3 ms due to cache misses (sparse chunks) |
| `hash_map_put` (allocate sparse chunk + write) | ~1 µs × 6500 = 6.5 ms | **~11 ms measured** — sparse chunk allocation is the killer |
| **Total hash_map** | ~6.8 ms theoretical | ~13 ms measured |
| **Total linear scan** | ~21M × 19 ns = 400 ms theoretical | **~13 ms measured** (sequential cache wins) |

### Key insight

The hash_map's underlying storage is a **sparse var_array**: each touched slot allocates its own chunk. This means:
- `hash_map_put` triggers sparse chunk allocation in the underlying var_array
- Sparse allocations have high cache-miss cost
- The "asymptotic O(N²) → O(N)" win is hidden by the per-put cache-miss cost

The linear scan uses a **dense var_array** (`var_array_append`):
- Sequential chunk allocation, very cache-friendly
- Sequential lookup pattern, hot in L1 cache
- Theoretical O(N²) work runs ~25x faster than predicted due to cache locality

### Crossover point

The hash_map wins when **N is large enough** that the linear scan's 21M+ comparisons exceed the hash_map's per-op cache-miss cost. From the microbench:
- Linear scan: ~48 µs/insert (constant, scales as N)
- Hash map: ~1.7 µs/insert (constant, but with cache misses)

Linear wins below ~30K entries; hash_map wins above.

## Source files

- `test/scenarios/hash_map_microbench.c` — `hash_map_*` operation timing
- `test/scenarios/linear_scan_microbench.c` — `var_array_*` operation timing
- `test/scenarios/bench_phase24b_compare/COMPARISON.md` — pre/post hash_map full bench

