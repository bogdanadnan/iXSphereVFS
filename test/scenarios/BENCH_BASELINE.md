# Bench_ditto Baseline Benchmark (Pre-Flush-Fix)

**Date:** 2026-07-10
**Binary:** `native/iXSphereVFS/build/vfs_fuse` (Phase 18 + commit `1c839e7`)
**Host:** macOS, macFUSE 5.2.0
**Corpus:** `~/Downloads/VSCode-win32-x64-1.123.0.zip` (264 MB, ~6500 files extracted)

## Test sequence

1. `vfs_fuse /tmp/vfs_bench.vfs /tmp/vfs_bench` — mount
2. `cp VSCode.zip /tmp/vfs_bench/archive.zip` — copy ZIP into VFS
3. `ditto -x -k archive.zip extracted/` — unzip via VFS
4. Compare MD5 of every file in VFS extract vs host-side extract
5. Unmount, clean up

## Wall-clock times

| Step | Time | Throughput |
|---|---|---|
| `mount` | 248.9 ms | — |
| `copy_in` (264 MB) | **1553.2 ms** | **162.19 MB/s** |
| `ditto_unzip` (448 MB extracted) | **27,546.5 ms** | **15.53 MB/s** |
| `host_extract_ref` (parallel reference) | 7571.0 ms | 110 MB/s |
| `md5_compare` (2653 files) | 8265.8 ms | — |
| **Total** | **~37.6 s** | — |

## Validation results

| Bucket | Count | Notes |
|---|---|---|
| Common files (in both VFS and host extract) | 1151 | MD5 match exactly — content is clean where present |
| Only in VFS | 1502 | All `._*` AppleDouble files (macOS metadata noise, expected) |
| Only in host | 5412 | Real VSCode files: `6a44c352bd/locales/*.pak`, `Code.exe`, etc. — **the VFS bug** |
| MD5 mismatches on common | 0 | — |

**Net: ~80% of the archive's content is silently missing from the VFS extract.** Every file that *does* make it through has correct content. The bug is that the ditto extract creates lots of files, then the FUSE chain/cache drops most of them (read rule + 64-cap truncation + chain ordering issues).

## Key observations

1. **Copy is fast** (162 MB/s). The FUSE write path can handle large sequential writes without issues.

2. **Extract is slow** (15.5 MB/s). ditto creates ~6500 files, mostly small (~70 KB average). The FUSE create+write+release path is the bottleneck for many small files.

3. **The bug is mass-deletion, not corruption.** 0 mismatches on common files means writes work correctly. The problem is that reads/listings miss files that should be there.

4. **Stale mounts accumulate.** After this run, 4 FUSE mounts remained stuck (cannot be unmounted even with `diskutil unmount force`). The bench uses nonce-based paths to avoid collisions.

## Reproduction

```bash
cd native/iXSphereVFS/test/scenarios
PYTHONPATH=. python3 -u bench_ditto.py --label baseline
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
