#ifndef VFS_VAR_ARRAY_H
#define VFS_VAR_ARRAY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Chunk size defines — the branching factor at every level of the tree.
 * DEFAULT: 256 entries per chunk (good cacheline trade-off, ~2KB)
 * MIN:     16  entries per chunk (tight minimum, ~128B)
 * MAX:     4096 entries per chunk (large-dataset ceiling, ~32KB) */
#define VFS_VAR_ARRAY_DEFAULT_CHUNK_SIZE  256
#define VFS_VAR_ARRAY_MIN_CHUNK_SIZE       16
#define VFS_VAR_ARRAY_MAX_CHUNK_SIZE     4096

/* ---------------------------------------------------------------------------
 * VarArrayBase — compact, fixed-size header for a variable-length array.
 *
 * root       — void* to the topmost chunk (height == 0) or level node
 *              (height > 0).  Cast to VarArrayChunk* or VarArrayLevel*
 *              after reading the chunk's height field.
 * chunk_size — branching factor at every height; set at init, never changed.
 * count      — total slots ever claimed via va_claim_slot; monotonically
 *              increasing, never decremented.
 *
 * Size: 16 bytes on 64-bit, 12 bytes on 32-bit.
 * --------------------------------------------------------------------------- */
typedef struct {
    void*          root;
    int            chunk_size;
    volatile int   count;
} VarArrayBase;

#endif /* VFS_VAR_ARRAY_H */
