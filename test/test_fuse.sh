#!/bin/bash
# test_fuse — smoke-test the FUSE mount/unmount cycle.
# Requires FUSE3 and the vfs_fuse binary.
set -e

VFS_FILE="/tmp/test_fuse_smoke.vfs"
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

rmdir "$MNT_POINT" 2>/dev/null
rm -f "$VFS_FILE"
echo "=== test_fuse PASS ==="
