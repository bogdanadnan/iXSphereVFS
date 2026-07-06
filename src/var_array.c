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
