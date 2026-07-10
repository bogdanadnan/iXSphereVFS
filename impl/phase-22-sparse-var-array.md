# Phase 22: Sparse VarArray + Path Allocation

## Goal

Generalize `var_array` from a dense array (sequential `[0, count)`) to a sparse array (any index `[0, capacity)` may be set independently). Add `var_array_set_base(idx, value_ptr)` and `var_array_reserve_base(idx)` primitives. Rename the existing `var_array_update` macro to `var_array_set` with new semantics (allocates path if needed).

This phase delivers only the var_array primitive changes. Hash_map simplifications that consume the new API are deferred to **Phase 23** (separate spec).

## Background

Today var_array only supports sequential allocation via `var_array_grow_base`. The chunk tree internally supports sparse allocation (intermediate nodes can be NULL until first access, and the resolver returns NULL for unallocated paths), but the public API doesn't expose this.

Callers that want to write to a specific index must:
1. Manually walk the tree, allocating missing chunks via repeated `grow_base` calls until count > target
2. Write via `resolve_base` + manual indexing

This is exactly the dance that hash_map's `hash_map_grow` rehash loop and `hash_map_base_put` insert path do today (~30 lines of grow coordination).

The chunk tree already implements the path-allocation algorithm in `var_array_grow_base` lines 102-169 of `src/var_array.c`. We just need to parameterize it by an explicit `idx` and expose it as a clean public API.

## Design

### Renames

**`var_array_update(a, idx, entry)` → `var_array_set(a, idx, entry)`**

Same parameters, new semantics:
- **Old**: silently drops the write if the slot doesn't exist.
- **New**: allocates the tree path for `idx` if needed, then writes.

This is a strict superset of the old behavior — any old caller that hit an existing slot still works; any caller that hit a non-existing slot now gets the allocation (an error if OOM, which previously was a silent no-op).

### New base functions

**`int var_array_reserve_base(VarArrayBase* a, int idx)`**

Lazily allocates the tree path for `idx`. On success, the slot at `idx` exists in the leaf chunk. Does NOT write any value — just ensures the slot is allocated. Updates `count = max(count, idx+1)` via CAS. Returns 0 on success, -1 on OOM.

**`int var_array_set_base(VarArrayBase* a, int idx, const void* value)`**

Same as `reserve_base` plus writes `entry_size` bytes from `value` into the slot. Returns 0 on success, -1 on OOM or invalid args.

### Path allocation semantics

When `idx` requires a chunk that doesn't yet exist:

1. **Determine required tree height for `idx`**: existing algorithm — count how many levels needed to address `idx`.

2. **Promote root to required height** if needed. **Rebalancing**: existing chunks get re-pointed into the new top level's slot 0. Chunk contents (actual entries) are unchanged — only the tree structure shifts. This is the same rebalance that `grow_base` already performs when its atomic-add'd idx exceeds current capacity.

3. **Allocate the path from root to leaf**:
   - At each level, look at the slot corresponding to `idx`.
   - If the slot is NULL, allocate a new level node (or leaf chunk) and CAS-install it.
   - **Sibling slots stay NULL** — only the path is allocated.

   Example: 3-level array (chunks of 256 → 16M total slots), set index 100:
   - Level 2 has 16 children (chunks 0-15).
   - Index 100 → level 2 slot 0, level 1 chunk for indices 0-255.
   - If level 2 slot 0 is NULL: allocate level-1 chunk for indices 0-255, then leaf chunk (same node), then set slot 100.
   - **All other level-2 slots (1-15) remain NULL**. No siblings allocated.

   Example: set index 500 (a different chunk): level 2 slot 1, level 1 chunk for indices 256-511.
   - After this, level 2 has 2 children: slot 0 (for 0-255) and slot 1 (for 256-511). Other 14 slots still NULL.

4. **Update `count`** to `max(count, idx+1)` (CAS).

### Refactored `var_array_grow_base`

```c
int var_array_grow_base(VarArrayBase* a) {
    if (!a) return -1;
    int idx = vfs_atomic_add_i32((int32_t*)&a->count, 1) - 1;
    if (var_array_reserve_base(a, idx) != 0) return -1;
    return idx;
}
```

Identical external behavior. Same atomic-add claim, same intermediate node allocation, same return value. Implementation now reuses `reserve_base`.

### Typed macros

**Renamed**:
```c
// Old: var_array_update(a, idx, entry) — silent drop if slot missing
// New:
#define var_array_set(a, idx, entry) \
    var_array_set_base((VarArrayBase*)(a), (idx), &(entry))
```

**Unchanged**:
- `var_array_append(a, entry)` — still uses `grow_base` + `resolve_base` + write. Sequential use is the common case; append stays sequential.
- `var_array_lookup(a, idx)` — unchanged. Returns NULL for missing slots.
- `var_array_new(T)`, `var_array_delete(a)` — unchanged.

### `count` semantics — clarified

**Dense mode** (sequential `grow_base` calls): `count` = next-to-allocate index. Indices `[0, count)` are all valid.

**Sparse mode** (calls to `set_base(idx, value)` with `idx > old_count`): `count` = `max(allocated_idx) + 1`. Indices `[0, count)` may include holes where `var_array_resolve_base` returns NULL.

For all existing dense users (sequential `grow_base` calls): `count` semantics unchanged. No holes appear between `[0, count)`.

For new sparse users (calling `set_base(idx, value)` with `idx > old_count`): `count` jumps to `idx+1`. Subsequent `resolve_base` of indices between `(old_count, idx)` returns NULL.

**`var_array_resolve_base` is unchanged** — already returns NULL for both "idx >= count" and "intermediate node NULL". Same observable behavior.

### Backward compatibility

- All 59,487 existing var_array tests must pass unchanged (with the 4 `update` call sites renamed to `set`).
- `var_array_grow_base` and `var_array_resolve_base` have identical external behavior to before.
- New `var_array_set_base` and `var_array_reserve_base` are pure additions.
- Existing dense callers (`var_array_append`, `var_array_lookup`, `var_array_new`, `var_array_delete`) work unchanged.

## Files

| File | Change |
|------|--------|
| `src/var_array.h` | Rename `var_array_update` macro → `var_array_set` (with new semantics). Declare `var_array_set_base` and `var_array_reserve_base`. |
| `src/var_array.c` | Implement `set_base` and `reserve_base` (extracting shared tree-walk code from `grow_base`). Refactor `grow_base` to use `reserve_base`. |
| `test/test_var_array.c` | Rename 4 `update` call sites to `set`. Update test name `test_var_array_update_in_place` → `test_var_array_set_in_place`. Add sparse-mode tests (set at high idx, holes between allocations, mixed sparse + dense). |

## Acceptance

- [ ] `var_array_set_base` and `var_array_reserve_base` declared in var_array.h.
- [ ] Both implemented in var_array.c (extracting shared tree-walk code).
- [ ] `var_array_update` macro removed; `var_array_set` macro added with new semantics.
- [ ] `var_array_grow_base` refactored to use `reserve_base` internally (identical external behavior).
- [ ] All 59,487 existing var_array tests pass after rename.
- [ ] At least 10 new sparse-mode tests added (set at idx > count, holes, mixed, rebalancing).
- [ ] All 10 unit test suites pass.

## Out of scope

- **Hash_map simplification**: phase 23 will use `set_base` to eliminate the manual grow coordination in `hash_map_grow` rehash and `hash_map_base_put` insert path.
- **`var_array_unset_base`**: zeroing out a slot. Hash_map uses state-encoded tombstones, not NULL slots. Defer until a use case appears.
- **Compaction**: freeing tree nodes when all slots in a chunk become empty. Var_array never shrinks today. Defer.
- **Migration of existing dense users**: none needed. Dense users get the same behavior.

## Risks

- **`var_array_set` is a behavior change from `var_array_update`**: any caller relying on the silent-drop behavior will now get an allocation (or OOM). 4 call sites in tests are easy to update; no other callers exist.
- **`count` semantics shift**: a sparse user sees `count` jump to `max(idx)+1`. Existing dense users see no change. Test must cover this distinction.
- **Performance regression in adversarial cases**: if hash distribution maps every key to bucket `new_capacity-1`, the loop does `new_capacity` grow calls, same as pre-allocate. No regression.
- **Atomic ops**: `set_base` does multiple CASes (one per tree level + one for count). Same as `grow_base`'s CAS pattern.
- **Refactor risk on `grow_base`**: even though external behavior is identical, the implementation now goes through `reserve_base`. Any bug in the refactor could break dense users. Mitigation: keep all 59,487 tests as the regression gate.

## Implementation order

1. Add `var_array_set_base` and `var_array_reserve_base` declarations to var_array.h.
2. Implement them in var_array.c (extracting the shared tree-walk code).
3. Refactor `var_array_grow_base` to use `reserve_base`.
4. Rename `var_array_update` macro → `var_array_set` in var_array.h.
5. Update 4 call sites in test_var_array.c (rename `update` → `set`, rename test function).
6. Build and run all 59,487 var_array tests — must still pass.
7. Add sparse-mode tests to test_var_array.c.
8. Build, run all 10 unit test suites — no regression.