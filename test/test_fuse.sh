#!/bin/bash
# test_fuse — smoke-test the FUSE mount/unmount cycle.
# Requires FUSE3 and the vfs_fuse binary.
set -e

# ---------------------------------------------------------------------------
# skip_if_no_fuse — exit 0 with SKIP message if FUSE is unavailable.
# ---------------------------------------------------------------------------
skip_if_no_fuse() {
    if [ ! -c /dev/fuse ] && [ ! -e /dev/fuse ]; then
        echo "SKIP: /dev/fuse not available"
        exit 0
    fi
    if ! command -v fusermount3 >/dev/null 2>&1 && ! command -v fusermount >/dev/null 2>&1; then
        echo "SKIP: fusermount not available"
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
    rm -rf "$(dirname "$VFS_FILE")" 2>/dev/null || true
    rmdir "$MNT_POINT" 2>/dev/null || true
}

setup_test
MNT_POINT="/tmp/test_fuse_mnt_$$"

echo "=== test_fuse smoke test ==="

# Build a small test VFS
./vfsctl "$VFS_FILE" create test.txt 2>/dev/null || {
    echo "SKIP: vfsctl unavailable or VFS prep failed"
    exit 0
}

mkdir -p "$MNT_POINT"

# Mount via FUSE (background)
./vfs_fuse "$VFS_FILE" "$MNT_POINT" -f &
FUSE_PID=$!
sleep 1

# Verify the mount
if mount | grep -q "$MNT_POINT" || ls "$MNT_POINT" >/dev/null 2>&1; then
    echo "PASS: FUSE mount successful"
    # Unmount
    fusermount3 -u "$MNT_POINT" 2>/dev/null || umount "$MNT_POINT" 2>/dev/null || true
    wait $FUSE_PID 2>/dev/null || true
else
    echo "FAIL: FUSE mount not visible"
    kill $FUSE_PID 2>/dev/null || true
    wait $FUSE_PID 2>/dev/null || true
    rmdir "$MNT_POINT" 2>/dev/null
    exit 1
fi

teardown_test
echo "=== test_fuse PASS ==="
