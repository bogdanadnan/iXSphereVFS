# Phase 16: Variable Lock-Free Array (VarArray)

## Goal
A generic, variable-size, lock-free array. Fixed chunk size (`chunk_size`,
configurable at init). Growth is lazy: each level of indirection is allocated
only when the previous level is exhausted. No upper bound except RAM.

For ≤ `chunk_size` entries: one pointer dereference — same as a native array.
For > `chunk_size`: two dereferences. Each 256× overshoot adds one more level.

Used as the foundation for the directory index (Phase 17) and any future
structure needing a growable, concurrent-safe array.

## Data Structure

### Type-Agnostic Core

VarArray operates on opaque chunks. The core knows about allocation, growth,
and indexing. It never reads or writes entry contents.

```c
typedef struct VarArray {
    void*         root;       // VarArrayChunk* or VarArrayLevel*
    int           chunk_size; // configurable, default 256
    int           entry_size; // sizeof(entry type), set at init
    volatile int  count;      // atomic: total entries
} VarArray;
```

### Chunk (internal to core)

```c
typedef struct {
    void* entries;  // malloc'd: chunk_size * entry_size bytes
} VarArrayChunk;
```

The core does `chunk->entries[idx * entry_size]` via pointer arithmetic.
Never knows or cares about the entry type.

### Type-Safe Header (the macro layer)

Following the article's pattern: define a BASE struct for the chunk, then
a macro that creates a typed alias with the SAME LAYOUT.

```c
// var_array.h — base definitions

#define VARRAY_DEFINE_CHUNK(T, suffix) \
    typedef struct { T* entries; } VarArrayChunk_##suffix;

#define VARRAY_CHUNK(T, suffix) VarArrayChunk_##suffix
```

Usage:
```c
// dir_index.c
typedef struct { uint64_t key; int64_t vp; } DirEntry;

VARRAY_DEFINE_CHUNK(DirEntry, dir)
// Expands to: typedef struct { DirEntry* entries; } VarArrayChunk_dir;

// Now VarArrayChunk_dir is LAYOUT-COMPATIBLE with VarArrayChunk (both have void*/T* at offset 0)
```

The function `var_array_insert(VarArray*, void* entry)` memcpy's `entry_size` bytes
into the chunk's entries buffer at the right offset. Lookup returns `void*` to the
byte offset — the macro wraps it into a typed pointer.

### Type-Safe Insert/Lookup Macros

```c
#define var_array_insert_typed(arr, entry_ptr) \
    var_array_insert((arr), (const void*)(entry_ptr))

#define var_array_lookup_typed(arr, index, T) \
    ((T*)var_array_lookup((arr), (index)))
```

All the type information is in the SIZE (`entry_size` in `VarArray`). The
macros are just casts for safety — the core already handles any entry type
via `memcpy` and pointer arithmetic.

### Chunk

A fixed-size array of entries. `root` points directly to one of these when
`count ≤ chunk_size`.

```c
typedef struct {
    VarArrayEntry* entries;  // malloc'd: chunk_size * sizeof(VarArrayEntry)
    int            capacity; // chunk_size (always the configured value)
} VarArrayChunk;
```

### Level Node

A pointer table of `chunk_size` slots. Height N: slots point to height N-1
nodes (for N > 1) or to chunks (for N = 1).

```c
typedef struct VarArrayLevel {
    void*          slots;    // malloc'd: chunk_size * sizeof(void*)
    int            height;   // 1 = slots point to chunks, N = slots point to height N-1
    int            chunk_size;
} VarArrayLevel;
```

### VarArray

```c
typedef struct {
    void*         root;       // VarArrayChunk* or VarArrayLevel*
    int           chunk_size; // configurable, default 256
    volatile int  count;      // atomic: total entries inserted
} VarArray;
```

## Growth Algorithm

```
INSERT(array, entry):
    idx = atomic_fetch_add(&array->count, 1)
    cap = chunk_size

    if idx < cap:
        // Level 0: root IS the chunk
        if array->root == NULL:
            chunk = calloc_chunk()
            CAS(&array->root, NULL, chunk)
        chunk = (VarArrayChunk*)array->root
        chunk->entries[idx] = entry
        return

    // Need level 1.  If root is still a chunk, promote it.
    if current root is a chunk:
        old_chunk = (VarArrayChunk*)array->root
        new_level = calloc_level(height=1)
        new_level->slots[0] = old_chunk        // slot 0 gets the old data
        // Allocate the chunk for the new slot
        target_slot = idx / chunk_size
        new_chunk = calloc_chunk()
        new_level->slots[target_slot] = new_chunk
        // Atomic swap: old root → new level
        if CAS(&array->root, old_chunk, new_level) succeeds:
            chunk = new_chunk
            chunk->entries[idx % chunk_size] = entry
            return
        else:
            // CAS failed — another thread promoted.  Retry from top.
            idx = count - 1  // re-fetch?  No: idx is already assigned.
            goto retry

    // Root is already a level.  Walk down.
    node = (VarArrayLevel*)array->root

    // Find or create the path
    remaining = idx
    while node->height > 1:
        stride = chunk_size^node->height
        slot = remaining / stride
        remaining = remaining % stride
        if node->slots[slot] == NULL:
            new_child = calloc_level(height = node->height - 1)
            CAS(&node->slots[slot], NULL, new_child)
        node = node->slots[slot]

    // height == 1: slots point to chunks
    slot = remaining / chunk_size
    entry_idx = remaining % chunk_size
    if node->slots[slot] == NULL:
        new_chunk = calloc_chunk()
        CAS(&node->slots[slot], NULL, new_chunk)
    chunk = (VarArrayChunk*)node->slots[slot]
    chunk->entries[entry_idx] = entry

  retry:
    // Re-read root (may have been promoted by another thread) and try again
```

### Root Promotion

When `idx ≥ chunk_size` and root is still a chunk, promote:

1. Allocate a new level node (height=1, chunk_size slots)
2. Set `new_level->slots[0] = old_chunk` (existing entries preserved)
3. Allocate the chunk needed for the new slot
4. Set `new_level->slots[target_slot] = new_chunk`
5. CAS `array->root` from `old_chunk` to `new_level`

If CAS fails, another thread already promoted. Release our allocations
(leak — GC'd later or accepted leak). Re-read `array->root` and retry the
walk from the new root.

A thread observing the CAS mid-flight sees:
- Before CAS: `root = old_chunk` — valid, all entries accessible
- After CAS: `root = new_level` — all old entries at slot 0, new entry accessible

No thread sees a half-built state. The new level is fully initialized before
the CAS publishes it.

### Higher-Level Growth

Same pattern: when a level is full (all `chunk_size` slots occupied), the
parent (or root) allocates a sibling. When the root level itself is full,
a new root with `height = old_root->height + 1` is allocated, old root
becomes slot 0, CAS-root.

## Lookup

```
LOOKUP(array, index):
    if index >= array->count: return NULL

    root = array->root   // atomic snapshot
    if root is a chunk:
        return &((VarArrayChunk*)root)->entries[index]

    node = (VarArrayLevel*)root
    remaining = index
    while node->height > 1:
        stride = chunk_size^node->height
        slot = remaining / stride
        remaining = remaining % stride
        if node->slots[slot] == NULL: return NULL
        node = node->slots[slot]

    // height == 1
    slot = remaining / chunk_size
    entry_idx = remaining % chunk_size
    chunk = node->slots[slot]
    if chunk == NULL: return NULL
    return &chunk->entries[entry_idx]
```

## Remove

Set `entry->value = VFS_VPTR_NULL`. Entry stays in place — count never
decremented. The slot remains occupied for indexing.

## Concurrency

- **`atomic_fetch_add(&count, 1)`**: unique index per writer. No two writers
  write to the same slot.
- **CAS on root**: publishes promotion atomically. Losers retry.
- **CAS on slot pointers**: allocates chunks/levels lazily. Losers retry or
  accept the winner's allocation.
- **Lock-free readers**: walk the pointer chain. Null slots mean "not yet
  allocated" → entry doesn't exist. No locks, no retry loops.

## Non-Negotiable Constraints

- **No locks.** CAS only.
- **No realloc.** Chunks and levels are fixed-size, never resized.
- **No copy-on-grow.** Old entries stay in place during promotion.
- **Lazy allocation.** Each level/chunk allocated only when needed.
- **No upper bound.** Grows until RAM exhausted.
- **Configurable `chunk_size`.** Passed at init. Used for ALL math.
- **Single indirection for ≤ chunk_size entries.** `root` directly points to a chunk.

## Files

| File | Purpose |
|------|---------|
| `src/var_array.c` | VarArray implementation |
| `src/var_array.h` | Public API |
| `test/test_var_array.c` | Tests |

## API

```c
// Core (type-agnostic, operates on raw bytes)
void  var_array_init(VarArray* arr, int chunk_size, int entry_size);
void  var_array_destroy(VarArray* arr);
int   var_array_insert(VarArray* arr, const void* entry);  // returns index
void* var_array_lookup(VarArray* arr, int index);          // returns pointer to entry bytes
void  var_array_remove(VarArray* arr, int index);          // zeroes the entry
int   var_array_count(VarArray* arr);
```

### Usage Example

```c
// Define entry type
typedef struct { uint64_t key; int64_t vp; } DirEntry;

// Define layout-compatible chunk type
VARRAY_DEFINE_CHUNK(DirEntry, dir)

void example(void) {
    VarArray va;
    var_array_init(&va, 256, sizeof(DirEntry));

    DirEntry e = { .key = hash("foo"), .vp = 0x12345 };
    int idx = var_array_insert(&va, &e);

    DirEntry* found = (DirEntry*)var_array_lookup(&va, idx);
    printf("key=%llu vp=%lld\n", found->key, found->vp);

    var_array_destroy(&va);
}
```

## Acceptance

- [ ] Insert 10 entries (chunk_size=256): root is chunk, O(1) lookup
- [ ] Insert 256 entries: chunk full, root still chunk
- [ ] Insert 257 entries: root promoted to level(height=1), old chunk at slot 0
- [ ] Insert 10,000 entries: all retrievable by index
- [ ] Concurrent insert from 4 threads: all entries present, no duplicates
- [ ] Lookup on out-of-range returns NULL, no crash
- [ ] Remove sets value to VFS_VPTR_NULL
- [ ] Custom chunk_size=64 works correctly
- [ ] All tests pass
