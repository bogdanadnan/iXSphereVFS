# Phase 16: Dynamic Lock-Free Array (DirIndex Foundation)

## Goal
A variable-size, lock-free array for storing directory index entries. Fixed
chunk size (256 entries default, configurable), grows via pointer tables.
Lazy allocation: each level is allocated only when the previous level is
exhausted. No upper bound except available RAM.

## Data Structure

### Entry

```c
typedef struct {
    uint64_t key;      // hash or unique identifier
    int64_t  value;    // VirtualPtr or other payload
} DirArrayEntry;       // 16 bytes, 256 entries = 4096 bytes per chunk
```

### Chunk

```c
typedef struct {
    DirArrayEntry entries[256];
    int           count;      // number of valid entries (0..256)
} DirArrayChunk;              // 4100 bytes
```

### Level Node

A level N node is a pointer table of 256 slots. At level 0, slots point to
`DirArrayChunk`. At level N (N > 0), slots point to level N-1 nodes.

```c
typedef struct DirArrayLevel {
    void*          slots[256];  // DirArrayChunk* or DirArrayLevel*
    int            height;      // 0 = chunk level, N = N levels of indirection
    volatile int   allocated;   // number of slots filled (for GC/reclaim)
} DirArrayLevel;
```

### Dynamic Array

```c
typedef struct {
    DirArrayLevel* root;        // points to a Level node (starting at height 0)
    int            chunk_size;  // configurable, default 256
    volatile int   count;       // atomic: total entries
} DirArray;
```

### Growth Algorithm

On insert, `idx = atomic_fetch_add(&count, 1)`. The path to the target chunk
is determined by `idx`. Start at `root`:

```
    remaining = idx
    node = root

    // Walk down from the current root height
    while node->height > 0:
        stride = chunk_size^node->height
        slot = remaining / stride
        remaining = remaining % stride
        if node->slots[slot] == NULL:
            new_node = calloc(1, sizeof(DirArrayLevel))
            new_node->height = node->height - 1
            CAS(&node->slots[slot], NULL, new_node)
        node = node->slots[slot]

    // At height 0: slot points to a chunk
    chunk_slot = remaining / chunk_size   // but remaining < chunk_size...
```

Wait — at height 0, the slots point to chunks. `remaining < chunk_size` because
the stride at height 1 was exactly `chunk_size`. The `remaining` value is the
index within the chunk.

Actually let me simplify. The number of entries per height-H node is `chunk_size^(H+1)`:

- Height 0: `chunk_size^1 = 256` entries (one chunk)
- Height 1: `chunk_size^2 = 65,536` entries (256 chunks)
- Height 2: `chunk_size^3 = 16,777,216` entries
- ...

To find entry `idx`:

```
node = root
remaining = idx

while node->height > 0:
    stride = chunk_size^(node->height)   // entries per sub-tree
    slot = remaining / stride
    remaining = remaining % stride
    if node->slots[slot] == NULL:
        allocate new level with height = node->height - 1
        CAS into node->slots[slot]
    node = node->slots[slot]

// node->height == 0: node->slots are chunk pointers
chunk_slot = remaining / chunk_size  // always 0 if chunk_size == 256
                                       // (remaining < chunk_size^1 = 256)
entry_idx = remaining % chunk_size
chunk = node->slots[chunk_slot]
if chunk == NULL:
    chunk = calloc(1, sizeof(DirArrayChunk))
    CAS(&node->slots[chunk_slot], NULL, chunk)
chunk->entries[entry_idx] = entry
```

When root is exhausted (all 256 slots of the root node's sub-trees are full),
allocate a NEW root with `height = old_root->height + 1`, CAS into `array->root`:

```
if idx >= chunk_size^(root->height + 1):
    new_root = calloc(1, sizeof(DirArrayLevel))
    new_root->height = root->height + 1
    new_root->slots[0] = root          // old root is slot 0 of new root
    CAS(&array->root, root, new_root)
```

### Lookup

Same walk as insert, without allocation. If any slot is NULL, entry doesn't
exist — return NULL.

### Remove

Set `chunk->entries[entry_idx].value = VFS_VPTR_NULL`. The slot remains
occupied — key is preserved for lookup. The entry count is never decremented.

## Concurrency

- **Readers**: lock-free. Walk the tree following CAS-installed pointers.
  If a pointer is NULL (not yet allocated), entry doesn't exist.
- **Writers**: CAS on slot pointers. Losers retry from the slot allocation step.
  The `atomic_fetch_add(&count, 1)` assigns a unique index to each writer.
  No two writers write to the same slot.
- **Root growth**: CAS on `array->root`. Loser re-reads root and retries.

## Growth Examples

```
Insert #1: root->height=0, chunk allocated, entry[0] = entry → 1 level
Insert #256: root->height=0, chunk slots[0] has 256 entries → still 1 level
Insert #257: root exhausted (capacity 256).
  → allocate new root (height=1), old root = new_root->slots[0]
  → stride = 256^1 = 256, slot = 257/256 = 1, remaining = 1
  → allocate level-0 node at slots[1], allocate chunk, entry[1] = entry
  → 2 levels: root(height=1) → level[0][0](old data) + level[0][1](new chunk)

Insert #65536: root's 256 level-0 nodes all full.
  → allocate new root (height=2), old root = new_root->slots[0]
  → 3 levels
```

Maximum pointer dereferences for N entries: `ceil(log_256(N))`.

## Files

| File | Purpose |
|------|---------|
| `src/dir_array.c` | DirArray implementation |
| `src/dir_array.h` | Public API |
| `test/test_dir_array.c` | Tests |

## API

```c
void  dir_array_init(DirArray* arr, int chunk_size);
void  dir_array_destroy(DirArray* arr);
int   dir_array_insert(DirArray* arr, DirArrayEntry entry);
DirArrayEntry* dir_array_lookup(DirArray* arr, int index);
void  dir_array_remove(DirArray* arr, int index);
int   dir_array_count(DirArray* arr);
```

## Non-Negotiable Constraints

- **No locks.** CAS only.
- **No realloc of existing chunks or levels.** Once allocated, never resized.
- **No copy-on-grow.** Old entries stay in place during root growth.
- **Lazy allocation.** Level N is allocated only when level N-1 is full.
- **No upper bound.** Grows until RAM is exhausted.
- **Configurable chunk_size.** Default 256, minimum 16, maximum 4096.

## Acceptance

- [ ] Insert 10 entries: single chunk, root->height=0
- [ ] Insert 256 entries: single chunk at capacity, root->height=0
- [ ] Insert 257 entries: root grows to height=1, new chunk at slot 1
- [ ] Insert 10,000 entries: verify all retrievable by index
- [ ] Concurrent insert from 4 threads: all entries present, no duplicates
- [ ] Lookup on unallocated slot returns NULL (no crash)
- [ ] Remove sets value to VFS_VPTR_NULL without crashing
- [ ] All tests pass
