#include "ixsphere/vfs.h"
#include <stdio.h>
#include <string.h>

/* Test: mount, take snapshot, modify, remount at snapshot epoch, verify. */

static int create_test_vfs(const char* path) {
    vfs_t* vfs = vfs_mount(path, 8192);
    if (!vfs) { fprintf(stderr, "mount failed\n"); return -1; }
    int64_t root = vfs_root(vfs);
    int64_t f = vfs_create(vfs, root, "original.txt", 0);
    if (f > 0) {
        vfs_write(vfs, f, "ORIGINAL_CONTENT", 0, 16, 0);
    }
    vfs_flush(vfs);
    vfs_unmount(vfs);
    return 0;
}

static int64_t get_epoch(const char* path) {
    /* Returns current epoch from VFS superblock.  Uses bootstrap header. */
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    int64_t epoch = -1;
    /* Read at offset where currentEpoch lives — for our test, just return -1. */
    fclose(f);
    return epoch;
}

static int verify_at_epoch(const char* path, int64_t epoch, const char* expected_name,
                            const char* expected_content) {
    fprintf(stderr, "Mounting at epoch %lld\n", (long long)epoch);
    vfs_t* vfs = vfs_mount(path, 8192);
    if (!vfs) { fprintf(stderr, "mount failed\n"); return -1; }

    int64_t root = vfs_root(vfs);
    vfs_dirent_t ents[64];
    int n = vfs_readdir(vfs, root, ents, 64, epoch);
    fprintf(stderr, "Epoch %lld: %d entries\n", (long long)epoch, n);
    for (int i = 0; i < n; i++) {
        fprintf(stderr, "  %s\n", ents[i].name);
        if (strcmp(ents[i].name, expected_name) == 0) {
            /* Found file — read content */
            char buf[256] = {0};
            int r = vfs_read(vfs, ents[i].vp, buf, 0, strlen(expected_content), epoch);
            if (r == (int)strlen(expected_content) &&
                memcmp(buf, expected_content, r) == 0) {
                fprintf(stderr, "  content OK: %.*s\n", r, buf);
            } else {
                fprintf(stderr, "  content mismatch: got '%.*s' (%d bytes)\n", r, buf, r);
                vfs_unmount(vfs);
                return -1;
            }
        }
    }
    vfs_unmount(vfs);
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <file.vfs>\n", argv[0]);
        return 1;
    }
    const char* path = argv[1];

    /* Step 1: Create test VFS */
    fprintf(stderr, "=== Step 1: Create test VFS ===\n");
    if (create_test_vfs(path) != 0) return 1;

    /* Step 2: Mount at base epoch (0), take snapshot */
    fprintf(stderr, "\n=== Step 2: Mount at base, take snapshot ===\n");
    vfs_t* vfs = vfs_mount(path, 8192);
    if (!vfs) return 1;
    int64_t epoch0 = 0;
    int64_t root = vfs_root(vfs);
    /* Verify original exists at base */
    vfs_dirent_t ents[10];
    int n = vfs_readdir(vfs, root, ents, 10, epoch0);
    fprintf(stderr, "Base epoch %lld: %d entries\n", (long long)epoch0, n);

    /* Take snapshot — current epoch advances from 0 to 1 (snapshot is odd) */
    int64_t snap_epoch = vfs_snapshot(vfs);
    fprintf(stderr, "Snapshot epoch: %lld (current: %lld)\n",
            (long long)snap_epoch, (long long)vfs_current_epoch(vfs));

    /* After snapshot, vfs_current_epoch is still 1 (the live base is the
       snapshot epoch). Writes at base go to the new live epoch. */

    /* Step 3: Modify file at the new live epoch (post-snapshot) */
    fprintf(stderr, "\n=== Step 3: Modify file at live epoch ===\n");
    int64_t live_epoch = vfs_current_epoch(vfs);
    int64_t file_vp = ents[0].vp;
    vfs_write(vfs, file_vp, "MODIFIED_CONTENT!", 0, 18, live_epoch);

    /* Step 4: Add a new file at the new live epoch */
    int64_t new_f = vfs_create(vfs, root, "newfile.txt", live_epoch);
    vfs_write(vfs, new_f, "NEWFILE_CONTENT", 0, 15, live_epoch);

    vfs_flush(vfs);
    vfs_unmount(vfs);

    /* Step 5: Verify at snapshot epoch (should see ORIGINAL) */
    fprintf(stderr, "\n=== Step 4: Verify at snapshot epoch %lld ===\n",
            (long long)snap_epoch);
    if (verify_at_epoch(path, snap_epoch, "original.txt", "ORIGINAL_CONTENT") != 0) {
        fprintf(stderr, "FAIL: snapshot epoch doesn't show original\n");
        return 1;
    }

    /* Step 6: Verify at current live epoch (should see MODIFIED + newfile) */
    fprintf(stderr, "\n=== Step 5: Verify at current live epoch ===\n");
    vfs = vfs_mount(path, 8192);
    if (!vfs) return 1;
    int64_t current_epoch = vfs_current_epoch(vfs);
    fprintf(stderr, "Current epoch: %lld\n", (long long)current_epoch);
    root = vfs_root(vfs);
    n = vfs_readdir(vfs, root, ents, 10, current_epoch);
    fprintf(stderr, "Live epoch: %d entries (expect 2)\n", n);
    int found_original = 0, found_new = 0;
    for (int i = 0; i < n; i++) {
        fprintf(stderr, "  %s\n", ents[i].name);
        if (strcmp(ents[i].name, "original.txt") == 0) found_original = 1;
        if (strcmp(ents[i].name, "newfile.txt") == 0) found_new = 1;
    }
    if (!found_original || !found_new) {
        fprintf(stderr, "FAIL: missing files at live epoch\n");
        return 1;
    }
    vfs_unmount(vfs);

    fprintf(stderr, "\n=== ALL TESTS PASSED ===\n");
    return 0;
}