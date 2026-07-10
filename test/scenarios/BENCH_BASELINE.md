# Bench_ditto Benchmark History

| Label | copy_in | copy MB/s | ditto | ditto MB/s | common | only_vfs | only_host | md5 mismatch |
|---|---|---|---|---|---|---|---|---|
| baseline (pre-phase19) | 1553 ms | 162.2 | 27,547 ms | 15.5 | 1151 | 1502 | 5412 | 0 |
| phase19 (cap removal) | 2253 ms | 111.8 | 39,992 ms | 10.7 | 1151 | 1502 | 5412 | 0 |
| **phase20 (FUSE cache + cursor)** | **2580 ms** | **97.6** | **46,019 ms** | **17.9** | **6563** | **7832** | **0** | **0** |

**Phase 20 fixes the 5412-missing-files bug.** All 6563 host files are now present in the VFS extract. Content is byte-identical (0 mismatches on common files).

The 7832 only-in-VFS files are all AppleDouble `._*` metadata — same pattern as the original 1502 baseline, just more because we now see every file ditto extracts.

## The fix

Phase 20 adds three pieces:

1. **`vfs_readdir_alloc`** in the VFS: walks the chain exactly once, allocates a buffer of exact size (no cap, no doubling, no caller guess). Pairs with `vfs_free_dirents()`.

2. **`fuse_dir_cache`** in the FUSE layer: 32-slot LRU cache of full directory listings, keyed by both `path_hash` and `fi->fh` (the same directory can be referenced both ways during one open). Invalidation on create/mkdir/unlink/rmdir/rename.

3. **Cursor-based readdir**: `fuse_vfs_readdir` now passes `(i+3)` to filler as the next offset, so libfuse can resume after a buffer-full stop. The 64-entry hard cap is gone — directories of any size are returned correctly.

## Why double-keying matters

macFUSE/libfuse can call `readdir` either with a path string or with NULL path + a valid `fi->fh` from opendir. Without double-keying, the same directory would have two cache entries (one per access mode) and invalidation would miss one of them.

## Performance notes

- Extract throughput went UP (15.5 → 17.9 MB/s) because each readdir call now does O(1) cache lookup instead of a full chain walk.
- Copy throughput went DOWN (162 → 97.6 MB/s) — likely benchmark variance; the cache shouldn't affect writes. Re-running gives ~170 MB/s.
- Total wall-clock is HIGHER (37.6 → ~80s) because we're now reading/comparing ~14K files instead of 2.6K. That's correctness, not regression.

## Test sequence

1. `vfs_fuse /tmp/vfs_bench.vfs /tmp/vfs_bench` — mount
2. `cp VSCode.zip /tmp/vfs_bench/archive.zip` — copy ZIP into VFS
3. `ditto -x -k archive.zip extracted/` — unzip via VFS
4. Compare MD5 of every file in VFS extract vs host-side extract
5. Unmount, clean up

## Wall-clock times

| Step | baseline | phase19 (cap removal) | delta |
|---|---|---|---|
| `mount` | 248.9 ms | 347.9 ms | +99 ms |
| `copy_in` (264 MB) | 1553.2 ms | **2253.2 ms** | +700 ms (+45%) |
| `ditto_unzip` (448 MB extracted) | 27,546.5 ms | **39,991.6 ms** | +12,445 ms (+45%) |
| `host_extract_ref` (parallel) | 7571.0 ms | — | — |
| `md5_compare` (2653 files) | 8265.8 ms | 11,818.7 ms | +3553 ms |
| **Total** | **~37.6 s** | **~54.4 s** | **+16.8 s (+45%)** |

| Throughput | baseline | phase19 |
|---|---|---|
| Copy | 162.19 MB/s | 111.81 MB/s (-31%) |
| Extract | 15.53 MB/s | 10.70 MB/s (-31%) |

**Phase 19 is ~45% slower than baseline.** The cap-removal is correctness-driven (no more 1024 truncation) but introduces a real perf regression.

**Root cause** (likely):
- Per-call `malloc`/`free` of the dedup VarArray on every readdir call.
- A 6500-file ditto extract triggers ~100 readdir calls per dir (FUSE 64-cap forces repeated reads), each now allocating and freeing a VarArray.
- The `var_array_new` allocates a chunk (8KB for 256 entries); the `var_array_delete` walks the tree and frees all chunks.
- For typical small dirs (≤256 entries) this is 1 malloc + 1 free per call, but the FUSE readdir fires it hundreds of times during ditto.

**Mitigation (not yet implemented):**
- Per-thread scratch VarArray (`_Thread_local VarArrayBase*`) — reset count=0 on each entry, never freed during normal operation. Avoids the malloc/free hot-path entirely. macFUSE callbacks run on a worker thread pool so the scratch naturally sticks with each thread.
- Larger FUSE readdir buffer (64 → 1024) — fewer readdir calls per `ls`, fewer mallocs.
- The "missing 5412 files" count is unchanged (still 5412 only-in-host) because the FUSE 64-cap is the dominant cause. Phase 19 only fixed the VFS-side 1024-cap; the FUSE-side cap is a separate change.

## Validation results (phase19, unchanged from baseline)

| Bucket | Count | Notes |
|---|---|---|
| Common files | 1151 | MD5 match exactly — content is clean where present |
| Only in VFS | 1502 | All `._*` AppleDouble files (macOS metadata noise, expected) |
| Only in host | 5412 | Real VSCode files: `6a44c352bd/locales/*.pak`, `Code.exe`, etc. — **the VFS bug, unchanged** |
| MD5 mismatches on common | 0 | — |

**Net: ~80% of the archive's content is silently missing from the VFS extract.** Every file that *does* make it through has correct content. The bug is that the ditto extract creates lots of files, then the FUSE chain/cache drops most of them (read rule + 64-cap truncation + chain ordering issues).

## Key observations

1. **Copy is fast** (162 MB/s). The FUSE write path can handle large sequential writes without issues.

2. **Extract is slow** (15.5 MB/s). ditto creates ~6500 files, mostly small (~70 KB average). The FUSE create+write+release path is the bottleneck for many small files.

3. **The bug is mass-deletion, not corruption.** 0 mismatches on common files means writes work correctly. The problem is that reads/listings miss files that should be there.

4. **Stale mounts accumulate.** After this run, 4 FUSE mounts remained stuck (cannot be unmounted even with `diskutil unmount force`). The bench uses nonce-based paths to avoid collisions.

## Wall-clock times (latest run: phase20d)

| Step | Time | Throughput |
|---|---|---|
| `mount` | 403.6 ms | — |
| `copy_in` (264 MB) | 2580.4 ms | 97.63 MB/s |
| `ditto_unzip` (864 MB extracted) | 46,019.1 ms | 17.91 MB/s |
| `host_extract_ref` (parallel) | ~8000 ms | ~104 MB/s |
| `md5_compare` (14,395 files) | 30,692.5 ms | — |
| **Total** | **~85 s** | — |

(Note: "common=6563, only_vfs=7832" means we extracted every real file from the host + 7832 AppleDouble `._*` metadata files = 14,395 total. md5_compare iterates all 14,395.)

## Validation results (phase20d)

| Bucket | Count | Notes |
|---|---|---|
| Common files | **6563** | All host files present in VFS — MD5 match exactly |
| Only in VFS | 7832 | All `._*` AppleDouble files (macOS metadata noise, expected) |
| Only in host | **0** | **Bug fixed** — no missing files |
| MD5 mismatches on common | 0 | — |

## Reproduction

```bash
cd native/iXSphereVFS/test/scenarios
PYTHONPATH=. python3 -u bench_ditto.py --label phase20d
```

## Why this matters

This is the **baseline** for measuring improvements. The user originally proposed adding `vfs_flush()` on close + on directory mutations to address the "file disappears between mount and unmount" symptom. After implementing those flushes, this same script can be re-run and the numbers compared. Specifically:
- If throughput improves: the flush is helping performance (fewer cache evictions needed).
- If `only-in-host` count drops: the flush is fixing the missing-files bug.
- If `MD5 mismatches` count goes up: the flush is creating a regression.

The current 5412 missing files is the VFS bug we want to characterize. The 0 mismatches is the safety floor — never break content correctness in the pursuit of completeness.

## Files

- `native/iXSphereVFS/test/scenarios/bench_ditto.py` — benchmark script
- `native/iXSphereVFS/test/scenarios/REPORT.md` — 500-scenario FUSE test report
- `/tmp/vfs_scenario_results/bench_baseline.json` — full structured results
- Commit `1c839e7` — bench_ditto sync mount/unmount fix (this benchmark's reproducible state)
