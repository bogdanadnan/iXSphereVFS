/* Phase 6: Epoch system — replaces Phase 5a stubs. */
#include "epoch.h"
#include "vfs_internal.h"

/* Test override: -1 = use real implementation (Phase 6 default).
   0 = frozen, 1 = all writable (backward compat for existing tests). */
static int _test_epoch_writable = 1;

void test_set_epoch_writable(int writable) {
    _test_epoch_writable = writable;
}

bool vfs_epoch_is_writable(TreeContext* ctx, int64_t epoch) {
    /* Test override: if set to 0 or 1, use that value directly */
    if (_test_epoch_writable >= 0)
        return _test_epoch_writable != 0;

    /* epoch == -1 means current live head */
    if (epoch == -1) epoch = ctx->currentEpoch;

    /* Live head (current even epoch) is always writable */
    if (epoch == ctx->currentEpoch) return true;

    /* Odd epoch (snapshot): writable if NOT in the mapper chain.
       Being in the mapper means it was committed or soft-deleted. */
    if (epoch % 2 == 1) {
        int64_t resolved = mapper_resolve(&ctx->mapper, epoch);
        return resolved == epoch;
    }

    /* Even epoch that isn't currentEpoch → not writable (frozen past) */
    return false;
}

void touchedfile_add(void* vfs, int64_t epoch, uint32_t nodeId) {
    (void)vfs;
    (void)epoch;
    (void)nodeId;
}
