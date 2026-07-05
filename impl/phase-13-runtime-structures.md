# Phase 13: Mapper Table Cache

## Goal
Replace `mapper_resolve` and `mapper_traversal_apply` chain walks with an
in-memory table. Built once at mount from the on-disk mapper chain. Updated
on commit, soft-delete, and GC. O(1) lookup instead of walking VirtualPtr
chains through pool pages.

## Current State
Every `vfs_read`, `vfs_write`, `vfs_epoch_is_writable` walks the mapper
chain via `pool_resolve` → `storage_read` → `cache_find`. For a 3-entry
chain, that's 3 hash lookups + 3 bucket locks. Per operation. Even when
the pool pages are cached, the chain walk overhead is measurable.

## Data Structure

```c
typedef struct {
    uint32_t fromEpoch;
    uint32_t toEpoch;
    bool     traversalApply;
} MapperEntry;

typedef struct {
    MapperEntry* entries;    // dynamic array
    int          count;
    int          capacity;
    int64_t*      epochMapperPtr;  // pointer to TreeContext.epochMapperPtr
    Pool*         pool;            // for writing to pool on mutations
} MapperTable;
```

## Operations

### `mapper_table_init(table, pool, epochMapperPtr)`
Walk the on-disk mapper chain from `*epochMapperPtr`. For each MapperEntry,
append `{fromEpoch, toEpoch, traversalApply}` to `entries[]`. Called once
at mount from `vfs_mount` → `tree_init`.

### `mapper_table_resolve(table, epoch) → int64_t`
Linear scan of `entries[]` (max ~10 entries). Return `toEpoch` if
`fromEpoch == epoch`, else return `epoch` unchanged. O(N) where N ≤ 10.

### `mapper_table_traversal_apply(table, epoch) → bool`
Linear scan. Return `traversalApply` flag if `fromEpoch == epoch`,
else false.

### `mapper_table_insert(table, fromEpoch, toEpoch, traversalApply)`
1. Write the MapperEntry to the pool chain via `mapper_insert` (CAS-prepend
   to `*epochMapperPtr`). This is the durable write — it MUST succeed first.
2. Append `{fromEpoch, toEpoch, traversalApply}` to `entries[]`.
3. Write barrier before incrementing `count` — ensures readers see the
   new entry's data when they see the updated count.

### `mapper_table_rebuild(table)`
Called after GC. Clears `entries[]`, re-walks the chain, repopulates.

## Changes to Existing Code

| File | Change |
|------|--------|
| `src/mapper.c` | Add `mapper_table_*` functions. Existing chain-walk functions remain for GC/write paths. |
| `src/vfs.c` | `vfs_mount` → after `mapper_init`, call `mapper_table_init` |
| `src/tree.c` | `vfs_read` → replace `mapper_resolve`/`mapper_traversal_apply` chain walks with `mapper_table_resolve`/`mapper_table_traversal_apply` |
| `src/epoch.c` | `vfs_commit`, `vfs_delete_snapshot` → after `mapper_insert`, call `mapper_table_insert`. `vfs_epoch_is_writable` → use `mapper_table_resolve`. |
| `src/gc.c` | After GC rebuilds pool pages, call `mapper_table_rebuild` |

## Non-Negotiable Constraints

- **Pool chain remains source of truth.** The `entries[]` array is a cache.
  On mount, it is rebuilt from the chain. If corrupted, discard and rebuild.
- **Mutations update both atomically.** Every insert into the pool chain also
  inserts into `entries[]`. The pool write (CAS-prepend) MUST succeed first.
  Only then is `entries[]` updated. If the pool write fails, the in-memory
  update is skipped — no divergence risk.
- **Reads never touch pool for mapper.** After mount, all reads use
  `entries[]`. The MapperTable is small — typically 0–3 entries.
- **Lock-free reads, serialized writes.** `mapper_table_resolve` and
  `mapper_table_traversal_apply` are read-only and need no synchronization.
  `mapper_table_insert` is called only from `vfs_commit` and
  `vfs_delete_snapshot`, which are serialized by the per-epoch lock.
  `mapper_table_rebuild` is called only from GC, which holds the exclusive
  tree lock. No concurrent inserts or rebuilds are possible. Entries are
  appended to the end of the array with a release barrier before `count`
  is incremented, ensuring readers see fully initialized data.

## Staging

### Stage A — MapperTable core
- `mapper_table_init`, `mapper_table_resolve`, `mapper_table_traversal_apply`,
  `mapper_table_insert`, `mapper_table_rebuild`
- Wire into `vfs_mount` (`mapper_table_init`)

### Stage B — Unified Chain-Walk Helpers
Extract all hand-rolled chain walks into shared functions. Each function
uses `pool_resolve` — same behavior as today, centralized in one place.

**`dirchain_find_child(ctx, dirVp, name, epoch) → childVp, childNodeId`**
Walk DirContent chain. Find child with matching `name` at `epoch` using
read-rule dedup (highest `epoch ≤ query`, tombstone-aware). Returns
child's VirtualPtr and nodeId. Used by: `vfs_mount`, `vfs_delete`,
`vfs_rename` (source lookup), `resolve_child_vp` (benchmark).

**`dirchain_list(ctx, dirVp, entries, max, epoch) → count`**
Walk DirContent chain. Collect all non-tombstone children visible at
`epoch`, deduplicated by `childNodeId` (highest `epoch ≤ query`).
Fills `entries[]` with `{nodeId, name, isDir}`. Used by: `vfs_readdir`.

**`verchain_get(ctx, versionRootPtr, readEpoch) → dataPage`**
Walk VersionPage chain from `versionRootPtr`. Apply read rule: exact
match at `readEpoch`, or highest even `epoch < readEpoch`. Apply mapper
traversal. Returns data page index, or -1 if never written. Used by:
`vfs_read`, `vfs_write` (COW base page lookup).

**`sizechain_get(ctx, sizePtr, epoch) → {fileSize, modifiedAt}`**
Walk FileSize chain from `sizePtr`. Apply read rule. Returns size and
mtime. Used by: `vfs_file_size`, `vfs_file_mtime`.

### Stage C — Refactor Callers
- Replace all inline chain walks in `vfs_read`, `vfs_file_size`, `vfs_file_mtime`,
  `vfs_mount`, `vfs_readdir`, `vfs_delete`, `vfs_rename` with calls to
  the shared helpers above.
- Replace `mapper_resolve(&ctx->mapper, epoch)` calls with `mapper_table_resolve`
- Replace `mapper_traversal_apply` calls with `mapper_table_traversal_apply`
- Replace `vfs_epoch_is_writable` mapper check with `mapper_table_resolve`

### Stage D — Wire Write Path
- After `mapper_insert` in `vfs_commit` and `vfs_delete_snapshot`,
  call `mapper_table_insert`
- After GC, call `mapper_table_rebuild`

## Acceptance
- [ ] After mount: `mapper_table_resolve` matches chain-walk result
- [ ] After commit: new mapping in table AND on disk
- [ ] After soft-delete: new mapping in table AND on disk
- [ ] After GC: table rebuilt correctly
- [ ] All existing tests pass without modification
- [ ] `vfs_read` shows measurable latency reduction (>10%)
