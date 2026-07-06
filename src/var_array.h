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
 * VarArrayChunk — leaf node holding the actual entries.
 *
 * height  — always 0; distinguishes a chunk (height == 0) from a level
 *           node (height > 0).  Must be the first field so that casting
 *           a void* to VarArrayChunk* and reading height works for both.
 * entries — pointer to the malloc'd entry array.
 *
 * Layout: 4 bytes height, 4 bytes padding, 8 bytes entries = 16 bytes.
 * --------------------------------------------------------------------------- */
typedef struct {
    volatile int height;
    void*         entries;
} VarArrayChunk;

/* ---------------------------------------------------------------------------
 * VarArrayLevel — internal node holding pointers to children.
 *
 * height   — always > 0; distinguishes a level from a chunk (height == 0).
 *            Must be the first field for type-dispatch via void* cast.
 * slots    — points to a chunk_size-element pointer table located immediately
 *            after this struct in the same malloc'd block.
 * reserved — padding for 8-byte alignment.
 *
 * Layout: 4 bytes height, 4 bytes reserved, 8 bytes slots = 16 bytes.
 *          The pointer table (chunk_size * 8 bytes) follows immediately.
 * --------------------------------------------------------------------------- */
typedef struct {
    volatile int height;
    int           reserved;
    void*         slots;
} VarArrayLevel;

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

/* ---------------------------------------------------------------------------
 * Base (untyped) API — operates on void* slots, caller handles element size.
 * --------------------------------------------------------------------------- */

/* Allocate a new variable-length array.  entry_size is the byte size of
 * each element.  Returns NULL on allocation failure. */
VarArrayBase* var_array_new_base(int entry_size, int chunk_size);

/* Free all memory associated with the array.  Idempotent (safe on NULL). */
void var_array_delete_base(VarArrayBase* a);

/* Claim the next slot index.  Returns the claimed index (0, 1, 2, ...)
 * on success, or a negative value on error. */
int var_array_grow_base(VarArrayBase* a);

/* Resolve slot `idx` to a pointer within a chunk.  Returns a pointer to
 * the slot's entry data, or NULL if idx is beyond the claimed range.
 * The returned pointer is valid until the next grow. */
void* var_array_resolve_base(VarArrayBase* a, int idx);

/* ---------------------------------------------------------------------------
 * Typed convenience macro — layout-compatible with VarArrayBase* because
 * the first three members (root pointer, chunk_size, count) have identical
 * layout regardless of T.  Usage:
 *
 *   VarArray(int)*  arr = (VarArray(int)*)var_array_new_base(sizeof(int), 256);
 *   int idx = var_array_grow_base((VarArrayBase*)arr);
 *   int* slot = var_array_resolve_base((VarArrayBase*)arr, idx);
 * --------------------------------------------------------------------------- */
#define VarArray(T) struct { T* root; int chunk_size; volatile int count; }*

/* Typed accessor for chunk entries — layout-compatible with VarArrayChunk*
 * because height is the first field and entries is the second. */
#define VarArrayChunk_T(T) struct { volatile int height; T* entries; }

/* Typed accessor for level nodes — layout-compatible with VarArrayLevel*
 * because fields mirror its exact order: height, reserved, slots.
 * For documentation/typing only — actual level walks use base types. */
#define VarArrayLevel_T(T) struct { volatile int height; int reserved; VarArrayChunk_T(T)** slots; }

/* Typed convenience wrapper: allocate a new VarArray with default chunk size.
 * Casts the VarArrayBase* return to the typed VarArray(T)* pointer. */
#define var_array_new(T) \
    ((VarArray(T))var_array_new_base(sizeof(T), VFS_VAR_ARRAY_DEFAULT_CHUNK_SIZE))

#endif /* VFS_VAR_ARRAY_H */
