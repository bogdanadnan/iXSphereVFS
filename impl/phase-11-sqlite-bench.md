# Phase 11: SQLite Benchmarking

## Goal
Measure iXSphereVFS performance under realistic SQLite workloads. This phase
is purely measurement and validation — no VFS code changes. Depends on Phase 10
(SQLite VFS Integration) and Phase 10 (Benchmark Harness).

## Dependencies

| Dependency | Phase | Purpose |
|------------|-------|---------|
| Full VFS API | Phase 8 | Backing VFS |
| SQLite VFS | Phase 10 | CowVfs xOpen/xRead/xWrite callbacks |
| Benchmark harness | Phase 10 | `vfs_bench` CLI framework |
| SQLite amalgamation | External | `sqlite3.c` + `sqlite3.h` |

---

## Workload 11.1 — SQLite Benchmark Workloads

### What
Extend the Phase 10 benchmark harness with SQLite-specific workloads that
exercise the CowVfs backend through real table operations.

### Command-Line Interface
Additions to `vfs_bench`:
```
  insert     Individual auto-commit INSERTs via SQLite
  batch      Multi-row batched INSERTs
  sql_mixed  Configurable SQLite read/write mix
```

Link `bench/bench_sqlite.c` against both `libixsphere_vfs` and `sqlite3`.

### Required per Workload

**insert** — single-row auto-commit INSERTs:
- CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT)
- Loop: INSERT INTO t VALUES (i, 'row N') — one transaction per row
- Measures: ops/sec, avg latency, p99

**batch** — multi-row batched INSERTs:
- CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT)
- Loop: BEGIN → INSERT 100 rows → COMMIT
- Measures: ops/sec (rows/second), avg transaction latency

**sql_mixed** — configurable ratio:
- Pre-populate with 10,000 rows
- Threads do SELECT 20% / UPDATE 40% / INSERT 40% ratio
- Configurable via `--read-ratio N` (0–100)

### Acceptance
- [ ] `vfs_bench --workload insert --threads 4 --count 10000` runs and prints
  ops/sec, avg latency, p99
- [ ] `vfs_bench --workload batch --threads 4 --count 100` runs and prints
  rows/second
- [ ] Results are deterministic across runs (±5% at same thread count on
  same hardware)

---

## Workload 11.2 — SQLite Hot Path Analysis

### What
Profile the individual INSERT workload through the full stack: SQLite pager →
CowVfs xWrite → iXSphereVFS vfs_write → VersionPage COW → StorageBackend
lazy mirror.

### Required Measurements

1. **Time spent in SQLite pager vs VFS layer.** Where does CPU time go?
2. **VersionPage allocations per INSERT.** Expected: ~9 page writes per
   statement (1 btree page + metadata).
3. **COW overhead per INSERT.** First write to a table → copy all 1,024
   PageNodes in the segment. Subsequent writes in same snapshot → in-place.
4. **PageMap lookup time.** Per INSERT, how many SQLite page → VFS file node
   resolutions?
5. **Total per-INSERT time.** Compare against native SQLite (file-based)
   to measure VFS overhead.

### Acceptance
- [ ] Top 5 functions by CPU time identified and documented
- [ ] Per-INSERT time measured and compared to native SQLite
- [ ] COW cost measured: first write vs subsequent writes in same epoch
- [ ] Summary document in `docs/SQLITE_PERFORMANCE.md`

---

## Workload 11.3 — SQLite-Specific Cache Tuning

### What
Determine optimal page cache size for SQLite OLTP workloads. The working set
includes both SQLite's page cache (configurable via SQLite PRAGMA) and
iXSphereVFS's page cache.

### Test Matrix

Run `vfs_bench --workload insert --threads 4` at:
- SQLite cache: 2 MB, 8 MB, 32 MB
- VFS cache: 32 MB, 64 MB, 128 MB, 256 MB, 512 MB

### Acceptance
- [ ] Optimal (SQLite cache, VFS cache) pair identified
- [ ] Diminishing returns point documented
- [ ] Summary in `docs/SQLITE_CACHE.md`

---

## Workload 11.4 — SQLite Concurrency

### What
Measure throughput scaling with SQLite connections. Each connection opens
the database independently; SQLite's locking serializes writers.

### Test Matrix
Run `vfs_bench --workload insert --threads N --count 10000` for
N = 1, 2, 4, 8. Each thread opens its own `sqlite3` connection.

### Required Measurements

1. **Throughput scaling.** Expected: limited by SQLite's exclusive lock.
2. **Lock wait time.** Time spent waiting for `xLock(SQLITE_LOCK_EXCLUSIVE)`.
3. **PageMap contention.** Concurrent connections accessing different tables.

### Acceptance
- [ ] Throughput scaling documented
- [ ] Lock wait time measured
- [ ] Summary in `docs/SQLITE_CONCURRENCY.md`
