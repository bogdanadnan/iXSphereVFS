/* Phase 5a: Epoch stubs — replaced with real implementations in Phase 6. */
#include "epoch.h"

/* Test override: when non-zero, vfs_epoch_is_writable returns this value.
   Zero means use default behavior (always true in stub). */
static int _test_epoch_writable = 1;

void test_set_epoch_writable(int writable) {
    _test_epoch_writable = writable;
}

int64_t mapper_resolve(void* mapper, int64_t epoch) {
    (void)mapper;
    return epoch;
}

bool vfs_epoch_is_writable(void* sb, int64_t epoch, void* mapper) {
    (void)sb;
    (void)epoch;
    (void)mapper;
    return _test_epoch_writable != 0;
}

void touchedfile_add(void* vfs, int64_t epoch, uint32_t nodeId) {
    (void)vfs;
    (void)epoch;
    (void)nodeId;
}
