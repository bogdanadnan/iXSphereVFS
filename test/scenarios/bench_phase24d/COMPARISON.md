# Phase 24d Bench: Hash_map dedup with new defaults

## Setup

This bench re-evaluates phase-24b (hash_map for dirchain_list dedup) after
phase-24c changed the hash_map defaults from `scale=20, granularity=8` to
`scale=16, granularity=9`.

The microbench in phase-24c showed:
- Old defaults: hash_map full cycle 10.6 ms (6500 unique keys)
- New defaults: hash_map full cycle 0.84 ms (12.6x faster)

So we now re-bench the end-to-end ditto extraction.

## Results (3 runs each)

### Hash_map dedup (phase-24b code, with phase-24c defaults scale=16, granularity=9)

| Run | copy_in | ditto_unzip | md5_compare | extract throughput |
|-----|---------|-------------|-------------|---------------------|
| 1   | 1261    | 21140       | 16168       | 39.0 MB/s           |
| 2   | 1263    | 20218       | 15145       | 40.8 MB/s           |
| 3   | 1291    | 20086       | 14020       | 41.0 MB/s           |
| **avg** | **1272** | **20481** | **15111** | **40.3 MB/s** |

### Linear scan dedup (no hash_map, original phase 24a code)

| Run | copy_in | ditto_unzip | md5_compare | extract throughput |
|-----|---------|-------------|-------------|---------------------|
| 1   | 1305    | 21890       | 18317       | 37.6 MB/s           |
| 2   | 1303    | 19919       | 16059       | 41.4 MB/s           |
| 3   | 1379    | 21818       | 16200       | 37.8 MB/s           |
| **avg** | **1329** | **21209** | **16859** | **38.9 MB/s** |

## Delta

| Metric | Hash_map | Linear | Hash_map delta |
|--------|----------|--------|----------------|
| extract | 20481ms | 21209ms | **-3.4%** (hash_map wins) |
| copy_in | 1272ms | 1329ms | **-4.3%** (hash_map wins) |
| md5_compare | 15111ms | 16859ms | **-10.4%** (hash_map wins) |

**Hash_map is now FASTER than linear scan across all metrics**, by 3-10%.

## Comparison to earlier phase 24b (with old defaults)

| Phase | Hash_map extract | Linear extract | Winner |
|---|---|---|---|
| phase-24b (old defaults, 3 runs) | 37382ms | 34232ms | **Linear +9.2%** |
| phase-24d (new defaults, 3 runs) | 20481ms | 21209ms | **Hash_map -3.4%** |

The phase-24c parameter tuning flipped the result:
- Before: hash_map was +9.2% slower (rejected)
- After: hash_map is -3.4% faster (viable)

## Why

The hash_map's microbench with new defaults:
- 25x faster put (sparse chunk allocation reduced by 28x fewer nodes)
- 2x faster lookup (load factor 16% vs 1%)
- 12.6x faster full cycle

That translates to end-to-end wins because the dirchain_list dedup loop
runs ~6500 times per VSCode-extract dir, dominating the per-readdir cost.

## Validation

- All 10 unit test suites pass (test_tree 966/966, test_hash_map 45917/45917, etc.)
- Validation: common=6563, only_vfs=7832 (AppleDouble), only_host=0, mismatches=0
- Same correctness as before; the only change is performance.

## Files
- `bench_phase24d_posthash_run{1,2,3}.json` — hash_map dedup (with phase-24c defaults)
- `bench_phase24d_prehash_run{1,2,3}.json` — linear scan dedup
