/* Phase 5a: Epoch stubs — replaced with real implementations in Phase 6. */
#include "epoch.h"

int64_t mapper_resolve(void* mapper, int64_t epoch) {
    (void)mapper;
    return epoch;
}

bool vfs_epoch_is_writable(void* sb, int64_t epoch, void* mapper) {
    (void)sb;
    (void)mapper;
    return true;
}

void touchedfile_add(void* vfs, int64_t epoch, uint32_t nodeId) {
    (void)vfs;
    (void)epoch;
    (void)nodeId;
}
