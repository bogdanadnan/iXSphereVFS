# 500-Scenario FUSE Test Report

**Date:** 2026-07-10
**Test subject:** `iXSphereVFS` mounted via FUSE (macFUSE 5.2.0) on macOS
**Binary:** `native/iXSphereVFS/build/vfs_fuse` (Phase 18 directory radix tree, commit `25f685d`)
**Corpus:** `~/Downloads/*` — small PDFs, large DMG, ZIP archives, images, documents

## Summary

| Metric | Value |
|---|---|
| Total scenarios run | **500** |
| Passed | **428 (85.6%)** |
| Failed | **72 (14.4%)** |

### Per-batch results

| Batch | Topic | Pass | Fail |
|---|---|---|---|
| 01 | basic ops (touch, mkdir, cp, mv, rm, rename) | 49/50 | 1 |
| 02 | nested trees | 47/50 | 3 |
| 03 | large files (1KB–10MB) | 46/50 | 4 |
| 04 | bulk operations | 45/50 | 5 |
| 05 | snapshot / epoch | 47/50 | 3 |
| 06 | edge cases (special chars, mode bits) | 42/50 | 8 |
| 07 | concurrency (multi-thread/multi-process) | 13/50 | 37 |
| 08 | persistence (unmount/remount) | 49/50 | 1 |
| 09 | stress (≥32 files in single dir) | 50/50 | 0 |
| 10 | misc mode bits | 40/50 | 10 |

## Failure Analysis

### 1. count_mismatch (30 fails, 41% of all failures)
Tests that count files in a directory return fewer than the expected count.
Likely root causes:
- `iterdir()` returning a subset due to the FUSE readdir hardcoded cap (`src/fuse_vfs.c:298` — `vfs_readdir(state->vfs, dir_vp, ents, 64, state->epoch)` — only 64 entries per readdir call).
- Snapshot or remount state inconsistencies.
- Race condition in concurrent creates (overlaps with concurrency category).

### 2. concurrency (9 fails, 13%)
Multiple threads/processes doing creates, appends, renames. The FUSE user-space callback runs single-threaded per-mount, so concurrent operations are serialized. Tests that expect parallel semantics to work (last-writer-wins, atomic appends) fail.

### 3. content_corruption (12 fails, 17%)
Files that should have specific content don't:
- `overwrite` tests: written bytes don't match expectations.
- `append` tests: file size wrong (e.g. `append 999999` results in less than 1MB).
- `persist across unmount/remount`: written content is gone or wrong after remount.

These look like real VFS bugs in the write path or chain/tree update on overwrite.

### 4. mode_perm (6 fails, 8%)
`chmod 600/755/400/777/111/222` doesn't reflect in `vfs_getattr` (or is stripped). All 6 of these fail consistently — `chmod` likely isn't wired into the FUSE `setattr` callback.

### 5. deep_nesting (4 fails, 6%)
Operations on paths with depth > 5 fail. The radix tree implementation has known limitations on depth (see `phase-18-directory-radix-tree.md` and prior discussion: the tree degenerates to a single LEAF at level 15+).

### 6. special_chars (2 fails categorized, 8 fails total in batch_06)
Filenames with `(`, `|`, `&`, `;`, `'`, `\` fail to be created. Likely the harness's `run_shell` is not properly escaping these when passed via shell=True.

### 7. recursion (2 fails)
`cp -R` and `rm -R` followed by `ls` show partial results.

### 8. other (6 fails)
Snapshot + deep copy, plus special chars not in #6.

## Test infrastructure

### Harness
- `run_scenario_batch.py` — JSON-driven runner, mounts per-scenario, runs setup + verify steps, captures results.
- `batch_01.json` through `batch_10.json` — 50 unique scenarios each.
- `bench_ditto.py` — performance benchmark (not used in this report).

### VFS mount management
- Each scenario creates a fresh VFS file + mount point with a unique nonce path.
- `mount()` polls for the entry to appear in `mount(8)` (synchronous, 5s timeout).
- `unmount()` calls `diskutil unmount` and polls for the entry to disappear (5s timeout).
- `wait_for_fuse_ready()` creates a probe file, verifies it appears via `readdir` (proving the FUSE callback path is active), then removes the probe.

### Key findings during harness development
- **macFUSE mount warmup**: `vfs_fuse` exits within ~50-100ms after launching, but the kernel helper (`mount_macfuse`) needs ~500ms more before the FUSE callbacks actually fire. Without the `wait_for_fuse_ready` probe, the first ~30% of scenarios in a batch silently fail because FUSE create/write are dropped.
- **macFUSE process behavior**: `vfs_fuse` exits cleanly (rc=0) very early. The `proc.wait()` in the old `unmount()` was effectively a no-op. The actual session is held by the kernel helper; sync unmount requires polling `mount(8)` for the entry to disappear.
- **Stale mounts**: 4 truly stuck mounts from very old test runs cannot be unmounted even forcibly. The harness uses nonce paths to avoid collisions.
- **AppleDouble files**: macOS Finder and certain tools create `._filename` AppleDouble files alongside regular files. The harness filters these in `count_eq` and `total_count_eq` to avoid spurious failures.

## Reproduction

```bash
cd native/iXSphereVFS
make -j4 -C build
cd test/scenarios

# Run one batch (≈80-150s):
PYTHONPATH=. python3 -u run_scenario_batch.py batch_01.json

# Run all 10 batches (≈20-25 minutes total):
# Use the double-fork runner to survive parent shell teardown:
python3 /tmp/run_all.py   # see /tmp/run_all.py
```

## Recommended fixes (not implemented)

1. **Increase readdir cap from 64 to 1024+** in `src/fuse_vfs.c:298` — would fix most count_mismatch failures for medium-sized directories.
2. **Implement `setattr` mode handling** in `fuse_vfs_setattr` — fixes all 6 mode_perm failures.
3. **Fix overwrite semantics in `vfs_write`** — verify the write actually replaces existing content; the radix tree + chain update on overwrite may have a bug.
4. **Add a per-thread lock or atomic write for concurrent operations** — fixes concurrency failures. Currently single-threaded callbacks don't help when multiple kernel threads are writing to the same FD.
5. **Harness: properly escape special characters in `run_shell` argv** — fixes special_chars failures.
6. **Harness: include a snapshot-aware "exists" check that can read from prior epoch** — relevant to batch_05.

## Files

- `native/iXSphereVFS/test/scenarios/run_scenario_batch.py` — scenario runner
- `native/iXSphereVFS/test/scenarios/batch_01.json`–`batch_10.json` — 500 scenario definitions
- `native/iXSphereVFS/test/scenarios/results_batch_01.json`–`results_batch_10.json` — pass/fail results
- `native/iXSphereVFS/test/scenarios/bench_ditto.py` — performance benchmark
