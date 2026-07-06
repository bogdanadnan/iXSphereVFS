# Phase 17: Directory Cache (Thread-Local VarArray)

## Goal
For directories with 64+ entries, build a thread-local VarArray containing
name hashes and VirtualPtrs. Lookups scan the array linearly (O(N) but with
contiguous memory and no pool lookups — 100× faster than chain walking).
For small directories (≤64), keep the current DirContent chain walk.

Also add a 64-bit name hash to each NameEntry — stored in the first 8 bytes
of the 24-byte data field. On lookup, compare the hash first; only walk the
full name chain on hash match. This accelerates ALL lookups regardless of
cache state.

## NameEntry Hash

Reuse the first 8 bytes of the existing 24-byte NameEntry data field:

```
NameEntry (32 bytes, existing layout):
 Offset  Size  Field
 ──────  ────  ─────
    0     24    data     (UTF-8 bytes, zero-padded)
   24      8    nextPtr  (VirtualPtr — next NameEntry, 0 = end)
```

New layout — first 8 bytes now serve double-duty as hash:
```
    0      8    name_hash / data[0..7]  (uint64 hash; also UTF-8 start if name ≤ 8)
    8     16    data[8..23]             (remaining UTF-8 bytes)
   24      8    nextPtr
```

For names ≤ 8 bytes: the hash IS the name in UTF-8 (fully inlined, no extra storage).
For names > 8 bytes: the hash is a real hash of the full name. Collision resolution
walks the remaining chain.

```c
uint64_t name_hash(const char* name, int len) {
    if (len <= 8) {
        uint64_t h = 0;
        memcpy(&h, name, len);
        return h;
    }
    return splitmix64((const uint8_t*)name, len);
}
```

Writing a name: compute hash, store in first 8 bytes of data, zero-pad remainder.
Reading a name: check hash match first, then verify full name.

## Thread-Local Directory Cache

Same pattern as the thread-local segment cache (Phase 15). Per-thread, small
number of entries (16), keyed by directory VirtualPtr.

```c
typedef struct {
    uint64_t name_hash;   // from name_hash()
    int64_t  child_vp;    // VirtualPtr to child (DirNode or FileNode)
    uint32_t child_id;     // for dedup
    uint32_t epoch;        // epoch of the DirContent entry
    bool     tombstone;    // true if namePtr == 0 in DirContent
} DirCacheEntry;           // 32 bytes, 256 per chunk = 8KB per chunk

// Thread-local cache
static __thread struct {
    int64_t       key;       // dir_vp
    VarArrayBase* entries;   // VarArray of DirCacheEntry
    int64_t       gc_gen;    // gc_generation at build time
    int           entry_count; // number of visible entries
    bool          built;
} dir_cache[16];
static __thread int dir_cache_next = 0;
```

## Operations

### Build

On first access to a directory after `entry_count >= 64`:

1. Walk the DirContent chain (same dedup rules as `vfs_readdir`)
2. For each visible entry: resolve name, compute hash, create `DirCacheEntry`
3. `var_array_append(cache_entry)` into the VarArray
4. Mark `built = true`, set `gc_gen` to current `ctx->gc_generation`

The walk is O(N) — same as today's `vfs_readdir`. But it's done once per
directory per thread, persisting across subsequent accesses to the same
directory.

### Lookup (replaces dirchain_find_child)

```c
int dir_cache_lookup(TreeContext* ctx, int64_t dir_vp,
                     const char* name, int64_t* out_childPtr,
                     uint32_t* out_nodeId) {
    // Try thread-local cache first
    DirCacheEntry* cache = find_dir_cache(dir_vp, ctx->gc_generation);
    if (cache && cache->built) {
        uint64_t h = name_hash(name, strlen(name));
        int n = var_array_count(cache->entries);
        for (int i = 0; i < n; i++) {
            DirCacheEntry* e = var_array_lookup(cache->entries, i);
            if (e->name_hash == h && !e->tombstone) {
                // Verify full name match
                char stored_name[256];
                if (read_name_from_entry(e->child_vp, stored_name, 256) > 0 &&
                    strcmp(stored_name, name) == 0) {
                    *out_childPtr = e->child_vp;
                    *out_nodeId = e->child_id;
                    return VFS_OK;
                }
            }
        }
        return VFS_ERR_NOTFOUND;
    }

    // Fall back to chain walk (small directory or cache not yet built)
    return dirchain_find_child_hashed(ctx, dir_vp, name, out_childPtr, out_nodeId);
}
```

### dirchain_find_child_hashed

Same as current `dirchain_find_child` but uses the NameEntry hash for fast
rejection:

```c
// In the chain walk:
uint64_t query_hash = name_hash(name, len);
// For each DirContent entry:
uint64_t stored_hash = read_name_hash(namePtr);  // 8 bytes from NameEntry
if (stored_hash != query_hash) continue;          // fast reject
// Only on match: walk full name chain and strcmp
```

### Cache Scope — vfs_readdir Only

The cache accelerates `vfs_readdir` — the most expensive directory operation
(O(N²) with dedup). On first `vfs_readdir` for a directory, build the VarArray
from the chain (one O(N) walk). Subsequent `vfs_readdir` calls iterate the
array directly — O(N) contiguous scan vs O(N²) chain dedup.

**`vfs_open_file` and `vfs_create` collision check still use the chain walk**
but with the NameEntry hash optimization for fast reject. This avoids the
tombstone staleness problem.

### Why Not Cache File Lookups

A thread's cached file entry may be stale — another thread created a tombstone
for that file at a higher epoch. Detecting this requires walking the chain
(every cache hit would need chain verification). The cache adds no value.

`vfs_readdir` builds a complete snapshot of the directory at query time.
Subsequent reads reuse the snapshot. Mutations invalidate it.

## Cache Lifecycle

- **Build**: on first `vfs_readdir(dir, epoch)` after entry_count ≥ 64.
  Walk the chain, populate VarArray. Cache keyed by `(dir_vp, epoch)`.
- **Serve**: `vfs_readdir` copies from array — no chain walk.
- **Invalidate**: on any mutation (create/delete/rename) in this directory.
  Sets `built = false`. Next `vfs_readdir` rebuilds.

## When to Build

Track `entry_count` in the DirNode's reserved space — e.g., at offset 16:

```c
#define DIRNODE_OFF_ENTRYCOUNT 16  // uint32_t — approximate entry count
```

Increment on create, decrement on delete (tombstone). Don't need atomic
accuracy — it's a heuristic. If `entry_count >= 64`, build the cache.

Alternative: just walk the chain on first access and count. If ≥ 64, build
the cache. Same cost as a `vfs_readdir`. Done once per directory.

## Impact

| Operation | Before | After | Improvement |
|-----------|--------|-------|-------------|
| `vfs_open_file` (5K files) | O(N) chain walk | O(N) array scan | 100× |
| `vfs_create` collision check | O(N) chain walk | O(N) array scan | 100× |
| `vfs_readdir` (5K files) | O(N) chain walk | O(N) array iteration | 100× |
| `vfs_open_file` (small) | O(N) chain walk | O(N) chain walk + hash | 2-8× (hash reject) |

## Acceptance

- [ ] Name hash stored in first 8 bytes of NameEntry data
- [ ] dirchain_find_child_hashed uses hash for fast reject
- [ ] Directory with 64+ entries builds thread-local VarArray
- [ ] lookup finds file via VarArray scan (not chain walk)
- [ ] Create/delete/rename invalidate cache for that directory
- [ ] GC invalidates all caches
- [ ] All existing tree tests pass
