# Phase 2: StorageBackend

## Goal
Page allocation, free bitmap, lazy mirror, file I/O, unified page cache, flush ordering.

## Workloads

### 2.1 File Layout & XVFS Magic
- Create/open backing file with `O_CREAT | O_RDWR`
- Write header page 0 with magic (`pageType=0x5658, flags=0x5346`)
- Write header page 1 (bitmap_dir continuation)
- On open: validate magic + CRC32C on page 0
- `total_pages`, `page_size` fields (init default 8192)

### 2.2 Free-Page Bitmap
- Bit operations: set, clear, test, find next free (ffs/ctz)
- `bitmap_dir[]` array: append, read, write
- Zone-based allocation: 1M pages per zone, per-zone hint cursor
- `Allocate(count)` — find `count` contiguous free bits across zones
- `Acquire(page)` — CAS set specific bit
- `Free(page)` — clear bit, adjust cursor if before current hint
- Cap: `(2 * page_size - 32) / 8` entries in bitmap_dir
- Tests: alloc 1, alloc contiguous, acquire specific, free, zone wrap

### 2.3 Lazy Mirror Pages
- Physical layout: 16-byte PageHeader + page_size-byte payload
- Write: first write → gen=1, mirror=-1. Second write → allocate sibling → link
- Read: check mirrorPage. -1 → direct. != -1 → compare gens, pick higher
- Crash safety: sibling written first, then original header linked
- PageHeader CRC scope: payload only (mirrorPage not covered — documented risk)
- Tests: write-read, second write, crash simulation (kill process mid-write, verify recovery)

### 2.4 File I/O
- `Read(logicalPage)` → check cache → disk read → validate CRC → return payload
- `Write(logicalPage, data)` → write payload → update PageHeader → mark dirty
- `Flush(logicalPage)` — <0 → all pages + fsync, >=0 → single page no fsync
- Write-back: priority 0 (data pages) only, threshold-triggered
- Flush ordering: per-priority dirty lists, lowest first

### 2.5 Unified Page Cache
- Hash table: `logicalPage → cache_entry*`
- Dirty/clean state per entry
- LRU eviction (clean pages only)
- Configurable memory budget (default 256 MB)
- Thread-safe: per-bucket mutex or lock-free hash table
- `Flush()` writes dirty in priority order, marks clean, keeps resident

### 2.6 Bootstrap & Mount
- Create: reserve pages 0-3, init header, init bitmap page, VFS acquires superblock at 3
- Open: validate magic, load bitmap_dir, load bitmap pages, mount ready
- Superblock read from page 3 via `Read(3)`

## Deliverables
- `src/storage.c`, `src/bitmap.c`, `src/lazy_mirror.c`, `src/page_cache.c`
- Tests for alloc/free, mirror lifecycle, cache eviction, flush ordering
