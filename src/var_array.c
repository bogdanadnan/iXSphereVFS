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
    a->count      = 0;
    return a;
}
