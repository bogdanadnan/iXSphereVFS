/* Phase 6: Epoch system — replaces Phase 5a stubs. */
#include "epoch.h"

/* Test override: when non-zero, vfs_epoch_is_writable returns this value. */
static int _test_epoch_writable = 1;

void test_set_epoch_writable(int writable) {
    _test_epoch_writable = writable;
}

/* mapper_resolve is now implemented in mapper.c — declared in mapper.h.
   The stub in this file has been removed. Callers include mapper.h directly. */

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
