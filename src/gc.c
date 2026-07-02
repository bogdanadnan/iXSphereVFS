/* Phase 7: GC — Tree Lock, Deferred Free Queue */
#include "gc.h"
#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * Tree Lock (§9.6)
 *
 * treeLockState bit layout:
 *   Bit 63:     GC exclusive lock (1 = held)
 *   Bits 32-62: Reader count (30 bits)
 *   Bits 0-31:  Unused
 * --------------------------------------------------------------------------- */

void tree_lock_acquire_shared(TreeContext* ctx) {
    for (;;) {
        int64_t old = vfs_atomic_load_i64(&ctx->treeLockState);
        /* Spin while exclusive lock is held (bit 63 set) */
        while (old & TREE_LOCK_EXCLUSIVE_BIT) {
            old = vfs_atomic_load_i64(&ctx->treeLockState);
        }
        /* Try to CAS-increment the reader count */
        int64_t desired = old + TREE_LOCK_READER_INC;
        int64_t cas = vfs_cas_i64(&ctx->treeLockState, old, desired);
        if (cas == old) return;  /* success */
        /* CAS failed — retry */
    }
}

void tree_lock_release_shared(TreeContext* ctx) {
    /* Atomically decrement reader count by one reader unit */
    vfs_atomic_add_i64(&ctx->treeLockState, -((int64_t)TREE_LOCK_READER_INC));
}

void tree_lock_acquire_exclusive(TreeContext* ctx) {
    for (;;) {
        int64_t old = vfs_atomic_load_i64(&ctx->treeLockState);
        /* Spin while exclusive is held (another GC is running) */
        while (old & TREE_LOCK_EXCLUSIVE_BIT) {
            old = vfs_atomic_load_i64(&ctx->treeLockState);
        }
        /* Try to CAS-set the exclusive bit */
        int64_t desired = old | (int64_t)TREE_LOCK_EXCLUSIVE_BIT;
        int64_t cas = vfs_cas_i64(&ctx->treeLockState, old, desired);
        if (cas == old) break;  /* exclusive bit acquired */
        /* CAS failed — retry */
    }

    /* Spin-wait for readers to drain (reader count goes to 0) */
    for (;;) {
        int64_t state = vfs_atomic_load_i64(&ctx->treeLockState);
        /* Keep only the reader bits, check if zero */
        int64_t readers = state & (int64_t)TREE_LOCK_READER_MASK;
        if (readers == 0) return;
        /* Spin */
    }
}

void tree_lock_release_exclusive(TreeContext* ctx) {
    /* Release barrier ensures all writes performed under exclusive lock
       are globally visible before the lock appears released. */
    vfs_mb_release();
    vfs_atomic_add_i64(&ctx->treeLockState,
                       -((int64_t)TREE_LOCK_EXCLUSIVE_BIT));
}
