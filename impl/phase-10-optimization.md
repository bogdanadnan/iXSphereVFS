# Phase 10: Optimization & Hardening

## Goal
Profile-guided optimization, cache tuning, lock contention analysis, production hardening.

## Workloads

### 10.1 Profiling Setup
- Integrate `perf` (Linux) / `Instruments` (macOS) profiling
- Benchmark harness: `bench/bench.c` with configurable workloads
- Key metrics: ops/sec, µs/op, cache misses, lock contention

### 10.2 Hot Path Analysis
- Profile individual INSERT workload
- Identify top CPU consumers: CRC32C, cache lookup, pool CAS, lazy mirror write
- Optimize: inline critical functions, reduce branches, prefetch hints

### 10.3 Cache Tuning
- Experiment with cache sizes (64 MB, 128 MB, 256 MB, 512 MB)
- Measure hit rates at each size for OLTP workload
- Tune LRU eviction batch size
- Default: 256 MB, configurable via `vfs_config`

### 10.4 Lock Contention
- Profile per-epoch lock contention under 4/8/16 concurrent writers
- Arena-based pool allocation: measure CAS retry rate with and without arenas
- Tune arena count (1, 4, 8, 16) for target thread count

### 10.5 Allocation Hot Paths
- Inline `poolState` CAS in header
- Prefetch next pool page during freeCount=0
- Zero-fill optimization: `memset` vs SIMD vs OS zero-page

### 10.6 Crash Recovery
- Kill -9 during write, remount, verify consistency
- Kill during GC, verify old state intact
- Kill during mirror allocation, verify single-copy recovery
- Fuzzing: random bit flips in page data → verify CRC detection

### 10.7 Platform Matrix
- Test on: Linux x86_64, Linux aarch64, macOS x86_64, macOS aarch64
- CI pipeline: `cmake --build . && ctest`
- Valgrind/ASan: no leaks, no UAF, no buffer overflows

## Deliverables
- Benchmark suite with results documented
- Optimization commit log with before/after metrics
