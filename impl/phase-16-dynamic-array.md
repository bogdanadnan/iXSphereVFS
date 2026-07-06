# Phase 16: Dynamic Lock-Free Array (DirIndex Foundation)

## Goal
A variable-size, lock-free array for storing directory index entries. Fixed
chunk size (256 entries default, configurable), grows via pointer tables in
power-of-two levels. Supports concurrent reads without locks. Insertions use
CAS to add new chunks and levels.

## Motivation

VFS directory operations walk DirContent chains (O(N)). A hash table or
sorted array would require variable-size storage. A dynamic array is the
prerequisite — used by both the directory index (Phase 17) and future
structures.

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

### Dynamic Array

```c
typedef struct {
    DirArrayChunk*   direct;        // level 0: first chunk (count <= 256)
    DirArrayChunk**  level1;        // level 1: pointer table, 256 slots (count <= 65536)
    DirArrayChunk*** level2;        // level 2: pointer-to-pointer table, 256 slots
    int              chunk_size;    // configurable, default 256
    volatile int     count;         // atomic: total entries across all chunks
} DirArray;
```

### Growth Algorithm

```
INSERT(array, entry):
   idx = atomic_fetch_add(&array->count, 1)
   level0_cap = chunk_size                     // 256
   level1_cap = level0_cap * chunk_size        // 65,536
   level2_cap = level1_cap * chunk_size        // 16,777,216

   if idx < level0_cap:
       if array->direct == NULL:
           CAS(&array->direct, NULL, calloc(1, sizeof(DirArrayChunk)))
       chunk = array->direct
       chunk->entries[idx] = entry
       return

   if idx < level1_cap:
       if array->level1 == NULL:
           CAS(&array->level1, NULL, calloc(256, sizeof(DirArrayChunk*)))
       table_idx = idx / chunk_size - 1          // 0..255
       chunk_idx = idx % chunk_size              // 0..255
       if array->level1[table_idx] == NULL:
           chunk = calloc(1, sizeof(DirArrayChunk))
           CAS(&array->level1[table_idx], NULL, chunk)
       chunk = array->level1[table_idx]
       chunk->entries[chunk_idx] = entry
       return

   // level 2 (and beyond, same pattern)
```

### Lookup

```
LOOKUP(array, index):
   if index < level0_cap:
       return &array->direct->entries[index]
   if index < level1_cap:
       table_idx = index / chunk_size - 1
       chunk_idx = index % chunk_size
       return &array->level1[table_idx]->entries[chunk_idx]
   if index < level2_cap:
       idx = index - level1_cap
       t2_idx = idx / (chunk_size * chunk_size)  // 0..255
       inner_idx = idx % (chunk_size * chunk_size)
       t1_idx = inner_idx / chunk_size           // 0..255
       ch_idx = inner_idx % chunk_size           // 0..255
       return &array->level2[t2_idx][t1_idx]->entries[ch_idx]
```

Maximum 3 pointer dereferences for 16.7M entries.

## Concurrency

- **Readers**: lock-free. Load `array->count` atomically, compute chunk, read entry.
  If chunk is NULL (not yet allocated), reader sees a partial write — retry or
  return "not found."
- **Writers**: CAS on `array->direct`, `array->level1`, `array->level1[N]`.
  Losers retry. Chunks are pre-allocated and CAS-installed atomically.
  Entries within a chunk are written with a release barrier before count
  increment.

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
- **No realloc of existing chunks.** Chunks are fixed-size, never resized.
- **No copy-on-grow.** Old entries stay in place. Only new chunks are allocated.
- **Configurable chunk_size.** Default 256, minimum 16, maximum 4096.

## Acceptance

- [ ] Insert 10,000 entries, verify all retrievable by index
- [ ] Concurrent insert from 4 threads, all entries present, no duplicates
- [ ] Lookup on unallocated chunk returns NULL (no crash)
- [ ] Remove sets value to VFS_VPTR_NULL without shifting
- [ ] All tests pass
