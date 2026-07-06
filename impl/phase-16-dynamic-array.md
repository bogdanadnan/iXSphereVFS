# Phase 16: Variable Lock-Free Array (VarArray)

## Goal
A generic, variable-size, lock-free array. Typed via macros using the
layout-compatible struct pattern from C template idioms. Resizes via
pointer-table indirection — no realloc, no copy-on-grow. Always O(1)
lookup after the initial root cast. Supports any value type.

## Pattern

From the article: define a base struct with `void*` fields, a typed struct
with `T*` fields (same binary layout), and macros that accept the typed
struct while calling functions that operate on the base.

```c
// base struct (layout-compatible with any typed version)
typedef struct {
    void* root;
    int   chunk_size;
    volatile int count;
} VarArrayBase;

// typed struct — same layout, typed buffer
#define VarArray(T) struct { T* root; int chunk_size; volatile int count; }*
```

`VarArray(int)` produces an anonymous struct with `int* root`. It has the
same memory layout as `VarArrayBase` (both start with a pointer + two ints).
A `VarArray(int)` can be passed to any function expecting `VarArrayBase*`
— no cast needed, just a warning-suppressible implicit conversion.

## Data Structure

### Chunk

```c
// base
typedef struct { void* entries; } VarArrayChunk;

// typed (macro-generated)
#define VarArrayChunk_T(T) struct { T* entries; }
```

### Level Node

```c
// base
typedef struct { void* slots; int height; } VarArrayLevel;

// typed
#define VarArrayLevel_T(T) struct { VarArrayChunk_T(T)** slots; int height; }
```

## API

All functions take `VarArrayBase*`. Macros wrap them for typed access.

```c
// --- Core (var_array.h) ---

VarArrayBase* var_array_new_base(int chunk_size);
void          var_array_delete_base(VarArrayBase* a);
int           var_array_grow_base(VarArrayBase* a);  // returns index of new slot
void*         var_array_resolve_base(VarArrayBase* a, int idx); // returns pointer to slot

// --- Typed macros (same header) ---

#define VarArray(T) struct { T* root; int chunk_size; volatile int count; }*

#define var_array_new(T) \
    ((VarArray(T))var_array_new_base(sizeof(T)))

#define var_array_delete(a) var_array_delete_base((VarArrayBase*)(a))

#define var_array_insert(a, entry) ({                      \
    int _idx = var_array_grow_base((VarArrayBase*)(a));     \
    typeof((a)->root) _r = (a)->root;                       \
    if (_idx < (a)->chunk_size)                             \
        _r->entries[_idx] = entry;                          \
    else                                                    \
        _r = var_array_resolve_base((VarArrayBase*)(a), _idx); \
        _r->entries[_idx % (a)->chunk_size] = entry;        \
    _idx;                                                   \
})

#define var_array_lookup(a, idx) ({                         \
    typeof((a)->root) _r = (idx < (a)->chunk_size)          \
        ? (a)->root                                         \
        : var_array_resolve_base((VarArrayBase*)(a), idx);  \
    _r ? &_r->entries[(idx) % (a)->chunk_size] : NULL;      \
})
```

## Usage

```c
typedef struct { uint64_t key; int64_t vp; } DirEntry;

VarArray(DirEntry) list = var_array_new(DirEntry);

DirEntry e = { .key = hash("foo"), .vp = 0x12345 };
int idx = var_array_insert(list, e);

DirEntry* found = var_array_lookup(list, idx);
printf("key=%llu vp=%lld\n", found->key, found->vp);

var_array_delete(list);
```

No casts anywhere. `list` is `VarArray(DirEntry)` — a typed handle. The
macros access `list->root`, `list->chunk_size` directly through the
layout-compatible struct. The compiler knows `sizeof(DirEntry)` because
the root pointer is `DirEntry*` in the typed struct.

## Growth

When `count` reaches `chunk_size`, `var_array_grow_base` promotes `root`:

1. Allocates a `VarArrayLevel` (height=1)
2. Sets `slots[0] = old_root` (existing chunk)
3. Allocates a new chunk for slot 1
4. CAS swaps `root` from old chunk to new level

Same pattern for higher levels — root CAS when a level's slots are exhausted.

## Concurrency

- `var_array_grow_base` uses `atomic_fetch_add(&count, 1)` for index assignment
- CAS on root for promotion — loser retries
- CAS on slot pointers for lazy chunk/level allocation
- Readers: lock-free. Walk the tree. NULL slot = not yet allocated.

## Non-Negotiable Constraints

- **No locks.** CAS only.
- **No realloc.** Chunks and levels are fixed-size.
- **No copy-on-grow.** Old data in place.
- **Lazy allocation.** Levels and chunks only when needed.
- **No upper bound.**
- **Macro layer provides full type safety.** No casts in user code.
- **Layout compatibility.** All typed structs are binary-identical to base.

## Files

| File | Purpose |
|------|---------|
| `src/var_array.c` | Core: new, delete, grow, resolve |
| `src/var_array.h` | Base structs + typed macros |
| `test/test_var_array.c` | Tests |

## Acceptance

- [ ] Insert 10 entries: single chunk, O(1) lookup
- [ ] Insert 257 entries: root promoted to level 1, all retrievable
- [ ] Insert 10,000 entries: all retrievable by index
- [ ] Concurrent insert from 4 threads: all present, no duplicates
- [ ] Lookup out-of-range returns NULL
- [ ] Custom chunk_size via `var_array_new_base(64)` works
- [ ] All tests pass
