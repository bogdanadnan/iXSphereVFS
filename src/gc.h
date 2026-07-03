#ifndef VFS_GC_H
#define VFS_GC_H

#include "vfs_internal.h"
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * Tree Lock (§9.6)
 *
 * A single int64_t (treeLockState) serves as both a reader-writer lock
 * for tree operations and an exclusive lock for garbage collection.
 *
 *   Bit 63:  GC exclusive lock (1 = held)
 *   Bits 32-62:  Reader count (30 bits, up to ~1B concurrent readers)
 *   Bits 0-31:  Unused / reserved
 *
 * All operations are lock-free (CAS-based).  Readers spin on exclusive
 * lock; writers CAS the exclusive bit, then spin-wait for readers to drain.
 * --------------------------------------------------------------------------- */

#define TREE_LOCK_EXCLUSIVE_BIT (1ULL << 63)
#define TREE_LOCK_READER_MASK 0x7FFFFFFF00000000ULL
#define TREE_LOCK_READER_INC  0x0000000100000000ULL

/* Acquire the shared (reader) lock — spin until exclusive is released. */
void tree_lock_acquire_shared(TreeContext* ctx);

/* Release the shared (reader) lock. */
void tree_lock_release_shared(TreeContext* ctx);

/* Acquire the exclusive (GC) lock — spin until all readers drain. */
void tree_lock_acquire_exclusive(TreeContext* ctx);

/* Release the exclusive (GC) lock. */
void tree_lock_release_exclusive(TreeContext* ctx);

/* ---------------------------------------------------------------------------
 * Deferred Free Queue (§7.3)
 *
 * During GC, we cannot immediately free pages that might still be referenced
 * by in-flight readers.  Instead, we enqueue them here.  Once GC confirms
 * that no active readers remain, the pages are actually released.
 * --------------------------------------------------------------------------- */

typedef struct DeferredFreeQueue {
    int64_t* pages;      /* dynamic array of logical page indices */
    int      count;
    int      capacity;
    bool     confirmed;  /* true when no active traversals remain */
} DeferredFreeQueue;

/* Initialize a deferred free queue with the given initial capacity.
 * Allocates the pages array via malloc.  Returns VFS_OK on success. */
int deferred_free_init(DeferredFreeQueue* queue, int initial_capacity);

/* Enqueue a logical page for deferred freeing.
 * If the page has a mirror sibling (via StorageBackend), also enqueues
 * the sibling so both are freed together. */
void deferred_free_enqueue(DeferredFreeQueue* queue, int64_t logical_page,
                            StorageBackend* sb);

/* Check whether a logical page is in the deferred-free queue.
 * Returns true if the page is enqueued (and not yet confirmed+released). */
bool deferred_free_is_queued(DeferredFreeQueue* queue, int64_t logical_page);

/* Confirm that no active readers remain and release all queued pages
 * back to the StorageBackend.  After this call, the queue is empty. */
void deferred_free_confirm_and_release(DeferredFreeQueue* queue,
                                        StorageBackend* sb);

/* Destroy the queue — free the pages array without releasing pages.
 * For cleanup when GC is aborted or the VFS is closing. */
void deferred_free_destroy(DeferredFreeQueue* queue);

/* ---------------------------------------------------------------------------
 * GC root scan — shadow-compaction (§12.5)
 *
 * Walks the pool chain, the DententryCache, the epoch mapper chain,
 * and the TouchedFile chain to build the live set, then shadow-compacts.
 * Returns VFS_OK on success. */
int vfs_gc(vfs_t* vfs);

#endif /* VFS_GC_H */
