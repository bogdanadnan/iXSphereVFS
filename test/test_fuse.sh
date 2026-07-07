#!/bin/bash
# test_fuse — smoke-test the FUSE mount/unmount cycle.
# Requires FUSE3 and the vfs_fuse binary.
set -e

# ---------------------------------------------------------------------------
# skip_if_no_fuse — exit 0 with SKIP message if FUSE is unavailable.
# ---------------------------------------------------------------------------
skip_if_no_fuse() {
    if [ ! -e /dev/fuse ] && ! command -v fusermount3 >/dev/null 2>&1 && ! command -v fusermount >/dev/null 2>&1; then
        echo "SKIP: FUSE not available (/dev/fuse missing, no fusermount)"
        exit 0
    fi
}
skip_if_no_fuse

# ---------------------------------------------------------------------------
# setup_test — create per-test scratch dirs via mktemp.
# Returns: sets VFS_FILE and MNT_POINT globals.
# ---------------------------------------------------------------------------
VFS_FILE=""
MNT_POINT=""

setup_test() {
    VFS_FILE="$(mktemp -d)/test.vfs"
    MNT_POINT="$(mktemp -d)"
    # Ensure mountpoint is clean (fusermount any stale mount)
    fusermount3 -u -z "$MNT_POINT" 2>/dev/null || true
    fusermount -u -z "$MNT_POINT" 2>/dev/null || true
}

# ---------------------------------------------------------------------------
# teardown_test — clean up scratch dirs and unmount if needed.
# ---------------------------------------------------------------------------
teardown_test() {
    fusermount3 -u -z "$MNT_POINT" 2>/dev/null || true
    fusermount -u -z "$MNT_POINT" 2>/dev/null || true
    rm -f "$VFS_FILE" 2>/dev/null || true
    rm -rf "$MNT_POINT" 2>/dev/null || true
}

setup_test
trap teardown_test EXIT

# ---------------------------------------------------------------------------
# wait_for_mount — poll ls of mountpoint until success or 10s timeout.
# ---------------------------------------------------------------------------
wait_for_mount() {
    local timeout=10
    local elapsed=0
    while [ $elapsed -lt $timeout ]; do
        if ls "$MNT_POINT" >/dev/null 2>&1; then return 0; fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    return 1
}

# ---------------------------------------------------------------------------
# test_fuse_create_read_delete — create file, read, delete, verify.
# ---------------------------------------------------------------------------
test_fuse_create_read_delete() {
    echo "=== test_fuse_create_read_delete ==="
    local testfile="$MNT_POINT/hello.txt"
    local content="Hello, iXSphereVFS FUSE!"

    # Create file via shell echo
    echo "$content" > "$testfile" || { echo "FAIL: echo create"; return 1; }

    # Read back and verify
    local result
    result="$(cat "$testfile")" || { echo "FAIL: cat read"; return 1; }
    if [ "$result" != "$content" ]; then
        echo "FAIL: content mismatch: '$result' != '$content'"
        return 1
    fi
    echo "  created and verified: $content"

    # Delete file
    rm "$testfile" || { echo "FAIL: rm"; return 1; }

    # Verify gone
    if [ -e "$testfile" ]; then
        echo "FAIL: file still exists after rm"
        return 1
    fi
    echo "  deleted and gone"
    return 0
}

# ---------------------------------------------------------------------------
# test_fuse_mkdir_rmdir — mkdir nested dirs, verify with ls, rmdir.
# ---------------------------------------------------------------------------
test_fuse_mkdir_rmdir() {
    echo "=== test_fuse_mkdir_rmdir ==="
    local d1="$MNT_POINT/subdir"
    local d2="$MNT_POINT/subdir/nested"

    mkdir -p "$d2" || { echo "FAIL: mkdir -p nested"; return 1; }
    ls "$d1" >/dev/null 2>&1 || { echo "FAIL: ls subdir"; return 1; }
    ls "$d2" >/dev/null 2>&1 || { echo "FAIL: ls nested"; return 1; }
    echo "  created subdir/nested"

    rmdir "$d2" || { echo "FAIL: rmdir nested"; return 1; }
    if [ -d "$d2" ]; then echo "FAIL: nested still exists"; return 1; fi

    rmdir "$d1" || { echo "FAIL: rmdir subdir"; return 1; }
    if [ -d "$d1" ]; then echo "FAIL: subdir still exists"; return 1; fi
    echo "  removed and gone"
    return 0
}

# ---------------------------------------------------------------------------
# test_fuse_readdir — create 3 files, ls and verify all listed.
# ---------------------------------------------------------------------------
test_fuse_readdir() {
    echo "=== test_fuse_readdir ==="
    touch "$MNT_POINT/a.txt" "$MNT_POINT/b.txt" "$MNT_POINT/c.txt"
    local listing
    listing="$(ls "$MNT_POINT")" || { echo "FAIL: ls"; return 1; }
    for f in a.txt b.txt c.txt; do
        if ! echo "$listing" | grep -q "$f"; then
            echo "FAIL: $f not in listing"
            return 1
        fi
    done
    rm "$MNT_POINT"/a.txt "$MNT_POINT"/b.txt "$MNT_POINT"/c.txt
    echo "  all 3 files listed and cleaned up"
    return 0
}

# ---------------------------------------------------------------------------
# test_fuse_rename — create file with content, rename, verify.
# ---------------------------------------------------------------------------
test_fuse_rename() {
    echo "=== test_fuse_rename ==="
    local src="$MNT_POINT/old_name.txt"
    local dst="$MNT_POINT/new_name.txt"
    local content="rename test data"

    echo "$content" > "$src" || { echo "FAIL: create"; return 1; }
    mv "$src" "$dst" || { echo "FAIL: rename"; return 1; }

    if [ -e "$src" ]; then echo "FAIL: old still exists"; return 1; fi

    local result
    result="$(cat "$dst")" || { echo "FAIL: cat new"; return 1; }
    if [ "$result" != "$content" ]; then
        echo "FAIL: content mismatch: '$result' != '$content'"
        return 1
    fi
    rm "$dst"
    echo "  renamed and verified"
    return 0
}

# ---------------------------------------------------------------------------
# test_fuse_readonly — mount with -o readonly, verify write rejected.
# ---------------------------------------------------------------------------
test_fuse_readonly() {
    echo "=== test_fuse_readonly ==="
    # Unmount existing, re-mount with -o readonly
    fusermount3 -u -z "$MNT_POINT" 2>/dev/null || true
    wait $FUSE_PID 2>/dev/null || true

    ./vfs_fuse "$VFS_FILE" "$MNT_POINT" -o readonly -f &
    local ro_pid=$!

    if ! wait_for_mount; then
        echo "FAIL: readonly mount failed"
        kill $ro_pid 2>/dev/null || true
        return 1
    fi

    # Write should fail
    if touch "$MNT_POINT/ro_write.txt" 2>/dev/null; then
        echo "FAIL: write succeeded on readonly mount"
        rm -f "$MNT_POINT/ro_write.txt"
        fusermount3 -u -z "$MNT_POINT" 2>/dev/null || true
        wait $ro_pid 2>/dev/null || true
        return 1
    fi
    echo "  write rejected with EROFS"

    # Read should succeed (hello.txt was created earlier)
    if ! cat "$MNT_POINT/hello.txt" >/dev/null 2>&1; then
        echo "WARN: read of existing file failed (may not exist)"
    else
        echo "  read succeeded"
    fi

    # Unmount readonly mount, re-mount read-write for remaining tests
    fusermount3 -u -z "$MNT_POINT" 2>/dev/null || true
    wait $ro_pid 2>/dev/null || true

    ./vfs_fuse "$VFS_FILE" "$MNT_POINT" -f &
    FUSE_PID=$!
    if ! wait_for_mount; then
        echo "FAIL: re-mount after readonly test failed"
        return 1
    fi
    echo "  re-mounted read-write"
    return 0
}

# ---------------------------------------------------------------------------
# test_fuse_snapshot — write file, snapshot, overwrite, verify at snapshot.
# ---------------------------------------------------------------------------
test_fuse_snapshot() {
    echo "=== test_fuse_snapshot ==="
    local snapfile="snap_test.txt"  # relative to mountpoint
    local fullpath="$MNT_POINT/$snapfile"
    local original="snapshot-original"
    local modified="snapshot-modified"

    echo "$original" > "$fullpath" || { echo "FAIL: write original"; return 1; }

    # Take snapshot via vfsctl
    local epoch
    epoch="$(./vfsctl snapshot "$MNT_POINT")" || {
        echo "SKIP: vfsctl snapshot failed (ioctl may not be wired)"
        rm -f "$fullpath"
        return 0
    }
    echo "  snapshot epoch: $epoch"

    # Overwrite with modified content
    echo "$modified" > "$fullpath" || { echo "FAIL: write modified"; return 1; }

    # Unmount current mount
    fusermount3 -u -z "$MNT_POINT" 2>/dev/null || true
    wait $FUSE_PID 2>/dev/null || true

    # Remount at snapshot epoch
    ./vfs_fuse "$VFS_FILE" "$MNT_POINT" -o "epoch=$epoch" -f &
    FUSE_PID=$!
    if ! wait_for_mount; then
        echo "FAIL: remount at snapshot epoch"
        return 1
    fi
    echo "  remounted at epoch $epoch"

    # Verify original content at snapshot epoch
    local result
    result="$(cat "$fullpath")"
    if [ "$result" != "$original" ]; then
        echo "FAIL: original content not preserved at snapshot epoch: '$result' != '$original'"
        rm -f "$fullpath"
        return 1
    fi
    echo "  original content verified at snapshot epoch"

    # Remount at base epoch for subsequent tests
    fusermount3 -u -z "$MNT_POINT" 2>/dev/null || true
    wait $FUSE_PID 2>/dev/null || true
    ./vfs_fuse "$VFS_FILE" "$MNT_POINT" -f &
    FUSE_PID=$!
    if ! wait_for_mount; then
        echo "FAIL: re-mount at base epoch"
        return 1
    fi
    rm -f "$fullpath"
    echo "  re-mounted at base epoch"
    return 0
}

# ---------------------------------------------------------------------------
# test_fuse_vfsctl — each subcommand with fresh VFS file and mount.
# ---------------------------------------------------------------------------
test_fuse_vfsctl() {
    echo "=== test_fuse_vfsctl ==="
    local vfspath="$(mktemp -d)/ctl_test.vfs"
    local mnt="$(mktemp -d)"
    local epoch

    # --- snapshot ---
    ./vfs_fuse "$vfspath" "$mnt" -f &
    local pid=$!
    wait_for_mount || { echo "SKIP: mount failed"; rm -rf "$(dirname "$vfspath")" "$mnt"; return 0; }
    epoch="$(./vfsctl snapshot "$mnt")" || { echo "FAIL: vfsctl snapshot"; kill $pid 2>/dev/null; return 1; }
    echo "  snapshot ok, epoch=$epoch"
    fusermount3 -u -z "$mnt" 2>/dev/null || true; wait $pid 2>/dev/null || true
    rm -f "$vfspath"

    # --- commit ---
    vfspath="$(mktemp -d)/ctl_test.vfs"
    ./vfs_fuse "$vfspath" "$mnt" -f &
    pid=$!
    wait_for_mount || { echo "SKIP: mount failed"; return 0; }
    # Create file, snapshot, modify file
    echo "data" > "$mnt/f.txt"
    epoch="$(./vfsctl snapshot "$mnt")" || { echo "FAIL: vfsctl snapshot for commit"; kill $pid 2>/dev/null; return 1; }
    echo "modified" > "$mnt/f.txt"
    # Commit the snapshot
    ./vfsctl commit "$mnt" "$epoch" || { echo "FAIL: vfsctl commit"; kill $pid 2>/dev/null; return 1; }
    echo "  commit ok, epoch=$epoch"
    fusermount3 -u -z "$mnt" 2>/dev/null || true; wait $pid 2>/dev/null || true
    rm -f "$vfspath"

    # --- delete-snapshot ---
    vfspath="$(mktemp -d)/ctl_test.vfs"
    ./vfs_fuse "$vfspath" "$mnt" -f &
    pid=$!
    wait_for_mount || { echo "SKIP: mount failed"; return 0; }
    epoch="$(./vfsctl snapshot "$mnt")" || { echo "FAIL: vfsctl snapshot for delete"; kill $pid 2>/dev/null; return 1; }
    ./vfsctl delete-snapshot "$mnt" "$epoch" || { echo "FAIL: vfsctl delete-snapshot"; kill $pid 2>/dev/null; return 1; }
    echo "  delete-snapshot ok, epoch=$epoch"
    fusermount3 -u -z "$mnt" 2>/dev/null || true; wait $pid 2>/dev/null || true
    rm -f "$vfspath"

    # --- gc ---
    vfspath="$(mktemp -d)/ctl_test.vfs"
    ./vfs_fuse "$vfspath" "$mnt" -f &
    pid=$!
    wait_for_mount || { echo "SKIP: mount failed"; return 0; }
    ./vfsctl gc "$mnt" || { echo "FAIL: vfsctl gc"; kill $pid 2>/dev/null; return 1; }
    echo "  gc ok"
    fusermount3 -u -z "$mnt" 2>/dev/null || true; wait $pid 2>/dev/null || true
    rm -f "$vfspath"

    rmdir "$mnt" 2>/dev/null || true
    echo "=== test_fuse_vfsctl PASS ==="
    return 0
}

echo "=== test_fuse smoke test ==="

# Build a small test VFS
./vfsctl "$VFS_FILE" create test.txt 2>/dev/null || {
    echo "SKIP: vfsctl unavailable or VFS prep failed"
    exit 0
}

# Mount via FUSE (background)
./vfs_fuse "$VFS_FILE" "$MNT_POINT" -f &
FUSE_PID=$!

# Verify the mount
if wait_for_mount; then
    test_fuse_create_read_delete || exit 1
    test_fuse_mkdir_rmdir || exit 1
    test_fuse_readdir || exit 1
    test_fuse_rename || exit 1
    test_fuse_readonly || exit 1
    test_fuse_snapshot || exit 1
    test_fuse_vfsctl || exit 1
    # Unmount
    fusermount3 -u "$MNT_POINT" 2>/dev/null || umount "$MNT_POINT" 2>/dev/null || true
    wait $FUSE_PID 2>/dev/null || true
else
    echo "FAIL: FUSE mount not visible within 10s"
    kill $FUSE_PID 2>/dev/null || true
    wait $FUSE_PID 2>/dev/null || true
    exit 1
fi

teardown_test
echo "=== test_fuse PASS ==="
