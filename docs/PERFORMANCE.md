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
| `nodes_read_touchedfile` | 183 | TouchedFile chain scan during writes |
| `nodes_read_dircontent` | 166 | DirContent entry parsing for name collision checks |
| `pwrite` (kernel) | 148 | Direct disk writes via mirror_write |
| `touchedfile_add` | 107 | Tracking modified files for snapshot/commit |
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
