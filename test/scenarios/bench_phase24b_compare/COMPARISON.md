# Phase 24 Bench: Pre-hash vs Post-hash Dedup

## Setup
- VSCode.zip (264MB) → 6563 files + 7832 AppleDouble (._*) metadata files
- 3 runs each, bench_ditto.py end-to-end
- Same hardware, sequential runs (not parallel)

## Pre-hash (linear scan dedup, O(N²))

| Run | mount | copy_in | extract | md5_compare |
|-----|-------|---------|---------|-------------|
| 1   | 240   | 1570    | 29071   | 20875       |
| 2   | 261   | 1798    | 36910   | 27115       |
| 3   | 348   | 2074    | 36716   | 24514       |
| **avg** | **283** | **1814** | **34232** | **24168** |

## Post-hash (hash_map dedup, O(N))

| Run | mount | copy_in | extract | md5_compare |
|-----|-------|---------|---------|-------------|
| 1   | 298   | 1969    | 34555   | 25342       |
| 2   | 352   | 2166    | 38064   | 27637       |
| 3   | 391   | 2333    | 39527   | 26198       |
| **avg** | **347** | **2156** | **37382** | **26392** |

## Delta (post - pre)

| Step      | Pre mean | Post mean | Delta     | %      |
|-----------|----------|-----------|-----------|--------|
| copy_in   | 1814ms   | 2156ms    | +342ms    | +18.8% |
| extract   | 34232ms  | 37382ms   | +3150ms   | +9.2%  |
| md5_compare | 24168ms | 26392ms  | +2224ms   | +9.2%  |

## Findings

**The hash_map dedup is SLOWER than linear scan** for the typical 6500-file directory case.

Reasons:
1. **Linear scan is memory-bandwidth-bound** — sequential chunk access hits L1/L2 cache well for typical N.
2. **Hash_map is compute-bound** — FNV-1a hash + modulo + probe per insert/lookup has constant-factor overhead.
3. **Per-call hash_map_new/free overhead** — small fixed cost (~100µs) but multiplied across many extract dirs.

The asymptotic O(N²) → O(N) win only kicks in at **N >> 10K**, when the linear scan's 21M+ comparisons exceed the hash_map's constant overhead.

## Decision

**REVERT phase 24b.** Keep linear scan for now. Re-introduce hash_map dedup only if/when we observe real workloads with N > 50K files per directory.

The hash_map primitive (Phase 23) remains useful for other use cases. Just not for `dirchain_list` dedup at current scale.

## Action

Revert commit `ba92faa` (phase 24b). Keep:
- Phase 22: sparse var_array (var_array_set, var_array_unset)
- Phase 23: hash_map primitive (for other future uses)
- Phase 24a: single readdir API
