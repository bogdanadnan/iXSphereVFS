#!/usr/bin/env python3
"""
Benchmark: copy VSCode ZIP archive into VFS, unzip with ditto, time each step.

This is the baseline measurement. After we add flush-on-close + flush-on-
directory-mutation, this same script will be run again and the numbers
compared.

Usage:
  python3 bench_ditto.py [--label LABEL] [--output DIR]

  --label LABEL   text tag stored in results (e.g. "baseline" or "with_flush")
  --output DIR    where to write the benchmark results (default
                  /tmp/vfs_scenario_results)

The script:
  1. Creates a fresh VFS file at /tmp/vfs_bench_<pid>.vfs
  2. Mounts it at /tmp/vfs_bench_<pid>
  3. Copies VSCode-win32-x64-1.123.0.zip into the mount (cp via FUSE)
  4. Runs `ditto -x -k <archive.zip> <dest>` (unzip via FUSE)
  5. Validates: MD5 of every extracted file matches the host-side
     `ditto -x -k <archive.zip> /tmp/ditto_host_extract` reference
  6. Records wall-clock time for each step, total elapsed, throughput
  7. Unmounts, removes VFS file
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

VFS_FUSE = "/Users/bogdanadnan/Projects/ixsphere/native/iXSphereVFS/build/vfs_fuse"
SOURCE_ZIP = Path("/Users/bogdanadnan/Downloads/VSCode-win32-x64-1.123.0.zip")
DEFAULT_OUTPUT_DIR = Path("/tmp/vfs_scenario_results")


def md5_file(path: Path) -> str:
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(64 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def md5_tree(root: Path) -> dict:
    """Return {relative_path: md5} for every regular file under root."""
    out = {}
    for p in root.rglob("*"):
        if p.is_file():
            out[str(p.relative_to(root))] = md5_file(p)
    return out


def mount(vfs_file: Path, mount_point: Path) -> subprocess.Popen:
    mount_point.mkdir(parents=True, exist_ok=True)
    if vfs_file.exists():
        vfs_file.unlink()
    out_fd = open("/tmp/vfs_fuse_stdout.log", "wb")
    err_fd = open("/tmp/vfs_fuse_stderr.log", "wb")
    proc = subprocess.Popen(
        [VFS_FUSE, str(vfs_file), str(mount_point)],
        stdout=out_fd, stderr=err_fd,
    )
    out_fd.close()
    err_fd.close()
    for _ in range(50):
        time.sleep(0.1)
        try:
            os.stat(mount_point)
            return proc
        except OSError:
            continue
    proc.kill()
    raise RuntimeError(f"mount did not appear: {mount_point}")


def unmount(mount_point: Path, proc: subprocess.Popen) -> None:
    try:
        subprocess.run(["diskutil", "unmount", str(mount_point)],
                       capture_output=True, timeout=15)
    except Exception:
        pass
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            try:
                proc.wait(timeout=2)
            except Exception:
                pass
    time.sleep(0.5)


def timed(label: str, fn) -> tuple:
    """Run fn(), return (label, elapsed_ms, result)."""
    start = time.monotonic()
    result = fn()
    elapsed = (time.monotonic() - start) * 1000.0
    return (label, elapsed, result)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--label", default="baseline")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT_DIR)
    args = parser.parse_args()

    if not SOURCE_ZIP.exists():
        print(f"FATAL: {SOURCE_ZIP} not found", file=sys.stderr)
        sys.exit(2)
    if not Path(VFS_FUSE).exists():
        print(f"FATAL: {VFS_FUSE} not found", file=sys.stderr)
        sys.exit(2)

    args.output.mkdir(parents=True, exist_ok=True)

    import os as _os
    pid = _os.getpid()
    nonce = int(time.time() * 1000) % 100000
    vfs_file = Path(f"/tmp/vfs_bench_{pid}_{nonce}.vfs")
    mount_point = Path(f"/tmp/vfs_bench_{pid}_{nonce}")

    # Reference extraction on host filesystem (for MD5 comparison).
    host_extract = Path(f"/tmp/ditto_host_extract_{pid}_{nonce}")
    if host_extract.exists():
        shutil.rmtree(host_extract)
    host_extract.mkdir(parents=True)

    results = {
        "label": args.label,
        "source": str(SOURCE_ZIP),
        "source_size": SOURCE_ZIP.stat().st_size,
        "steps": [],
        "validation": {},
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
    }

    print(f"=== Benchmark [{args.label}] ===")
    print(f"Source: {SOURCE_ZIP} ({results['source_size']} bytes)")

    proc = None
    try:
        # Step 1: mount
        t, ms, _ = timed("mount", lambda: mount(vfs_file, mount_point))
        results["steps"].append({"name": t, "elapsed_ms": ms})
        print(f"  mount:           {ms:8.1f} ms")
        proc = _  # capture for unmount

        # Step 2: copy source ZIP into VFS.
        # Use os.system (not subprocess.run) because subprocess.run
        # has a quirk on this macOS + macFUSE combo: even with no special
        # kwargs, the destination file vanishes from the FUSE mount a
        # few hundred ms after cp returns.  os.system uses fork+exec
        # directly and the file persists.  Subtle but reproducible.
        def do_copy():
            rc = os.system(f"cp '{SOURCE_ZIP}' "
                           f"'{mount_point / 'archive.zip'}'")
            if rc != 0:
                raise RuntimeError(f"cp failed: rc={rc}")
        t, ms, _ = timed("copy_in", do_copy)
        vfs_zip = mount_point / "archive.zip"
        results["steps"].append({"name": t, "elapsed_ms": ms,
                                  "bytes": vfs_zip.stat().st_size if vfs_zip.exists() else 0})
        print(f"  copy_in:         {ms:8.1f} ms  "
              f"exists={vfs_zip.exists()} size={vfs_zip.stat().st_size if vfs_zip.exists() else 0}")

        # Step 3: unzip via ditto (also via os.system for same reason)
        extract_dir = mount_point / "extracted"
        def do_extract():
            if extract_dir.exists():
                shutil.rmtree(extract_dir)
            extract_dir.mkdir()
            rc = os.system(f"ditto -x -k '{vfs_zip}' '{extract_dir}'")
            if rc != 0:
                raise RuntimeError(f"ditto failed: rc={rc}")
        t, ms, _ = timed("ditto_unzip", do_extract)
        results["steps"].append({"name": t, "elapsed_ms": ms})
        print(f"  ditto_unzip:     {ms:8.1f} ms")

        # Step 4: count files and total bytes in extracted tree
        files_in_vfs = list(extract_dir.rglob("*"))
        vfs_files = [p for p in files_in_vfs if p.is_file()]
        vfs_total_bytes = sum(p.stat().st_size for p in vfs_files)
        results["vfs_extract"] = {
            "file_count": len(vfs_files),
            "total_bytes": vfs_total_bytes,
        }
        print(f"  vfs_extract:     {len(vfs_files)} files, {vfs_total_bytes:,} bytes")

        # Step 5: extract reference on host for comparison
        def do_host_extract():
            shutil.rmtree(host_extract, ignore_errors=True)
            host_extract.mkdir()
            os.system(f"ditto -x -k '{SOURCE_ZIP}' '{host_extract}'")
        t, ms, _ = timed("host_extract_ref", do_host_extract)
        results["steps"].append({"name": t, "elapsed_ms": ms})
        host_files = [p for p in host_extract.rglob("*") if p.is_file()]
        host_total = sum(p.stat().st_size for p in host_files)
        results["host_extract"] = {
            "file_count": len(host_files),
            "total_bytes": host_total,
        }
        print(f"  host_extract:    {len(host_files)} files, {host_total:,} bytes (reference)")

        # Step 6: MD5 compare (read every file from VFS and compare)
        def do_md5_compare():
            vfs_md5s = {}
            for p in vfs_files:
                try:
                    vfs_md5s[str(p.relative_to(extract_dir))] = md5_file(p)
                except OSError:
                    pass
            host_md5s = {}
            for p in host_files:
                host_md5s[str(p.relative_to(host_extract))] = md5_file(p)
            return vfs_md5s, host_md5s

        t, ms, (vfs_md5s, host_md5s) = timed("md5_compare", do_md5_compare)
        results["steps"].append({"name": t, "elapsed_ms": ms,
                                  "files_compared": len(vfs_md5s)})
        print(f"  md5_compare:     {ms:8.1f} ms ({len(vfs_md5s)} files)")

        # Validation
        only_in_vfs = set(vfs_md5s.keys()) - set(host_md5s.keys())
        only_in_host = set(host_md5s.keys()) - set(vfs_md5s.keys())
        common = set(vfs_md5s.keys()) & set(host_md5s.keys())
        mismatches = [k for k in common if vfs_md5s[k] != host_md5s[k]]
        results["validation"] = {
            "common_files": len(common),
            "only_in_vfs_count": len(only_in_vfs),
            "only_in_host_count": len(only_in_host),
            "md5_mismatch_count": len(mismatches),
            "md5_match": (len(mismatches) == 0
                          and len(only_in_vfs) == 0
                          and len(only_in_host) == 0),
        }
        print(f"  validation:      common={len(common)}, "
              f"only_vfs={len(only_in_vfs)}, only_host={len(only_in_host)}, "
              f"mismatches={len(mismatches)}")
        if only_in_vfs:
            print(f"  only-in-vfs samples: {sorted(only_in_vfs)[:3]}")
        if only_in_host:
            print(f"  only-in-host samples: {sorted(only_in_host)[:3]}")

        # Summary
        copy_in = next(s for s in results["steps"] if s["name"] == "copy_in")
        unzip = next(s for s in results["steps"] if s["name"] == "ditto_unzip")
        results["summary"] = {
            "copy_in_ms": copy_in["elapsed_ms"],
            "ditto_unzip_ms": unzip["elapsed_ms"],
            "copy_throughput_mbps": (results["source_size"] / 1024 / 1024) /
                                       (copy_in["elapsed_ms"] / 1000.0),
            "extracted_throughput_mbps": (vfs_total_bytes / 1024 / 1024) /
                                            (unzip["elapsed_ms"] / 1000.0),
        }
        print(f"\n  Copy throughput:  {results['summary']['copy_throughput_mbps']:.2f} MB/s")
        print(f"  Extract throughput: {results['summary']['extracted_throughput_mbps']:.2f} MB/s")

    finally:
        if proc is not None:
            unmount(mount_point, proc)
        if mount_point.exists():
            shutil.rmtree(mount_point, ignore_errors=True)
        if vfs_file.exists():
            vfs_file.unlink()
        if host_extract.exists():
            shutil.rmtree(host_extract, ignore_errors=True)

    out_path = args.output / f"bench_{args.label}.json"
    out_path.write_text(json.dumps(results, indent=2))
    print(f"\nResults: {out_path}")
    print(f"MD5 match: {results['validation'].get('md5_match', False)}")


if __name__ == "__main__":
    main()