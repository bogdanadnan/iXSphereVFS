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
at mount from `vfs_open` → `tree_init`.

### `mapper_table_resolve(table, epoch) → int64_t`
Linear scan of `entries[]` (max ~10 entries). Return `toEpoch` if
`fromEpoch == epoch`, else return `epoch` unchanged. O(N) where N ≤ 10.

### `mapper_table_traversal_apply(table, epoch) → bool`
Linear scan. Return `traversalApply` flag if `fromEpoch == epoch`,
else false.

### `mapper_table_insert(table, fromEpoch, toEpoch, traversalApply)`
Append to `entries[]`. Also write the MapperEntry to the pool chain
(CAS-prepend to `*epochMapperPtr`) via `mapper_insert` (existing code
in mapper.c). Both updates must succeed.

### `mapper_table_rebuild(table)`
Called after GC. Clears `entries[]`, re-walks the chain, repopulates.

## Changes to Existing Code

| File | Change |
|------|--------|
| `src/mapper.c` | Add `mapper_table_*` functions. Existing chain-walk functions remain for GC/write paths. |
| `src/vfs.c` | `vfs_open` → after `mapper_init`, call `mapper_table_init` |
| `src/tree.c` | `vfs_read` → replace `mapper_resolve`/`mapper_traversal_apply` chain walks with `mapper_table_resolve`/`mapper_table_traversal_apply` |
| `src/epoch.c` | `vfs_commit`, `vfs_delete_snapshot` → after `mapper_insert`, call `mapper_table_insert`. `vfs_epoch_is_writable` → use `mapper_table_resolve`. |
| `src/gc.c` | After GC rebuilds pool pages, call `mapper_table_rebuild` |

## Non-Negotiable Constraints

- **Pool chain remains source of truth.** The `entries[]` array is a cache.
  On mount, it is rebuilt from the chain. If corrupted, discard and rebuild.
- **Mutations update both.** Every insert into the pool chain also inserts
  into `entries[]`. No divergence.
- **Reads never touch pool for mapper.** After mount, all reads use
  `entries[]`. The MapperTable is small — typically 0–3 entries.

## Staging

### Stage A — MapperTable core
- `mapper_table_init`, `mapper_table_resolve`, `mapper_table_traversal_apply`,
  `mapper_table_insert`, `mapper_table_rebuild`
- Wire into `vfs_open` (`mapper_table_init`)

### Stage B — Refactor read path
- Replace `mapper_resolve(&ctx->mapper, epoch)` calls in `vfs_read`,
  `vfs_file_size`, `vfs_file_mtime`, `vfs_open_file`, `vfs_readdir`,
  `vfs_epoch_is_writable` with `mapper_table_resolve`
- Replace `mapper_traversal_apply` calls with `mapper_table_traversal_apply`

### Stage C — Wire write path
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
