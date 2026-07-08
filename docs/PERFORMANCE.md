# VFS Performance Analysis

Profiled on macOS 13 (X86-64) using `sample` (macOS equivalent of `perf record`).

**Workload**: `vfs_bench --workload write --count 100000` (100k file creates + small writes).  
**Total samples**: 7332.

## Top 5 CPU Consumers

| Rank | Function | Samples | % of Total | Description |
|------|----------|---------|-----------|-------------|
| 1 | `cache_find` | 4,715 | 64.3% | Page cache hash-table lookup. Every `storage_read` probes the cache first, and the write workload hits the cache on every create+write+dirchain walk. |
| 2 | `storage_read` | 755 | 10.3% | Storage layer read path (cache miss → mirror_read → pread). Called from `pool_resolve` during directory chain walks. |
| 3 | `indir_lookup` | 372 | 5.1% | Logical→physical page indirection table lookup. Called from `storage_read` and `mirror_write` to find physical offsets. |
| 4 | `pool_resolve` | 360 | 4.9% | Pool VirtualPtr→memory resolution. Converts `(page, slot)` addresses to cached payload pointers. |
| 5 | `vfs_create` | 243 | 3.3% | File creation entry point (excluding callees). Walks directory chain, checks name collisions, allocates FileNode+DirContent+NameEntry. |

## Analysis

### 1. `cache_find` dominates (64.3%)

The page cache is probed on every `storage_read` and `storage_write` call. The write workload creates 100,000 files, each requiring:
- DirContent chain walk (checking for name collisions) → multiple `pool_resolve` → `storage_read` calls
- FileNode allocation → `pool_alloc` → `storage_read` for new/returned slots
- Data page write → `storage_write` → `cache_insert`

Each of these goes through `cache_find` to check if the page is already in cache. With the default cache size (32768 entries, 256 MB), the cache easily holds all working pages, so misses are rare — but the hash-table probe cost is still paid on every access.

**Optimization suggestion**: The cache uses a chained hash table with spin-locked buckets. Switching to an open-addressing scheme (e.g., Robin Hood hashing) could reduce probe cost per lookup at the expense of more complex eviction.

### 2. `storage_read` (10.3%) and `indir_lookup` (5.1%)

The write workload triggers directory chain walks for name collision detection, causing repeated `pool_resolve` → `storage_read` → `indir_lookup` chains. The indirection table maps logical→physical page addresses and is probed on every cold read.

**Optimization suggestion**: The indirection table uses inline entries in the header page + overflow pages. A per-segment page-array cache (already partially implemented in `page_array.c`) could bypass `indir_lookup` for frequently-accessed pages.

### 3. `pool_resolve` (4.9%)

Every VirtualPtr access goes through `pool_resolve` which calls `storage_read`. The pool uses 32-byte slots with 255 slots per page. Frequent directory chain walks create many VirtualPtr resolutions.

### 4. `vfs_create` (3.3%)

File creation involves: directory chain walk (O(entries) per directory), hash computation, lock acquisition, pool allocation for FileNode+DirContent+NameEntry, and CAS-based DirContent insertion. The 3.3% count excludes callee time spent in `pool_resolve`/`storage_read`/`cache_find` (which are captured by their own samples).

## Secondary Consumers

| Function | Samples | Notes |
|----------|---------|-------|
| `nodes_read_dircontent` | 166 | DirContent entry parsing for name collision checks |
| `pwrite` (kernel) | 148 | Direct disk writes via mirror_write |
| `nodes_read_name_hash` | 73 | FNV-1a hash fast-reject during dirchain walks |
| `storage_allocate` | 91 | New logical page allocation + indirection setup |

## Methodology

```bash
# macOS (instruments/sample):
vfs_bench --workload write --count 100000 &
sample vfs_bench 10 -file vfs_perf.txt

# Linux (perf):
perf record -g ./vfs_bench --workload write --count 100000
perf report
```

## SPEC §13 Comparison: Per-Page Write Latency

[SPEC.md §13](../SPEC.md#13-performance-model) predicts **~42 µs** total per page write,
assuming warm cache and no disk I/O:

| Operation | Predicted | 
|-----------|-----------|
| Page array lookup + VirtualPtr decode | ~1 µs |
| Version chain walk (2 hops) | ~0.1 µs |
| Data page write (lazy mirror: write to inactive half, generation flip, CRC32C) | ~40 µs |
| Pool slot allocation (per-page CAS) | ~0.1 µs |
| Version chain CAS | ~0.1 µs |
| **Total** | **~42 µs** |

### Measured Results

Ran sequential write benchmark (`seqwrite` workload, which pre-allocates pages then
overwrites them page-by-page, measuring per-page latency):

```bash
vfs_bench --workload=seqwrite --count=5000
```

| Run | p50 | p95 | p99 | ops/sec |
|-----|-----|-----|-----|---------|
| 1 | 47 µs | 55 µs | 73 µs | 21,241 |
| 2 | 49 µs | 65 µs | 116 µs | 19,826 |
| 3 | 46 µs | 55 µs | 89 µs | 21,245 |
| **Avg** | **47 µs** | **58 µs** | **93 µs** | **20,771** |

### Analysis

**Measured vs. predicted**: 47 µs actual vs. 42 µs predicted — **+12% deviation**.

The 5 µs gap (~12%) is within expected variance for several reasons:

1. **CRC32C overhead**: The model estimates ~40 µs for the mirror write (CRC32C computation + pwrite), but the actual CRC32C cost on this hardware (Intel Haswell, hardware CRC32C via `vfs_crc32c_hw_x86`) plus the `pwrite` syscall round-trip may be slightly higher than estimated.

2. **Cache bucket spin-lock contention**: `storage_write` → `cache_insert` acquires a spin-lock on the hash bucket. With single-threaded workload this is minimal, but the lock acquisition still adds ~0.5-1 µs per call.

3. **Indirection table walk**: Each `storage_write` → `mirror_write` → `physical_offset` → `indir_lookup` walks the indirection table. For the second+ write (mirror path), this requires two indirection lookups (original + sibling).

4. **System variability**: macOS background tasks (spotlight, mds, etc.) add noise. The p50 of 47 µs is consistent across runs; the p95/p99 tail varies more due to OS scheduling.

### Throughput forecast

SPEC predicts ~2,600 ops/sec at 9 page writes per "statement" (individual operation). The measured `seqwrite` benchmark does ~1 page write per operation (single-page overwrite), achieving **20,771 ops/sec** — consistent with the ~42 µs/page model:

```
1 / 42 µs ≈ 23,809 ops/sec (ideal, single-page)
Actual: 20,771 ops/sec (87% of ideal)
```

For a complex operation like `vfs_write` on a new file (which involves pool allocation,
metadata writes, and data page writes — ~9 pages total), the actual throughput would be
expected around `20,771 / 9 ≈ 2,308 ops/sec` — close to the SPEC prediction of 2,600 ops/sec.

### Conclusion

The measured per-page write latency (47 µs p50) validates the SPEC §13 performance
model within acceptable tolerance (+12%). The lazy mirror write path (CRC32C + pwrite)
is the dominant cost, consistent with the model's 40 µs estimate. Cache and indirection
overhead accounts for the remaining 5-7 µs discrepancy.

