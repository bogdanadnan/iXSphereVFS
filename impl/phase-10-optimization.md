# Phase 10: Optimization & Hardening

## Goal
Profile the VFS under realistic workloads, tune the page cache and allocator
for throughput, analyze lock contention, harden crash recovery, and validate
correctness across the full platform matrix. This phase is about measurement,
not feature development.

---

## Workload 10.1 — Benchmark Harness & Profiling Setup

**What:** Build a configurable benchmark tool and integrate platform profilers
to measure throughput, latency, and cache behavior.

**Why:** Optimization without measurement is guesswork. A repeatable benchmark
with clear metrics (operations/second, microseconds/operation, cache hit rate,
lock contention percentage) identifies the real bottlenecks.

**How:**
- Create `bench/bench.c`: a command-line tool that accepts parameters for
  workload type, thread count, operation count, and page size.
- Supported workloads:
  - **Individual INSERTs:** single-row auto-commit INSERTs via SQLite with the
    CowVfs backend. This is the primary benchmark from the original Phase 13b
    performance investigation.
  - **Batched INSERTs:** multi-row transactions. Measures metadata amortization.
  - **Random reads:** read random pages from a pre-populated file.
  - **Sequential scan:** read all pages of a file in order.
  - **Mixed read/write:** configurable ratio of reads to writes.
  - **Directory operations:** create, list, delete cycles.
- Output metrics per run: total time, operations per second, average latency,
  min/max latency, percentiles (p50, p95, p99).
- Integrate platform profilers:
  - Linux: `perf record` / `perf report` for CPU sampling and cache misses.
  - macOS: `Instruments` (Time Profiler template) or `sample` command.
- The benchmark harness is built as a separate executable (`bench/vfs_bench`)
  linked against the VFS library and SQLite.

**Acceptance:**
  - `vfs_bench --workload insert --threads 4 --count 10000` runs and prints
    ops/sec and latency distribution.
  - Profiler output identifies the top 5 functions by CPU time.
  - Cache miss rate is measurable and reported.

---

## Workload 10.2 — Hot Path Analysis

**What:** Profile the individual INSERT workload (the original bottleneck from
Phase 13b) and identify the top CPU consumers.

**Why:** Understanding where time is spent in the hot path determines which
optimizations will have the greatest impact. The spec predicts ~42 µs per
page write; measurement validates or refutes this.

**How:**
- Run the individual INSERT benchmark from Workload 10.1 with 4 threads and
  2,000 rows per thread.
- Collect a CPU profile using `perf` or `Instruments`.
- The expected hot path: version chain walk → CAS on `poolState` → CRC32C →
  `Write` to StorageBackend (lazy mirror) → CAS on `versionRootPtr`.
- Specific items to measure:
  - CRC32C time per 8KB page (should be ~2 µs with hardware, ~250 µs without).
  - Pool slot allocation time (CAS retry rate, average attempts).
  - Page cache lookup time (should be O(1) with hash table).
  - Version chain walk depth (should average ~2 entries for live-head reads).
- Compare measured per-page-write time against the ~42 µs prediction from
  the performance model.

**Acceptance:**
  - Profile output maps at least 80% of CPU time to known functions.
  - CRC32C time is within 2× of the predicted hardware-accelerated value.
  - Pool CAS retry rate is measured (expected: <5% under 4 threads).
  - A summary document lists the top 5 bottlenecks with measured times.

---

## Workload 10.3 — Cache Tuning

**What:** Experiment with page cache sizes and eviction policies to find the
optimal configuration for OLTP workloads.

**Why:** The page cache absorbs repeated reads of hot metadata pages (pool
pages, directory nodes). Too small a cache causes thrashing; too large wastes
memory.

**How:**
- Run the individual INSERT benchmark at cache sizes: 32 MB, 64 MB, 128 MB,
  256 MB (default), 512 MB, 1 GB.
- Measure cache hit rate at each size. The hit rate should plateau when the
  working set fits entirely in the cache.
- Measure the LRU eviction rate (pages evicted per second). High eviction at
  small cache sizes correlates with performance degradation.
- Tune the LRU eviction batch size (currently unspecified). Larger batches
  amortize the eviction overhead but may delay allocation.
- If hit rates are low even at large cache sizes, investigate cache pollution
  from sequential scans (e.g., a full table scan should not evict hot metadata
  pages). Consider a two-queue or LRU-K policy if simple LRU is insufficient.
- Default configuration: 256 MB, LRU eviction, single-queue. Update only if
  measurement shows a clear win from a different policy.

**Acceptance:**
  - Cache hit rate is ≥95% at the default 256 MB for the individual INSERT
    workload.
  - Throughput at 128 MB is within 10% of throughput at 512 MB (diminishing
    returns).
  - Document the recommended cache size for OLTP workloads.

---

## Workload 10.4 — Lock Contention Analysis

**What:** Measure contention on per-epoch file locks and pool `poolState` CAS
under increasing thread counts.

**Why:** Lock contention limits scalability. The spec claims per-epoch locks
allow cross-epoch concurrency and pool CAS retries are rare. Measurement
validates these claims.

**How:**
- Run the individual INSERT benchmark at thread counts: 1, 2, 4, 8, 16.
- Measure:
  - Per-epoch lock wait time (average and p99).
  - Global lock wait time (when SQLite global locking is active).
  - Pool `poolState` CAS retry rate (per allocation attempt).
  - `poolListHead` CAS retry rate (per new page creation).
  - `versionRootPtr` CAS retry rate (per version chain prepend).
- Expected results:
  - Per-epoch lock wait should be near zero for different-file workloads.
  - Pool CAS retry rate should be <10% at 8 threads, <20% at 16 threads.
  - `versionRootPtr` CAS retries should be zero under per-file locking (only
    one writer per file per epoch).
- If pool CAS retry rate exceeds 30%: consider enabling arena-based allocation
  (already spec'd as optional in Phase 3). Measure the improvement.
- If `poolListHead` contention is high (many new pages being created): consider
  a larger initial pool page pre-allocation or batch allocation.

**Acceptance:**
  - Throughput scales roughly linearly from 1 to 4 threads (within 20% of
    linear).
  - Pool CAS retry rate documented for each thread count.
  - Arena-based allocation, if enabled, reduces pool CAS retry rate by at
    least 50% at 8+ threads.
  - Lock wait times are negligible (<1% of total operation time) for
    different-file workloads.

---

## Workload 10.5 — Crash Recovery Testing

**What:** Systematically test crash recovery by killing the process at various
points during writes and GC, then remounting and verifying data integrity.

**Why:** The VFS claims crash safety via lazy mirror and shadow-compaction GC.
This must be empirically validated, not just reasoned about.

**How:**
- Test harness: `test/test_crash.c`. Spawns a child process that performs VFS
  operations, then kills it with `SIGKILL` at a random or controlled point.
  The parent remounts the backing file and verifies consistency.
- Crash scenarios:
  1. **Mid-write to single-copy page:** write 8KB, kill before `mirrorPage`
     link. Remount: page is either intact (old data) or lost (CRC failure →
     NULL). No corruption of other pages.
  2. **Mid-write to mirrored page:** write to a page that already has a mirror.
     Kill mid-write. Remount: data is either old or new, never torn.
  3. **During GC (before swap):** kill after GC has written new pool pages
     but before superblock swap. Remount: old tree intact, no data loss.
  4. **During GC (after swap, before deferred-free):** kill after swap but
     before old pages freed. Remount: new tree active, old pages in deferred-
     free queue or reclaimed.
  5. **During snapshot (epoch increment):** kill before any write at new epoch.
     Remount: old epoch, no corruption.
  6. **During commit:** kill after TouchedFile scan but before mapper write.
     Remount: snapshot still uncommitted, data intact.
- For each scenario, run at least 100 iterations to catch low-probability
  races.
- Verify: no double-allocated pages, no dangling VirtualPtrs, no corrupted
  file data. The verification reads every file in the tree and compares
  against expected content.

**Acceptance:**
  - All crash scenarios pass 100 iterations without data corruption.
  - Mid-write crashes never corrupt pages other than the one being written.
  - GC crashes never leave the tree in an inconsistent state.
  - Documentation of any scenarios where data loss is possible (single-copy
    page write is the only known case).

---

## Workload 10.6 — Fuzzing

**What:** Inject random bit flips into page data and verify that CRC32C
detection catches the corruption and the system degrades gracefully.

**Why:** Silent data corruption from storage media (bit rot, SSD errors) must
be detected, not silently propagated. The CRC32C on every page is the primary
defense.

**How:**
- Test harness: read a pool page or data page from the backing file, flip
  one or more bits in the payload, write it back, then attempt to read via
  the VFS. Verify:
  - The corrupt page is detected (CRC32C mismatch).
  - For data pages: the lazy mirror fallback returns the other half.
  - For pool pages: the entry is treated as unallocated (chain terminates).
- Test corner cases:
  - Corrupt the PageHeader itself (generation field, mirrorPage field,
    checksum field). Verify the page is rejected.
  - Corrupt a VirtualPtr in a pool entry (point to a non-existent page).
    Verify the chain terminates or the reference is treated as null.
  - Corrupt the superblock. Verify mount detects it and falls back to the
    alternate half.
- Run fuzzing for at least 10,000 random corruptions across different page
  types and offsets.

**Acceptance:**
  - All corruptions are detected (no silent data corruption passes through).
  - The VFS never crashes on corrupted input (graceful degradation).
  - Lazy mirror fallback works for data page corruption.
  - Superblock fallback works for superblock corruption.

---

## Workload 10.7 — Platform Matrix & CI

**What:** Ensure the VFS compiles and passes all tests on every supported
platform. Set up continuous integration.

**Why:** The VFS uses platform-specific intrinsics (SSE4.2, ARMv8 CRC32) and
atomics. It must work correctly on Linux x86_64, Linux aarch64, macOS x86_64,
macOS aarch64, and Windows x86_64.

**How:**
- CI pipeline (GitHub Actions or similar): on each push, build and test on
  all five platforms.
- Build matrix: `cmake -DCMAKE_BUILD_TYPE=Release .. && cmake --build .`
- Test matrix: `ctest --output-on-failure`
- Sanitizer builds (optional, slower): add `-fsanitize=address,undefined` to
  debug builds on Linux. Run the test suite under AddressSanitizer and
  UndefinedBehaviorSanitizer. No leaks, no buffer overflows, no use-after-free.
- Valgrind (optional, Linux): `valgrind --leak-check=full ./vfs_test`. No
  memory leaks.
- Performance is NOT tested in CI (hardware-dependent). Only correctness.

**Acceptance:**
  - All tests pass on all five platforms.
  - ASan/UBSan clean on Linux x86_64.
  - Valgrind reports no leaks.
  - CI badge in the repository README shows passing status.

---

## Workload 10.8 — Documentation

**What:** Write a user-facing README and API reference.

**Why:** The spec and implementation phases are internal. External consumers
need a concise document describing how to build, link, and use the library.

**How:**
- `README.md`: project overview, build instructions (`cmake .. && make`),
  quick-start example (open, create file, write, read, close), link to the
  full API reference.
- `docs/API.md`: one section per API function, describing parameters, return
  values, error codes, and thread safety. Generated from the public header
  comments or written manually.
- Document the epoch model: what epochs are, how snapshots work, how commit
  and soft-delete behave, and when to call GC.
- Document platform support and known limitations (single-copy write risk,
  maximum file size, manual GC requirement).

**Acceptance:**
  - A developer unfamiliar with the VFS can build and run the quick-start
    example in under 5 minutes.
  - API reference covers every public function.
  - Platform support table lists all five platforms with build status.

---

## Deliverables

| File | Purpose |
|------|---------|
| `bench/bench.c` | Benchmark harness with configurable workloads |
| `test/test_crash.c` | Crash recovery test harness (fork/kill/verify) |
| `test/test_fuzz.c` | Fuzzing harness for corruption detection |
| `docs/API.md` | API reference documentation |
| `README.md` | Updated with build instructions and quick-start |

## Success Criteria
- Benchmark tool produces repeatable, documented results.
- Hot path analysis identifies top CPU consumers matching the performance model.
- Cache hit rate ≥95% at default 256 MB for OLTP workload.
- Throughput scales to 4 threads with minimal lock contention.
- Crash recovery passes 100 iterations per scenario without corruption.
- Fuzzing detects 100% of injected corruptions.
- CI passes on all five platforms.
- Sanitizers and Valgrind report no errors.
