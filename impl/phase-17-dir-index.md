# Phase 17: Directory Index (Hash Table on VarArray)

## Goal
Replace O(N) DirContent chain walks with O(1) hash lookups. Each directory
gets an in-memory hash table backed by Phase 16's VarArray. Built lazily
on first access. Writes update both the pool chain (durable) and the index
(in-memory). Survives close/reopen by being rebuilt from the chain.

## Data Structure

```c
typedef struct {
    uint64_t name_hash;   // hash of the file name
    int64_t  child_vp;    // VirtualPtr to child's DirNode or FileNode
    uint32_t child_id;     // nodeId for dedup
    uint32_t epoch;        // epoch this entry was written at
} DirEntry;

// Per-directory index — stored in the TreeContext or in a global table
typedef struct {
    VarArray(DirEntry) entries;     // hash table entries
    int64_t           dir_vp;       // DirNode VirtualPtr this index belongs to
    int64_t           gc_gen;       // gc_generation at build time (invalidation)
    bool              loaded;       // true if built from chain
} DirIndex;
```

## Hash Function

splitmix64 of the name string, modulo VarArray capacity.

```c
static uint64_t hash_name(const char* name) {
    uint64_t h = splitmix64((const uint8_t*)name, strlen(name));
    return h ? h : 1;   // 0 = empty slot sentinel
}
```

## Operations

### Build (on first access)

Walk the DirContent chain for the directory. For each entry visible at
the current epoch (same dedup rules as `vfs_readdir`):
1. Hash the name
2. `var_array_append(index, entry)` — insert sequentially
3. After all entries inserted, build a quick-lookup array by walking the
   VarArray and creating a hash→index map... 

Actually, simpler: use open addressing. Each slot has `name_hash`. To find:

```c
int64_t dir_index_lookup(DirIndex* di, const char* name) {
    uint64_t h = hash_name(name);
    int n = var_array_count(di->entries);
    int idx = h % n;
    int start = idx;
    do {
        DirEntry* e = var_array_lookup(di->entries, idx);
        if (!e || e->name_hash == 0) return 0;  // empty — not found
        if (e->name_hash == h) {
            // Verify name matches (hash collision check)
            char* actual_name = read_name_from_pool(e->child_vp);
            if (strcmp(actual_name, name) == 0) return e->child_vp;
        }
        idx = (idx + 1) % n;
    } while (idx != start);
    return 0;
}
```

### Insert

On `vfs_create`:
1. Hash the name
2. Walk the DirContent chain to find the first empty slot or probe to find
   insertion point. But we build the index from the chain, not incrementally.

Better: don't insert incrementally. Rebuild the index from the chain after each
mutation. The chain is the source of truth. The index is a snapshot cache.

### Rebuild

On create, delete, rename:
1. Mutate the DirContent chain (CAS-prepend new entry or tombstone)
2. Call `dir_index_rebuild(di)` — walks the chain, clears VarArray, re-inserts all entries

This is O(N) per mutation — same as the current chain walk. But reads become O(1)
via hash. For the small-file benchmark (5000 files), the DirContent chain walk
happens once per mutation anyway (name collision check). Rebuilding adds a second
walk — doubling the cost. The 3.5× improvement from Phase 15 came from eliminating
the segment build. The hash table improves reads (`vfs_open_file`: O(N) → O(1)).

If rebuild cost is too high: build incrementally. On create, `var_array_append`
the new entry. On delete, mark tombstone in the index. On rename, update the
entry. No rebuild needed — the index stays in sync. Reads probe open-addressing
slots.

### Incremental Insert

```c
void dir_index_insert(DirIndex* di, const char* name, int64_t child_vp,
                      uint32_t child_id, uint32_t epoch) {
    DirEntry e = { .name_hash = hash_name(name),
                   .child_vp = child_vp,
                   .child_id = child_id,
                   .epoch = epoch };
    int idx = var_array_append(di->entries, e);
}
```

On lookup: probe linearly from `hash % count`. Empty slot (name_hash=0) means
not found. Tombstone (child_vp = VFS_VPTR_NULL) means deleted — skip.

On delete: `var_array_lookup` to find the entry, `var_array_update` to set
`child_vp = VFS_VPTR_NULL` (tombstone).

On rename: find entry by old name, update `name_hash` and `child_vp` in place.

### Fragmentation

After many deletes, the linear probe chain gets longer (skipping tombstones).
Rebuild on GC or when probe length exceeds threshold (e.g., 8 probes).

## Where to Store the Index

One index per directory. Store in a global hash table in TreeContext:
`{ dir_vp → DirIndex* }`. Built lazily — first access to a directory triggers
build. Invalidated on GC (`gc_generation` mismatch). Stored as `void*` in an
auxiliary slot or a separate small structure.

Alternative: store directly in the DirNode. DirNode has 16 bytes reserved.
Could store a pointer to the DirIndex there. But VarArray is heap-allocated,
so we store a `DirIndex*` pointer (8 bytes). Remaining 8 bytes unused.

## API

```c
// Internal (called from tree.c)
DirIndex* dir_index_ensure(TreeContext* ctx, int64_t dir_vp);
int64_t   dir_index_lookup(DirIndex* di, const char* name);
void      dir_index_insert(DirIndex* di, const char* name, int64_t child_vp,
                           uint32_t child_id, uint32_t epoch);
void      dir_index_delete(DirIndex* di, const char* name);
void      dir_index_rename(DirIndex* di, const char* old_name,
                           const char* new_name, int64_t child_vp);
void      dir_index_destroy(DirIndex* di);

// Refactored read path (use dir_index_lookup instead of dirchain_find_child)
int64_t vfs_open_file(vfs, parent, name, epoch);
int     vfs_create(vfs, parent, name, epoch);  // name collision check
int     vfs_readdir(vfs, dir, entries, max, epoch);  // iterate index
```

## Acceptance

- [ ] `dir_index_lookup` returns same result as `dirchain_find_child` for any directory
- [ ] Create+lookup: file found immediately via index
- [ ] Delete+lookup: file not found (tombstone skip)
- [ ] Rename: old name not found, new name found
- [ ] 5000-file directory: open_file is O(1), not O(N)
- [ ] GC rebuild: index properly invalidated and rebuilt
