#ifndef VFS_VAR_ARRAY_H
#define VFS_VAR_ARRAY_H

/* GNU extensions required: uses statement expressions ({ ... }) and
 * typeof().  Only include in translation units compiled with
 * -Wno-pedantic -fno-strict-aliasing. */

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
    int            entry_size;  /* byte size of each element */
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

/* Write a value at slot `idx`, allocating the tree path lazily.  Used
   by the var_array_set macro (writes value) and var_array_unset macro
   (passes NULL value to memset the slot to 0).  On success, the slot
   at `idx` is allocated and the value is written (or cleared).  Updates
   count to max(count, idx+1) via CAS.  Thread-safe on different idxs;
   concurrent writes to the same idx must be serialized by the caller.
   Returns 0 on success, -1 on OOM or invalid args. */
int var_array_set_base(VarArrayBase* a, int idx, const void* value);

/* Resolve slot `idx` to a pointer within a chunk.  Returns a pointer to
 * the slot's entry data, or NULL if idx is beyond the claimed range.
 * The returned pointer is valid until the next grow. */
void* var_array_resolve_base(VarArrayBase* a, int idx);

#ifdef VFS_VAR_ARRAY_TESTING
int var_array_root_height_for_test(VarArrayBase* a);
#endif

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
 * Casts the VarArrayBase* return to the typed VarArray(T)* pointer.
 * NOTE: -Wno-incompatible-pointer-types required (each VarArray(T)
 * expansion creates a distinct anonymous struct). */
#define var_array_new(T) \
    ((VarArray(T))var_array_new_base(sizeof(T), VFS_VAR_ARRAY_DEFAULT_CHUNK_SIZE))

/* Typed convenience wrapper: free a VarArray.  Casts to VarArrayBase*. */
#define var_array_delete(a) var_array_delete_base((VarArrayBase*)(a))

/* Append an entry to a typed VarArray.  Claims a slot via grow_base,
 * resolves it via resolve_base, and writes the entry into the chunk.
 *
 * NOTE: the fast path (direct chunk write via root cast) has been
 * intentionally removed — it races with root promotion: another thread
 * can promote root from a chunk to a level between grow_base returning
 * and this macro reading root.  The resolve_base call handles this
 * safely by re-walking the tree from the current root. */
#define var_array_append(a, entry) ({ \
    int _idx = var_array_grow_base((VarArrayBase*)(a)); \
    void* _rp = var_array_resolve_base((VarArrayBase*)(a), _idx); \
    if (_rp) ((VarArrayChunk_T(typeof(entry))*)_rp)->entries[_idx % (a)->chunk_size] = (entry); \
    _idx; \
})

/* Set an entry at index `idx`.  Allocates the tree path for `idx` if
 * needed (sparse array), then writes the value.  Always succeeds modulo
 * OOM — does NOT silently drop on missing slots (replaces the old
 * var_array_update macro which did silent-drop).
 *
 * Uses a statement expression to handle rvalue entries (e.g., literals):
 * we copy the entry into a local typed variable to take its address. */
#define var_array_set(a, idx, entry) ({ \
    int _idx = (idx); \
    typeof(entry) _entry = (entry); \
    var_array_set_base((VarArrayBase*)(a), _idx, &_entry); \
})

/* Clear the slot at index `idx` to zero.  If the slot doesn't exist
 * (chunk not allocated), this is a no-op — unset does NOT allocate
 * the path.  Use var_array_set(arr, idx, value) if you want to allocate
 * and write; use var_array_unset(arr, idx) if you only want to clear
 * an already-existing slot. */
#define var_array_unset(a, idx) ({ \
    VarArrayBase* _ab = (VarArrayBase*)(a); \
    void* _chunk = var_array_resolve_base(_ab, (idx)); \
    if (_chunk) { \
        VarArrayChunk_T(typeof(*(a)->root)) *_c = \
            (VarArrayChunk_T(typeof(*(a)->root))*)_chunk; \
        memset(&_c->entries[(idx) % _ab->chunk_size], 0, _ab->entry_size); \
    } \
})

/* Look up an entry by index.  Returns a pointer to the element, or NULL
 * if idx is out of range or the slot was never written. */
#define var_array_lookup(a, idx) ({ \
    void* _rp = var_array_resolve_base((VarArrayBase*)(a), (idx)); \
    _rp ? &((VarArrayChunk_T(typeof(*(a)->root))*)_rp)->entries[(idx) % (a)->chunk_size] : NULL; \
})

#endif /* VFS_VAR_ARRAY_H */
