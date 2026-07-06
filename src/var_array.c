/* var_array.c — thread-safe variable-length array (Phase 16) */

#include "var_array.h"
#include "platform.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* ---------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------------- */

/* Return the height field from a chunk or level node via the common first
 * field.  chunk→height == 0, level→height > 0. */
static int height_of(void* node) {
    return ((VarArrayChunk*)node)->height;
}

/* Return a pointer to the i-th child slot within a level node.
 * The slot table lives immediately after the VarArrayLevel struct. */
static void** slot_of(VarArrayLevel* level, int i) {
    return ((void**)(level + 1)) + i;
}

/* Allocate a chunk with inline entry storage.
 * Layout: [VarArrayChunk header][entries array: chunk_size * entry_size bytes]
 * chunk->height = 0 distinguishes it from a level node. */
static void* alloc_chunk_typed(int chunk_size, size_t entry_size) {
    VarArrayChunk* chunk = (VarArrayChunk*)calloc(1,
        sizeof(VarArrayChunk) + (size_t)chunk_size * entry_size);
    if (!chunk) return NULL;
    chunk->height  = 0;
    chunk->entries = (char*)chunk + sizeof(VarArrayChunk);
    return chunk;
}

/* Allocate a level node with inline pointer table.
 * Layout: [VarArrayLevel header][chunk_size * sizeof(void*)].
 * The slot table follows immediately after the struct. */
static void* alloc_level_typed(int chunk_size, int height) {
    VarArrayLevel* level = (VarArrayLevel*)calloc(1,
        sizeof(VarArrayLevel) + (size_t)chunk_size * sizeof(void*));
    if (!level) return NULL;
    level->height   = height;
    level->slots    = (char*)level + sizeof(VarArrayLevel);
    level->reserved = 0;
    return level;
}

/* Recursively free a chunk or level node and all its descendants.
 * Chunks (height==0) are freed directly — the entries buffer is part of
 * the same allocation.  Levels (height>0) recursively free each non-NULL
 * child slot before freeing the level itself. */
static void free_recursive(void* node, int height, int chunk_size) {
    if (!node) return;
    if (height > 0) {
        VarArrayLevel* level = (VarArrayLevel*)node;
        for (int i = 0; i < chunk_size; i++) {
            void* child = slot_of(level, i) ? *slot_of(level, i) : NULL;
            if (child) free_recursive(child, height - 1, chunk_size);
        }
    }
    free(node);
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

VarArrayBase* var_array_new_base(int entry_size, int chunk_size) {
    if (entry_size <= 0) return NULL;
    if (chunk_size < VFS_VAR_ARRAY_MIN_CHUNK_SIZE)
        chunk_size = VFS_VAR_ARRAY_MIN_CHUNK_SIZE;
    if (chunk_size > VFS_VAR_ARRAY_MAX_CHUNK_SIZE)
        chunk_size = VFS_VAR_ARRAY_MAX_CHUNK_SIZE;

    VarArrayBase* a = (VarArrayBase*)calloc(1, sizeof(VarArrayBase));
    if (!a) return NULL;
    a->root = alloc_chunk_typed(chunk_size, (size_t)entry_size);
    if (!a->root) { free(a); return NULL; }
    a->chunk_size = chunk_size;
    a->entry_size = entry_size;
    a->count      = 0;
    return a;
}

/* ---------------------------------------------------------------------------
 * grow_base — claim the next slot index, promoting root as needed.
 * Multi-level CAS promotion: atomically claims count, computes the
 * required tree height, then promotes the root from chunk to level node
 * when the current root overflows.  Thread-safe via CAS on a->root.
 * --------------------------------------------------------------------------- */

int var_array_grow_base(VarArrayBase* a) {
    if (!a) return -1;
    int cs = a->chunk_size;
    int es = a->entry_size;

    /* Atomically claim the next index.  vfs_atomic_add_i32 returns the
     * value AFTER adding, so subtract 1 for 0-based indexing (first slot
     * is idx=0, not idx=1). */
    int idx = vfs_atomic_add_i32((int32_t*)&a->count, 1) - 1;

    /* Compute required height: how many levels needed to address idx slots */
    int needed_height = 0;
    int64_t cap = cs;
    while ((int64_t)idx >= cap) {
        needed_height++;
        cap *= cs;
    }

    /* CAS loop: promote root until its height >= needed_height.
     * Multiple threads can reach this point simultaneously.  The CAS on
     * a->root serializes them: one thread wins, promotes root to the new
     * height, and continues.  Losers re-read root, free their allocated
     * level, and retry.  The height field at offset 0 lets readers
     * distinguish chunk from level without racing on count. */
    void* old_root = vfs_atomic_load_ptr((const void* const*)&a->root);
    while (height_of(old_root) < needed_height) {
        int new_height = height_of(old_root) + 1;
        void* new_level = alloc_level_typed(cs, new_height);
        if (!new_level) return -1;
        *slot_of((VarArrayLevel*)new_level, 0) = old_root;

        void* cas_result = vfs_cas_ptr(&a->root, old_root, new_level);
        if (cas_result == old_root) {
            old_root = new_level;
        } else {
            free(new_level);
            old_root = cas_result;
        }
    }

    /* Walk from root down to the leaf chunk for this index */
    void* node = vfs_atomic_load_ptr((const void* const*)&a->root);
    int h = height_of(node);
    /* Pre-compute divisor for each level: cs^level */
    int64_t div = 1;
    for (int i = 0; i < h; i++) div *= cs;
    for (int level = h; level > 0; level--, div /= cs) {
        VarArrayLevel* lv = (VarArrayLevel*)node;
        int slot = (int)(((int64_t)idx / div) % cs);
        void* child = *slot_of(lv, slot);
        if (!child) {
            if (level > 1) {
                child = alloc_level_typed(cs, level - 1);
            } else {
                child = alloc_chunk_typed(cs, (size_t)es);
            }
            if (!child) return -1;
            void** slot_ptr = slot_of(lv, slot);
            void* old = vfs_cas_ptr(slot_ptr, NULL, child);
            if (old != NULL) {
                free(child);
                child = old;
            }
        }
        node = child;
    }
    return idx;
}

/* ---------------------------------------------------------------------------
 * resolve_base — resolve slot `idx` to a pointer within a chunk.
 * Walks from root down using height_of for type dispatch.  Returns NULL
 * if idx is beyond the claimed range or any intermediate slot is empty.
 * --------------------------------------------------------------------------- */

void* var_array_resolve_base(VarArrayBase* a, int idx) {
    if (!a || idx < 0) return NULL;
    int cs = a->chunk_size;
    int es = a->entry_size;

    /* Check bounds: idx must be < total claimed count */
    if (idx >= a->count) return NULL;

    void* node = vfs_atomic_load_ptr((const void* const*)&a->root);
    if (!node) return NULL;

    int h = height_of(node);

    /* If root is a chunk (height 0), return entry directly */
    if (h == 0) {
        VarArrayChunk* chunk = (VarArrayChunk*)node;
        return (char*)chunk->entries + (size_t)(idx % cs) * es;
    }

    /* Walk from root level down to leaf chunk */
    int64_t div = 1;
    for (int i = 0; i < h; i++) div *= cs;
    for (int level = h; level > 0; level--, div /= cs) {
        VarArrayLevel* lv = (VarArrayLevel*)node;
        int slot = (int)(((int64_t)idx / div) % cs);
        node = *slot_of(lv, slot);
        if (!node) return NULL;
    }
    /* node is now the leaf chunk */
    VarArrayChunk* chunk = (VarArrayChunk*)node;
    return (char*)chunk->entries + (size_t)(idx % cs) * es;
}

/* ---------------------------------------------------------------------------
 * delete_base — free the entire VarArray tree and the base struct.
 * Idempotent (safe on NULL).  Uses free_recursive to walk and free
 * all chunks and levels.
 * --------------------------------------------------------------------------- */

void var_array_delete_base(VarArrayBase* a) {
    if (!a) return;
    if (a->root) {
        free_recursive(a->root, height_of(a->root), a->chunk_size);
    }
    free(a);
}
