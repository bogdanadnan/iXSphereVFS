# TODO — Future Optimizations

Items deferred from current implementation. Not blocking any phase.

---

## TODO-1 — Directory Index (Hash Table)

### Problem
DirContent chains are linear. `vfs_create` validates name uniqueness by walking
the entire chain — O(N) per create. `vfs_open_file` and `resolve_child_vp`
also walk the chain to find a child by name. At 50,000 files in a single
directory, this becomes a bottleneck (~125 seconds for creates alone).

### Proposed Solution
Add a per-directory hash table mapping `(name) → (DirContent VirtualPtr)`.
On create: check hash table for collision (O(1)), if not found, insert.
On delete/rename: update hash table.
On mount: rebuild hash table by walking the DirContent chain once.

The hash table is an in-memory structure — not persisted to disk. It's rebuilt
from the DirContent chain on mount. This is consistent with the dentry cache
already in place.

### Impact
- `vfs_create` collision check: O(N) → O(1)
- `vfs_open_file`: O(N) → O(1)
- Mount time: +O(N) to rebuild hash table (already O(N) for tree init)
- Memory: ~64 bytes per directory entry (name pointer + VirtualPtr + hash overhead)

### Files Affected
- `src/tree.c` — `vfs_create`, `vfs_open_file`, delete, rename
- New file `src/dir_index.c` — hash table implementation

---

## TODO-2 — pool_resolve Read/Write Split

### Problem
`pool_resolve` marks the cache entry dirty on every call — including read-only
access (tree traversal, chain walks, directory listing). This causes unnecessary
flushing: every pool page accessed (even for reads) is included in the next
`Flush`, even if no slots were modified.

### Current State
The dirty-always approach is safe but suboptimal. Pool pages are hot (rarely
evicted from cache), so the practical impact is limited to `Flush` time —
extra pages are written that didn't change, which is a no-op write.

### Proposed Solution
Split into two functions:
- `pool_resolve(Pool*, int64_t vptr) → const uint8_t*` — read-only, no dirty marking
- `pool_resolve_rw(Pool*, int64_t vptr) → uint8_t*` — write access, marks dirty

### Impact
- ~50 call sites need update from `pool_resolve` to `pool_resolve_rw`
- Write path callers: `pool_alloc`, `nodes_write_*`, `tree_resolve_page` (segment creation), `gc_copy_entry`, `touchedfile_add`, `mapper_insert`
- Read path callers: tree traversal, version chain walks, directory listing, dentry cache, `vfs_read`, GC survival rule checks
- Risk: missing a write-path call site → silent data loss on eviction (same class as the original bug)

### Files Affected
- `src/pool.c`, `src/pool.h` — add `pool_resolve_rw`, make `pool_resolve` return `const`
- `src/tree.c`, `src/epoch.c`, `src/mapper.c`, `src/touched.c`, `src/gc.c`, `src/nodes.c`, `src/dentry_cache.c`, `src/page_array.c` — update write-path call sites

---

## TODO-3 — Linear Directory Chain Dedup

### Problem
The `vfs_create` collision check and `vfs_readdir` deduplication both walk
the DirContent chain comparing `childNodeId` values. At large directory sizes,
this is O(N) per operation.

### Proposed Solution
Extend TODO-1's directory index to include `childNodeId → childPtr` mapping
in addition to `name → VirtualPtr`. This makes `vfs_readdir` dedup O(1) per
child instead of O(N²).

### Impact
- `vfs_readdir` at large directories: O(N²) dedup → O(N) with index
- Memory: additional ~16 bytes per unique child

---

## TODO-4 — GC Physical Compaction

### Problem
GC reclaims logical pages (setting indirection entries to 0) but never
compacts physical space. Between GC cycles, the backing file grows
monotonically. Data pages from dropped VersionPages leave physical holes.

### Proposed Solution
During GC shadow-compaction, allocate a new sequential range of physical
offsets for surviving data pages, copy their content, and update indirection
entries. After the swap, `ftruncate` the backing file to reclaim unused
physical space.

### Impact
- GC time: +O(surviving data pages) for copy (already O(metadata) for tree walk)
- Backing file size: shrinks after GC
- Complexity: must coordinate physical offsets with deferred-free queue

---

## TODO-5 — Physical Offset Reuse Stack

### Problem
Every `Free` sets the indirection entry to 0, but the physical space at the
old offset is never reused. Between GC cycles, `physical_tail` advances
monotonically, creating physical holes.

### Proposed Solution
Per the deferred optimization noted in SPEC.md §3.8: maintain a stack of freed
physical offsets. `Allocate` pops from the stack when non-empty, falling back
to advancing `physical_tail`. All pages are the same size (`page_size + 16`),
so there is zero fragmentation.

### Impact
- `Allocate`: O(1) pop or CAS advance (no change)
- `Free`: O(1) push (currently just sets entry to 0)
- Backing file: stops growing between GC cycles
