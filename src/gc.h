#ifndef VFS_GC_H
#define VFS_GC_H

#include "vfs_internal.h"
#include "gc_map.h"
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

/* GC allocation cursor — tracks position in the current destination pool page
   during shadow-compaction copy.  Used by gc_walk_dirnode and other walkers. */
typedef struct {
    int64_t cur_page_vp;    /* VirtualPtr of current destination page (slot 0) */
    int     cur_slot;       /* next free slot index on current page */
    int     slots_per_page; /* VFS_POOL_ENTRIES_PER_PAGE for this context */
} GCAllocCursor;

/* Allocate a fresh pool page, initialize its pool header, and link it
 * into the pool's list.  Records the mapping in gc_map if non-NULL.
 * Returns the new page's VirtualPtr for slot 0, or VFS_VPTR_NULL on error. */
int64_t gc_allocate_new_pool_page(TreeContext* ctx, void* gc_map);

/* Copy a single 32-byte pool entry from old_slot to new_slot.
 * Updates gc_map with old_vp → new_vp mapping.
 * Remaps any VirtualPtrs within the entry by looking them up in gc_map.
 * old_vp — VirtualPtr of the slot being copied (key into the map)
 * new_vp — VirtualPtr of the destination slot (value in the map)
 * old_slot — pointer to the source slot data
 * new_slot — pointer to the destination slot data (may be NULL for compute-only) */
void gc_copy_entry(GCMap* gc_map, int64_t old_vp, int64_t new_vp,
                   const uint8_t* old_slot, uint8_t* new_slot);

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
 * GC tree walk (§12.5)
 * --------------------------------------------------------------------------- */

/* Walk a DirNode during GC shadow-compaction: copy the DirNode entry via
   gc_copy_entry, then traverse its DirContent chain applying survival rules.
   ctx     — VFS tree context (for pool, mapper access)
   gc_map  — VirtualPtr remapping hash map
   alloc   — allocation cursor for destination pool pages
   dir_vp  — VirtualPtr of the DirNode to walk
   epoch   — current live head epoch (for survival decisions) */
int gc_walk_dirnode(TreeContext* ctx, GCMap* gc_map, GCAllocCursor* alloc,
                    int64_t dir_vp, int64_t epoch);

/* Walk a FileNode during GC shadow-compaction: copy the FileNode entry,
   then walk FileContent → PageNode → VersionPage chains and FileSize chain
   applying survival rules. */
int gc_walk_filenode(TreeContext* ctx, GCMap* gc_map, GCAllocCursor* alloc,
                     int64_t file_vp, int64_t epoch);

/* Walk a VersionPage chain applying survival rules.
 * For each VersionPage in the chain starting at version_root_vp:
 *   - If epoch is soft-deleted (mapper entry with traversalApply=false): DROP
 *   - If epoch is committed (mapper entry with traversalApply=true):
 *     REWRITE epoch to the mapper's toEpoch, KEEP
 *   - Otherwise: KEEP unchanged
 * Copies surviving entries via gc_copy_entry.
 * Returns VFS_OK on success, or a negative error code. */
int gc_walk_versionpage_chain(TreeContext* ctx, GCMap* gc_map,
                               GCAllocCursor* alloc,
                               int64_t version_root_vp);

/* Walk a DirContent chain applying survival rules.
 * For each DirContent entry:
 *   - DROP if epoch belongs to deleted epoch AND no surviving entry for
 *     same childNodeId at higher epoch ≤ live head
 *   - DROP tombstone (namePtr=0) if epoch belongs to deleted epoch
 *   - KEEP otherwise
 * Copies surviving entries via gc_copy_entry.
 * Returns VFS_OK on success. */
int gc_walk_dircontent_chain(TreeContext* ctx, GCMap* gc_map,
                              GCAllocCursor* alloc,
                              int64_t head_content_vp, int64_t epoch);

/* ---------------------------------------------------------------------------
 * GC root scan — shadow-compaction (§12.5)
 *
 * Walks the pool chain, the DententryCache, the epoch mapper chain,
 * and the TouchedFile chain to build the live set, then shadow-compacts.
 * Returns VFS_OK on success. */
int vfs_gc(vfs_t* vfs);

#endif /* VFS_GC_H */
