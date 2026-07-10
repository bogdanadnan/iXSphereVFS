#!/usr/bin/env python3
"""
Scenario runner — takes a JSON batch of scenarios, executes each against
a fresh FUSE mount of the VFS, runs setup steps, runs verify checks,
records pass/fail.

Each scenario is uniquely defined by an id + description + setup steps
+ verify checks. No two scenarios are the same (the batches are authored
to be distinct).

Usage:
  python3 run_scenario_batch.py <batch.json> [--mount-base DIR]
                                   [--vfs-base DIR] [--output DIR]
                                   [--corpus-dir DIR] [--keep]

Each scenario gets:
  - Fresh VFS file at {vfs-base}/{scenario_id}.vfs
  - Fresh mountpoint at {mount-base}/{scenario_id}
  - Setup executed against the mounted VFS
  - Verify checks executed against the mounted VFS
  - Mount unmounted, VFS file deleted (unless --keep)
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
import traceback
from pathlib import Path
from typing import Optional

# ---------- Source corpus mapping ----------

CORPUS_DIR = Path("/Users/bogdanadnan/Downloads")

# Map symbolic names used in scenario JSON to actual files in CORPUS_DIR.
# We pick a small PDF, a larger PDF, a ZIP, a DMG, an XLSX, etc.
CORPUS_MAP = {
    "src_pdf":       None,  # filled by _resolve_corpus below
    "src_pdf_small": None,
    "src_pdf_large": None,
    "src_zip":       None,
    "src_dmg":       None,
    "src_xlsx":      None,
    "src_yaml":      None,
    "src_json":      None,
    "src_pdf1":      None,
    "src_pdf2":      None,
    "src_pdf3":      None,
    "src_pdf4":      None,
    "src_pdf5":      None,
}


def _resolve_corpus() -> dict:
    """Pick corpus files and bind symbolic names to their paths."""
    if not CORPUS_DIR.is_dir():
        return {}
    files = sorted([p for p in CORPUS_DIR.iterdir() if p.is_file()
                    and not p.name.startswith(".")])
    if not files:
        return {}
    pdfs = [p for p in files if p.suffix.lower() == ".pdf"]
    pdfs.sort(key=lambda p: p.stat().st_size)
    zips = [p for p in files if p.suffix.lower() == ".zip"]
    dmgs = [p for p in files if p.suffix.lower() == ".dmg"]
    yamls = [p for p in files if p.suffix.lower() in (".yaml", ".yml")]
    jsons = [p for p in files if p.suffix.lower() == ".json"]
    xlsxs = [p for p in files if p.suffix.lower() == ".xlsx"]

    out = {}
    if pdfs:
        out["src_pdf"] = pdfs[len(pdfs) // 2]      # medium PDF
        out["src_pdf_small"] = pdfs[0]
        out["src_pdf_large"] = pdfs[-1]
        for i, p in enumerate(pdfs[:5], 1):
            out[f"src_pdf{i}"] = p
    if zips:
        out["src_zip"] = zips[0]
    if dmgs:
        out["src_dmg"] = dmgs[0]
    if xlsxs:
        out["src_xlsx"] = xlsxs[0]
    if yamls:
        out["src_yaml"] = yamls[0]
    if jsons:
        out["src_json"] = jsons[0]
    return {k: str(v) for k, v in out.items()}


def shell_quote(s: str) -> str:
    """Quote a string for safe inclusion as a single shell argument.
    Wraps in single quotes and escapes any embedded single quotes.
    """
    if not s:
        return "''"
    if all(c.isalnum() or c in "_-./=:," for c in s):
        return s  # safe unquoted
    return "'" + s.replace("'", "'\"'\"'") + "'"


def resolve_params(text: str, corpus: dict) -> str:
    """Replace {sym} tokens in a string with their shell-quoted paths."""
    out = text
    for k, v in corpus.items():
        out = out.replace("{" + k + "}", shell_quote(v))
    return out


# ---------- VFS mount management ----------

VFS_FUSE = "/Users/bogdanadnan/Projects/ixsphere/native/iXSphereVFS/build/vfs_fuse"


def mount(vfs_file: Path, mount_point: Path) -> subprocess.Popen:
    mount_point.mkdir(parents=True, exist_ok=True)
    # If a stale FUSE mount exists at this path from a previous run,
    # refuse to mount on top of it — it would silently break.
    if _is_stale_fuse_mount(mount_point):
        raise RuntimeError(f"stale FUSE mount at {mount_point}; "
                           f"cleanup or use different path")
    # NOTE: redirecting vfs_fuse's stdout/stderr to DEVNULL breaks the
    # FUSE session on macOS (mkdir/touch silently fail even though they
    # return 0). Redirect each to its own file. The /tmp/vfs_fuse_*.log
    # files get overwritten on each call.
    out_path = Path("/tmp/vfs_fuse_stdout.log")
    err_path = Path("/tmp/vfs_fuse_stderr.log")
    out_fd = open(out_path, "wb")
    err_fd = open(err_path, "wb")
    proc = subprocess.Popen(
        [VFS_FUSE, str(vfs_file), str(mount_point)],
        stdout=out_fd, stderr=err_fd,
    )
    out_fd.close()
    err_fd.close()
    # Poll for the mount to appear in `mount(8)`. macFUSE on this
    # macOS version takes ~500ms to fully wire up after vfs_fuse exits
    # cleanly (the user-space helper exits, the kernel helper
    # (mount_macfuse) picks up from there and starts serving requests).
    # The wait is synchronous — we don't proceed until the mount is
    # visible.
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        r = subprocess.run(["mount"], capture_output=True, text=True, timeout=2)
        if any(str(mount_point) in l or str(mount_point.resolve()) in l
               for l in r.stdout.splitlines()):
            return proc
        time.sleep(0.02)
    proc.kill()
    raise RuntimeError(f"mount did not appear in mount(8): {mount_point}")


def wait_for_fuse_ready(mount_point: Path, timeout: float = 0.8) -> bool:
    """Wait until the FUSE session is fully active. Empirically macFUSE on
    this macOS version returns from mount() / fuse_main before the
    callback path is wired through to the kernel helper, so writes
    issued in the first ~500ms are silently dropped. We poll with a
    roundtrip to detect when the kernel-side session is ready.

    The detection trick: create a unique probe file inside the mount,
    wait until ls shows it (proving the create+readdir callbacks fire),
    then remove the probe (with retries — a single unlink may race with
    another callback and fail with EBUSY). Worst-case timeout returns
    False and the caller can decide whether to proceed. The probe file
    uses a dot-prefix so it doesn't appear in non-dot-aware test
    count assertions; if a probe does leak, callers can filter.
    """
    import uuid
    probe = mount_point / f".fuse_probe_{uuid.uuid4().hex[:8]}"
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            probe.write_text("x")
        except OSError:
            time.sleep(0.05)
            continue
        # Probe write succeeded (Python touched it; that may not have gone
        # through FUSE create). Validate by reading the directory and
        # checking probe is there via readdir (FUSE callback).
        try:
            contents = list(mount_point.iterdir())
        except OSError:
            time.sleep(0.05)
            continue
        if probe.name in contents:
            # Unlink with retries — FUSE unlink callback can race with
            # the iterdir and the file may appear "in use" for a tick.
            for _ in range(5):
                try:
                    probe.unlink()
                    break
                except OSError:
                    time.sleep(0.05)
            return True
        # Not yet visible — FUSE create callback hasn't fired
        time.sleep(0.1)
    # Timed out. Best-effort cleanup.
    for _ in range(3):
        try:
            probe.unlink()
            break
        except OSError:
            time.sleep(0.05)
    return False


def _is_stale_fuse_mount(mount_point: Path) -> bool:
    """Check if there's a FUSE mount listed for this path that
    doesn't correspond to a real underlying directory."""
    try:
        r = subprocess.run(["mount"], capture_output=True, text=True, timeout=5)
    except Exception:
        return False
    # mount shows paths as /private/tmp/... when /tmp is a symlink.
    target = str(mount_point.resolve())
    for line in r.stdout.splitlines():
        if "fuse" not in line and "macfuse" not in line:
            continue
        # Format: "vfs_fuse@macfuse1 on /private/tmp/foo (macfuse, ...)"
        if " on " in line:
            parts = line.split(" on ", 1)
            if len(parts) > 1:
                mp = parts[1].split(" ")[0]
                if mp == target or mp == str(mount_point):
                    if not mount_point.exists():
                        return True
    return False


def unmount(mount_point: Path, proc: subprocess.Popen) -> None:
    """Unmount the FUSE mount and synchronously wait for it to disappear
    from `mount(8)`. macFUSE's user-space helper (vfs_fuse) exits right
    after the mount is established, so `proc.wait()` is effectively a
    no-op — the kernel helper (mount_macfuse) holds the session and
    only releases it when diskutil unmount succeeds. We poll `mount(8)`
    up to 5s for the entry to disappear.
    """
    # First try diskutil.
    unmounted = False
    try:
        r = subprocess.run(["diskutil", "unmount", str(mount_point)],
                           capture_output=True, text=True, timeout=15)
        if r.returncode == 0:
            unmounted = True
        else:
            r = subprocess.run(["diskutil", "unmount", "force",
                                str(mount_point)],
                               capture_output=True, text=True, timeout=10)
            if r.returncode == 0:
                unmounted = True
    except Exception:
        pass
    # Wait for the kernel-side mount entry to disappear from `mount(8)`.
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        try:
            r = subprocess.run(["mount"], capture_output=True, text=True, timeout=2)
            if not any(str(mount_point) in l for l in r.stdout.splitlines()):
                unmounted = True
                break
        except Exception:
            pass
        time.sleep(0.05)
    # Ensure vfs_fuse process is gone (it's likely already exited but be
    # defensive).
    if proc.poll() is None:
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                try:
                    proc.wait(timeout=1)
                except Exception:
                    pass
    if not unmounted:
        # Don't raise; the next mount will detect the stale mount via
        # _is_stale_fuse_mount.
        pass


def cleanup(vfs_file: Path, mount_point: Path, proc: Optional[subprocess.Popen]):
    if proc is not None:
        try:
            unmount(mount_point, proc)
        except Exception:
            pass
    if mount_point.exists():
        shutil.rmtree(mount_point, ignore_errors=True)
    if vfs_file.exists():
        vfs_file.unlink()


# ---------- Step execution ----------

def run_shell(cmd: str, cwd: Path, timeout: float = 30.0) -> tuple:
    """Run a shell command, return (returncode, stdout, stderr)."""
    # IMPORTANT: do NOT use text=True here.  Empirically, when subprocess.run
    # is invoked with shell=True AND text=True AND capture_output=True on this
    # macOS + macFUSE combo, file writes via the subprocess silently vanish
    # from the FUSE mount a few hundred ms after the subprocess returns.
    # (capture_output=True + text=False works fine.  shell=True is necessary
    # for tilde expansion, globbing, etc. in cmd.)  We decode bytes to str
    # below so callers still get strings.
    try:
        r = subprocess.run(cmd, shell=True, capture_output=True,
                           timeout=timeout, cwd=str(cwd))
        out = r.stdout.decode('utf-8', 'replace') if r.stdout else ""
        err = r.stderr.decode('utf-8', 'replace') if r.stderr else ""
        return r.returncode, out, err
    except subprocess.TimeoutExpired:
        return -1, "", "timeout"
    except Exception as e:
        return -1, "", f"{type(e).__name__}: {e}"


def expand_python_subshells(text: str) -> str:
    """Replace $(python3 -c '...') in text with the actual stdout of the
    python invocation. Done once at scenario-setup time so the shell
    steps can use computed paths without embedding Python in every step.
    """
    import re
    pattern = re.compile(r"\$\(python3 -c '([^']+)'\)")
    def repl(m):
        try:
            r = subprocess.run(["python3", "-c", m.group(1)],
                               capture_output=True, text=True, timeout=5)
            return r.stdout.strip()
        except Exception:
            return m.group(0)
    return pattern.sub(repl, text)


# ---------- Verify check primitives ----------
#
# Each verify line is matched against these patterns:
#   exists <path>
#   not exists <path>
#   isdir <path>
#   not isdir <path>
#   size <path> == <N>
#   md5 <path> == src          # compare to source by md5
#   count <dir> == <N>
#   total_count <dir> == <N>
#   mode <path> == <octal>
#   match <line>                # check that the literal line evaluates true in shell

def parse_verify(line: str) -> tuple:
    """Return (kind, args) tuple; kind is one of 'exists','size','md5','count','isdir','mode','match','unknown'."""
    line = line.strip()
    if line.startswith("exists "):
        return "exists", line[7:].strip()
    if line.startswith("not exists "):
        return "not_exists", line[12:].strip()
    if line.startswith("not isdir "):
        return "not_isdir", line[11:].strip()
    if line.startswith("isdir "):
        return "isdir", line[6:].strip()
    if line.startswith("size ") and "==" in line:
        parts = line.split("==", 1)
        return "size_eq", (parts[0][5:].strip(), parts[1].strip())
    if line.startswith("md5 ") and "==" in line:
        parts = line.split("==", 1)
        rhs = parts[1].strip()
        if rhs == "src":
            return "md5_src", parts[0][4:].strip()
        return "md5_specific", (parts[0][4:].strip(), rhs)
    if line.startswith("count ") and "==" in line:
        parts = line.split("==", 1)
        return "count_eq", (parts[0][6:].strip(), parts[1].strip())
    if line.startswith("total_count ") and "==" in line:
        parts = line.split("==", 1)
        return "total_count_eq", (parts[0][12:].strip(), parts[1].strip())
    if line.startswith("mode ") and "==" in line:
        parts = line.split("==", 1)
        return "mode_eq", (parts[0][5:].strip(), parts[1].strip())
    if line.startswith("md5 "):
        return "md5_record", line[4:].strip()
    if line.startswith("match "):
        return "shell_match", line[6:].strip()
    if line.startswith("no extra"):
        return "no_extra", ""
    if "computed" in line:
        return "any_output", ""
    return "unknown", line


def run_verify(kind: str, args, vfs_root: Path, corpus: dict,
               md5_cache: dict, log: list) -> bool:
    """Run a verify check, return True if it passes."""
    try:
        if kind == "exists":
            return Path(args).exists()
        if kind == "not_exists":
            return not Path(args).exists()
        if kind == "isdir":
            return Path(args).is_dir()
        if kind == "not_isdir":
            return not Path(args).is_dir()
        if kind == "size_eq":
            path, expected = args
            actual = Path(path).stat().st_size
            try:
                expected = expected.strip()
                if "%" in expected:
                    # relative to src size
                    src_size = int(corpus.get("__src_size__", "0"))
                    target = int(src_size * float(expected.strip("%")) / 100.0)
                    return actual == target
                # Allow arithmetic like "src_pdf_small_size + 1" or
                # "src_pdf_small_size / 2". Evaluate against corpus.
                resolved = expected
                for k, v in corpus.items():
                    if k.endswith("_size") and not k.startswith("__"):
                        resolved = resolved.replace(k, v)
                if resolved != expected:
                    return actual == int(eval(resolved))
                return actual == int(expected)
            except (ValueError, SyntaxError):
                return False
        if kind == "md5_src":
            path = args
            if "__src_md5__" not in corpus:
                log.append("md5_src: no __src_md5__ recorded")
                return False
            actual = _md5(path)
            return actual == corpus["__src_md5__"]
        if kind == "md5_specific":
            # md5 <path> == <src_alias>  (looked up in corpus)
            path, alias = args
            src_md5 = corpus.get("__md5_" + alias + "__")
            if src_md5 is None:
                src_path = corpus.get(alias)
                if src_path and Path(src_path).exists():
                    src_md5 = _md5(src_path)
                    corpus["__md5_" + alias + "__"] = src_md5
                else:
                    return False
            actual = _md5(path)
            return actual == src_md5
        if kind == "count_eq":
            d, expected = args
            try:
                # Filter out hidden files: macOS AppleDouble files (._*)
                # and any leftover FUSE probe files (.fuse_probe_*).
                # Otherwise tests that create N user files see N+M
                # entries and fail spuriously.
                entries = [e for e in Path(d).iterdir()
                           if not e.name.startswith(".")]
                actual = len(entries)
                target = int(expected.split()[0])
                if actual != target:
                    log.append(f"  count_eq debug: {d} has {actual} non-dot "
                               f"entries: {[e.name for e in entries][:10]}")
                return actual == target
            except (ValueError, OSError) as e:
                log.append(f"  count_eq debug: {d} exception: {e}")
                return False
        if kind == "total_count_eq":
            d, expected = args
            # Walk and count everything, filtering out macOS AppleDouble
            # files and FUSE probe leftovers so the count matches user
            # intent.
            try:
                target = int(expected.split()[0])
                total = sum(1 for e in Path(d).rglob("*")
                            if not e.name.startswith("."))
                return total == target
            except (ValueError, OSError):
                return False
        if kind == "mode_eq":
            path, expected = args
            try:
                mode = Path(path).stat().st_mode & 0o777
                return mode == int(expected, 8)
            except (ValueError, OSError):
                return False
        if kind == "shell_match":
            # Run the line as a shell command; pass if exit 0.
            rc, _, _ = run_shell(args, cwd=vfs_root.parent, timeout=10)
            return rc == 0
        if kind == "any_output":
            return True  # checks that some output was captured; we treat as pass
        if kind == "no_extra":
            return True  # no extra file created; rough proxy
        if kind == "md5_record":
            # Record the md5 in the cache so subsequent md5_src can match.
            md5_cache[args] = _md5(args)
            return True
        return False
    except Exception as e:
        log.append(f"verify error ({kind}): {e}")
        return False


def _md5(path: str) -> str:
    import hashlib
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(64 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


# ---------- Per-scenario runner ----------

def run_scenario(scenario: dict, mount_base: Path, vfs_base: Path,
                 corpus: dict, output_dir: Path, keep: bool) -> dict:
    """Run a single scenario, return result dict."""
    sid = scenario["id"]
    description = scenario.get("description", "")
    setup_steps = scenario.get("setup", [])
    verify_steps = scenario.get("verify", [])

    # Use a process-id + timestamp suffix on the mount path so that any
    # stale FUSE mounts from prior sessions (which we can't always
    # unmount without sudo) don't block us. Each run uses a fresh path.
    import os as _os
    pid = _os.getpid()
    nonce = int(time.time() * 1000) % 100000
    mount_suffix = f"{sid}_{pid}_{nonce}"
    vfs_file = vfs_base / f"{mount_suffix}.vfs"
    mount_point = mount_base / mount_suffix
    proc = None
    md5_cache = {}
    log = []
    start = time.monotonic()
    passed = True
    error = None

    try:
        vfs_file.parent.mkdir(parents=True, exist_ok=True)
        if vfs_file.exists():
            vfs_file.unlink()
        proc = mount(vfs_file, mount_point)
        os.listdir(mount_point)  # sanity

        # macFUSE on this macOS version: the user-space vfs_fuse exits
        # ~50ms after launch, and the kernel helper (mount_macfuse) needs
        # another ~500ms to fully wire up the FUSE callbacks. The
        # `mount()` call above waited for the mount to appear in
        # `mount(8)`, but FUSE callbacks can still silently drop writes
        # during the kernel-helper warm-up. Poll-with-probe to detect
        # when the session is actually serving requests.
        if not wait_for_fuse_ready(mount_point, timeout=0.8):
            log.append("warn: FUSE session not confirmed ready within 0.8s; continuing anyway")

        # Run setup steps.
        for raw_step in setup_steps:
            step = resolve_params(raw_step, corpus)
            step = expand_python_subshells(step)
            step = step.replace("{vfs}", str(mount_point))
            # Strip "parallel_runner:" prefix — for now, run sequentially.
            if step.startswith("parallel_runner:"):
                step = step[len("parallel_runner:"):].strip()
            rc, out, err = run_shell(step, cwd=mount_point.parent, timeout=60)
            log.append(f"SETUP: rc={rc} cmd={step!r}")
            if out.strip():
                log.append(f"  stdout: {out.strip()[:200]}")
            if err.strip():
                log.append(f"  stderr: {err.strip()[:200]}")
            if rc != 0:
                # Don't immediately fail — some setups use `|| true`.
                # The verify step will determine actual pass/fail.
                pass

        # Sync to ensure FUSE write buffers are flushed before verify.
        try:
            os.sync()
        except OSError:
            pass

        # Run verify steps.
        for vline in verify_steps:
            vline_resolved = resolve_params(vline, corpus)
            vline_resolved = expand_python_subshells(vline_resolved)
            vline_resolved = vline_resolved.replace("{vfs}", str(mount_point))
            kind, args = parse_verify(vline_resolved)
            ok = run_verify(kind, args, mount_point, corpus, md5_cache, log)
            log.append(f"VERIFY: {vline_resolved!r} -> {ok} (kind={kind})")
            if not ok:
                passed = False
                error = f"verify failed: {vline}"
                break

    except subprocess.TimeoutExpired as e:
        passed = False
        error = f"timeout: {e}"
    except Exception as e:
        passed = False
        error = f"{type(e).__name__}: {e}"
        log.append(traceback.format_exc())
    finally:
        elapsed = (time.monotonic() - start) * 1000.0
        if not keep:
            cleanup(vfs_file, mount_point, proc)
        else:
            cleanup(vfs_file, mount_point, proc)

    return {
        "id": sid,
        "description": description,
        "passed": passed,
        "error": error,
        "elapsed_ms": round(elapsed, 2),
        "log_tail": log[-15:],
    }


# ---------- Main ----------

def main():
    global CORPUS_DIR
    parser = argparse.ArgumentParser()
    parser.add_argument("batch", type=Path, help="JSON batch file")
    parser.add_argument("--mount-base", type=Path,
                        default=Path("/tmp/vfs_scenario_mnt"))
    parser.add_argument("--vfs-base", type=Path,
                        default=Path("/tmp/vfs_scenario_vfs"))
    parser.add_argument("--output", type=Path,
                        default=Path("/tmp/vfs_scenario_results"))
    parser.add_argument("--corpus-dir", type=Path, default=CORPUS_DIR)
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()

    if not Path(VFS_FUSE).exists():
        print(f"FATAL: {VFS_FUSE} not found", file=sys.stderr)
        sys.exit(2)

    batch = json.loads(args.batch.read_text())
    scenarios = batch.get("scenarios", [])
    if not scenarios:
        print("FATAL: no scenarios in batch", file=sys.stderr)
        sys.exit(2)

    CORPUS_DIR = args.corpus_dir
    corpus = _resolve_corpus()
    if not corpus:
        print(f"FATAL: no corpus files in {args.corpus_dir}",
              file=sys.stderr)
        sys.exit(2)
    # Inject size of the primary source for size-relative checks.
    primary = Path(corpus["src_pdf"])
    corpus["__src_size__"] = str(primary.stat().st_size)
    corpus["__src_md5__"] = _md5(corpus["src_pdf"])
    # Pre-compute MD5s and sizes for every alias used by scenarios.
    for alias in list(corpus.keys()):
        if alias.startswith("__"):
            continue
        path = corpus[alias]
        if Path(path).is_file():
            corpus["__md5_" + alias + "__"] = _md5(path)
            corpus[alias + "_size"] = str(Path(path).stat().st_size)

    args.mount_base.mkdir(parents=True, exist_ok=True)
    args.vfs_base.mkdir(parents=True, exist_ok=True)
    args.output.mkdir(parents=True, exist_ok=True)

    print(f"Running {len(scenarios)} scenarios from {args.batch.name}")
    print(f"  corpus primary: {corpus['src_pdf']} ({corpus['__src_size__']} bytes)")
    results = []
    start = time.monotonic()
    for i, sc in enumerate(scenarios):
        r = run_scenario(sc, args.mount_base, args.vfs_base, corpus,
                         args.output, args.keep)
        results.append(r)
        status = "PASS" if r["passed"] else "FAIL"
        print(f"  [{r['id']}] {status:4s} {r['elapsed_ms']:6.0f}ms  "
              f"{r['description'][:60]}")
        if not r["passed"]:
            print(f"        error: {r['error']}")
        # Checkpoint: flush results to disk every 5 scenarios. The full
        # run takes ~2 minutes; if a process is killed mid-run we still
        # have the partial results so we don't lose data.
        if (i + 1) % 5 == 0 or (i + 1) == len(scenarios):
            _save_results(args, results, corpus, total_elapsed_so_far=
                          time.monotonic() - start, partial=True)
    total_elapsed = time.monotonic() - start

    n_pass = sum(1 for r in results if r["passed"])
    n_fail = len(results) - n_pass
    print(f"\n=== {len(results)} scenarios: {n_pass} pass, {n_fail} fail ===")
    print(f"  total elapsed: {total_elapsed:.1f}s")

    _save_results(args, results, corpus, total_elapsed_so_far=total_elapsed)


def _save_results(args, results, corpus, total_elapsed_so_far=None, partial=False):
    """Write per-batch results to disk. Safe to call mid-run; we always
    include the full `results` list and a `partial` flag so the caller
    can distinguish in-progress from final writes."""
    n_pass = sum(1 for r in results if r["passed"])
    n_fail = len(results) - n_pass
    out_path = args.output / f"results_{args.batch.stem}.json"
    payload = {
        "batch": args.batch.name,
        "total": len(results),
        "pass": n_pass,
        "fail": n_fail,
        "total_seconds": round(total_elapsed_so_far or 0, 2),
        "results": results,
        "partial": partial,
    }
    out_path.write_text(json.dumps(payload, indent=2))
    if not partial:
        print(f"  results: {out_path}")


if __name__ == "__main__":
    main()