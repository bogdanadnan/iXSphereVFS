# Phase 10: Optimization & Hardening

## Goal
Profile the VFS under realistic workloads, tune the page cache and allocator
for throughput, analyze lock contention, harden crash recovery, and validate
correctness across the full platform matrix. This phase is about measurement
and validation — no new features.

## Non-Negotiable Constraints

- **Every optimization must be backed by measurement.** Before and after
  numbers for throughput, latency, and contention. No "seems faster."
- **Crash recovery must be empirically validated.** Not reasoned about.
  100 iterations per scenario, zero corruption.
- **CI must pass on all five platforms.** Linux x86_64, Linux aarch64,
  macOS x86_64, macOS aarch64, Windows x86_64.
- **Sanitizers must be clean.** AddressSanitizer + UndefinedBehaviorSanitizer
  on Linux. No leaks, no buffer overflows, no use-after-free.

## File Organization

| File | Purpose |
|------|---------|
| `bench/bench.c` | Benchmark harness |
| `test/test_crash.c` | Crash recovery test harness |
| `test/test_fuzz.c` | Fuzzing harness |
| `docs/API.md` | API reference |
| `README.md` | Build instructions + quick-start |

---

## Workload 10.1 — Benchmark Harness

### What
A CLI tool `vfs_bench` that measures throughput and latency for configurable
workloads.

### Command-Line Interface
```
vfs_bench --workload <name> [options]

Workloads:
  insert     Individual auto-commit INSERTs via SQLite
  batch      Multi-row batched INSERTs
  read       Random reads from pre-populated file
  scan       Sequential full-file scan
  mixed      Configurable read/write mix
  dir        Directory create/list/delete cycles

Options:
  --threads N       Thread count (default 1)
  --count N         Operations per thread (default 10000)
  --page-size N     Page size in bytes (default 8192)
  --cache-mb N      Page cache size in MB (default 256)
  --output FILE     Write CSV results to FILE
```

### Required Output Metrics
- Total wall time (seconds)
- Operations per second
- Average latency (microseconds)
- p50, p95, p99 latency
- Cache hit rate (for read-heavy workloads)
- If multiple threads: per-thread breakdown

### Structure
```c
int main(int argc, char** argv) {
    parse_args();
    vfs_t* vfs = vfs_open("bench.vfs");
    // Setup: create tables, pre-populate data
    // Run benchmark
    start_timer();
    for (i = 0; i < thread_count; i++)
        spawn_thread(worker, workload, ops_per_thread);
    wait_all_threads();
    stop_timer();
    print_results();
    vfs_close(vfs);
}
```

### Acceptance
- [ ] `vfs_bench --workload insert --threads 4 --count 10000` runs and prints
  ops/sec, avg latency, p99
- [ ] Results are deterministic across runs (±5% at same thread count on
  same hardware)
- [ ] Profiler integration: running under `perf record` produces usable callgraphs

---

## Workload 10.2 — Hot Path Analysis

### What
Profile the individual INSERT workload and identify top CPU consumers.
Compare measured per-page-write time against the predicted ~42 µs.

### Required Measurements

1. **CRC32C time per 8KB page.** Expected: ~2 µs hardware, ~250 µs software.
   If software path is active on hardware that supports CRC32C → bug.
2. **Pool `pool_alloc` time.** Expected: dominated by one CAS (5–20ns) plus
   slot initialization (100–200ns). Total ~0.3 µs.
3. **Lazy mirror `storage_write` time.** Expected: ~40 µs (memcpy 8KB +
   CRC32C + PageHeader update).
4. **Version chain walk depth.** Expected: average 2 entries for live-head
   reads, 1 entry for reads at the current epoch.
5. **Page cache lookup time.** Expected: O(1) hash table, ~50ns.
6. **Total per-page-write time.** Sum of: cache lookup + version chain walk
   + pool alloc + data page write + VersionPage CAS.

### What to Fix (Based on Measurements)

| Issue | Fix |
|-------|-----|
| CRC32C > 10 µs on x86_64 | Hardware path not active — check `#if` guards |
| pool_alloc > 2 µs | CAS contention too high — check retry rate, consider arenas |
| storage_write > 100 µs | Disk I/O during write — cache miss, check flush behavior |
| Version chain > 10 entries | Too many versions per page — need GC more frequently |
| Cache hit rate < 80% | Cache too small for working set — increase budget |

### Acceptance
- [ ] Top 5 functions by CPU time identified and documented
- [ ] Per-page-write time measured and compared to model
- [ ] Any function exceeding 2× its predicted time has an explanation or fix
- [ ] Summary document in `docs/PERFORMANCE.md`

---

## Workload 10.3 — Cache Tuning

### What
Find the optimal page cache size for OLTP workloads.

### Test Matrix
Run `vfs_bench --workload insert --threads 4 --count 10000` at each
cache size:
- 32 MB (4,096 pages)
- 64 MB (8,192 pages)
- 128 MB (16,384 pages)
- 256 MB (32,768 pages) ← default
- 512 MB (65,536 pages)
- 1 GB (131,072 pages)

### Required Measurements per Size
- Cache hit rate (%)
- Operations/second
- Average latency
- LRU eviction rate (pages evicted per second)

### Expected Results
- Hit rate should reach ≥95% at some size and plateau
- Throughput should plateau within 10% of the peak
- Eviction rate should drop as cache grows

### What to Document
- Recommended default size with justification
- Memory-constrained minimum (where throughput drops <10% from peak)
- Whether LRU is sufficient or a 2-queue/LRU-K policy is needed

### Acceptance
- [ ] Cache hit rate ≥95% at default 256 MB for INSERT workload
- [ ] Peak throughput identified (the cache size after which more memory
  doesn't help)
- [ ] Document with charts in `docs/CACHE.md`

---

## Workload 10.4 — Lock Contention Analysis

### What
Measure contention under increasing thread counts.

### Test Matrix
Run `vfs_bench --workload insert --threads N --count 10000` for
N = 1, 2, 4, 8, 16.

### Required Measurements per Thread Count

1. **Throughput scaling.** Ideal: linear (N× throughput at N threads).
   Realistic: 0.7×–0.9× per doubling.
2. **Per-epoch lock wait time.** Average and p99. Should be near zero for
   different-file workloads.
3. **Pool `poolState` CAS retry rate.** Percentage of CAS calls that fail
   (retry). Expected: <5% at 4 threads, <10% at 8, <20% at 16.
4. **`poolListHead` CAS retry rate.** Expected: near zero (only fires once
   per 255 allocations).
5. **`versionRootPtr` CAS retry rate.** Expected: zero under per-file locking.

### What to Fix

| Issue | Fix |
|-------|-----|
| poolState CAS retry > 20% at 8 threads | Enable arena allocation (Phase 3 optional) |
| Throughput doesn't scale past 4 threads | Identify bottleneck (lock, CAS, cache, disk) |
| Lock wait time significant | Check global lock not held unnecessarily |

### Acceptance
- [ ] Throughput scaling documented for 1–16 threads
- [ ] CAS retry rates documented
- [ ] Arena allocation enabled if retry rate exceeds 30%, with before/after comparison
- [ ] Summary in `docs/CONTENTION.md`

---

## Workload 10.5 — Crash Recovery Testing

### What
Systematically `kill -9` the process at various points and verify data
integrity on remount.

### Test Harness: `test/test_crash.c`

```
for each scenario:
    for iteration = 1..100:
        child = fork()
        if child == 0:
            // CHILD PROCESS
            vfs_open("crash.vfs")
            perform scenario-specific operations
            // NO explicit flush (except where scenario requires it)
            // PARENT kills at random or controlled point:
            usleep(random_delay)
            // child exits or is killed
        else:
            // PARENT PROCESS
            wait for child to finish or kill it
            vfs_open("crash.vfs")  // remount
            validate_tree()         // walk all files, verify content
            vfs_close()
```

### Crash Scenarios (All 100 iterations Each)

1. **Mid-write, single-copy page.** Write to a page that has never been
   written before. Kill before the PageHeader is written. Remount: page
   is NULL (never written) or intact (old data). Never torn.
2. **Mid-write, mirrored page.** Write to a page that already has a mirror.
   Kill at random offset within the write. Remount: page is either old or
   new. Never torn — CRC validates on one half.
3. **During mirror allocation.** Write to a page for the second time. Kill
   between sibling write and original's `mirrorPage` update. Remount:
   original intact (gen=1, mirror=-1). Sibling is zombie.
4. **During GC, before swap.** Kill after GC has written new pool pages but
   before superblock swap. Remount: old tree intact.
5. **During GC, after swap, before deferred-free.** Kill after swap but
   before old pages freed. Remount: new tree active.
6. **During snapshot.** Kill after `currentEpoch += 2` but before any write
   at new epoch. Remount: old epoch, no corruption.
7. **During commit.** Kill after TouchedFile scan but before mapper write.
   Remount: snapshot still uncommitted, data intact.
8. **Power loss during flush.** Kill during `storage_flush(-1)`. Remount:
   superblock may be old or new. Lazy mirror ensures whichever superblock
   is active points to a consistent tree.

### Validation Function

```c
void validate_tree(vfs_t* vfs) {
    // Walk entire tree from root
    // For each file: read every page, verify CRC, verify content matches
    // expected (pre-recorded before the crash test)
    // Report any mismatches
}
```

### Acceptance
- [ ] All 8 scenarios × 100 iterations = 800 crash tests pass
- [ ] No data corruption in any scenario
- [ ] Single-copy write loss is the ONLY acceptable data loss
- [ ] Document any scenarios where data loss occurred (even if acceptable)

---

## Workload 10.6 — Fuzzing

### What
Inject random bit flips into the backing file and verify the VFS detects
all corruption and degrades gracefully (no crashes, no panics, no assertion
failures).

### Test Harness: `test/test_fuzz.c`

```
for iteration = 1..10000:
    vfs_open("fuzz.vfs")
    // populate with known data
    create files, write data
    vfs_close()

    // Corrupt the backing file
    fd = open("fuzz.vfs", O_RDWR)
    // pick random page, random offset, flip 1-8 random bits
    offset = random_page_offset()
    byte = random_byte()
    bitmask = random_bitmask()
    pread(fd, &original, 1, offset)
    pwrite(fd, &(original ^ bitmask), 1, offset)
    close(fd)

    // Remount and verify
    vfs_open("fuzz.vfs")
    walk tree, read all files
    // Expect: some reads fail (CRC mismatch), but NO crashes, NO hangs
    vfs_close()
```

### Expected Behaviors Under Corruption

| Corruption Location | Expected Behavior |
|---------------------|-------------------|
| PageHeader checksum | Page rejected (CRC mismatch) |
| PageHeader generation | Page rejected or alternate half used |
| PageHeader mirrorPage | Lazy mirror fallback fails → page lost |
| Data page payload | CRC mismatch → fallback to mirror half |
| Pool page payload | Entry treated as unallocated → chain terminates |
| VirtualPtr in chain | Points to garbage → treated as null (0) |
| Superblock | Mount detects → falls back to alternate superblock half |
| StorageBackend header | Mount rejects → file not a valid VFS |

### Acceptance
- [ ] 10,000 corruptions, zero crashes
- [ ] All corruptions detected (no silent data corruption)
- [ ] Lazy mirror fallback works for data page corruption
- [ ] Superblock fallback works for superblock corruption
- [ ] Summary in `docs/FUZZING.md`

---

## Workload 10.7 — Platform Matrix & CI

### What
Continuous integration that builds and tests on all 5 platforms.

### GitHub Actions Workflow (`.github/workflows/ci.yml`)
```yaml
name: CI
on: [push, pull_request]
jobs:
  build:
    strategy:
      matrix:
        os: [ubuntu-latest, macos-latest, windows-latest]
        arch: [x86_64]
        include:
          - os: ubuntu-latest
            arch: aarch64
          - os: macos-latest
            arch: aarch64
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/checkout@v4
      - name: Configure
        run: cmake -B build -DCMAKE_BUILD_TYPE=Release
      - name: Build
        run: cmake --build build
      - name: Test
        run: cd build && ctest --output-on-failure
```

### Sanitizer Build (Linux only, manual trigger)
```yaml
  sanitize:
    runs-on: ubuntu-latest
    steps:
      - name: Configure with sanitizers
        run: cmake -B build -DCMAKE_BUILD_TYPE=Debug
             -DCMAKE_C_FLAGS="-fsanitize=address,undefined"
      - name: Build & Test
        run: cmake --build build && cd build && ctest
```

### Valgrind (Linux only, manual trigger)
```bash
valgrind --leak-check=full --track-origins=yes ./vfs_test
```

### Acceptance
- [ ] All tests pass on all 5 platforms in CI
- [ ] ASan/UBSan clean (zero errors)
- [ ] Valgrind reports no leaks, no use-after-free, no uninitialized reads
- [ ] CI badge in README shows passing

---

## Workload 10.8 — Documentation

### What
User-facing `README.md` and `docs/API.md`.

### README.md Sections Required
1. What is iXSphereVFS (2-3 sentences)
2. Quick-start: build + example code (open, create file, write, read, close)
3. Build requirements (CMake 3.16+, C11 compiler, pthreads)
4. Platform support table
5. Link to full API reference
6. CI badge

### docs/API.md Sections Required
- Instance management: `vfs_open`, `vfs_close`, `vfs_flush`
- File operations: `vfs_create`, `vfs_open_file`, `vfs_read`, `vfs_write`,
  `vfs_delete`, `vfs_file_size`, `vfs_file_mtime`, `vfs_file_ctime`
- Directory operations: `vfs_mkdir`, `vfs_rmdir`, `vfs_readdir`, `vfs_rename`
- Locking: `vfs_lock`, `vfs_unlock` with locking rules
- Snapshot & commit: `vfs_snapshot`, `vfs_commit`, `vfs_delete_snapshot`
- GC: `vfs_gc`
- Error handling: `vfs_last_error`, `vfs_error_string`, error code table
- Thread safety notes
- Known limitations: single-copy write risk, manual GC, max file size

### Acceptance
- [ ] New developer can build and run quick-start in under 5 minutes
- [ ] API reference covers every public function
- [ ] Platform table lists all 5 platforms with build status
- [ ] Known limitations documented

---

## Final Phase 10 Checklist

- [ ] Benchmark produces repeatable results; hot path analyzed and documented
- [ ] Cache hit rate ≥95% at default 256 MB
- [ ] Throughput scales to 4 threads within 20% of linear
- [ ] 800 crash tests (8 scenarios × 100) pass without corruption
- [ ] 10,000 fuzz iterations detect 100% of corruptions, zero crashes
- [ ] CI green on all 5 platforms
- [ ] Sanitizers and Valgrind clean
- [ ] Documentation complete (README + API reference + performance/cache/contention/fuzzing reports)
