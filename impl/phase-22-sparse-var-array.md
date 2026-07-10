# Phase 22: Sparse VarArray

## Goal

Generalize `var_array` from a dense array (sequential `[0, count)`) to a sparse array (any index `[0, capacity)` may be set independently). Add `var_array_set_base(idx, value)` and `var_array_reserve_base(idx)` primitives. Keep `var_array_grow_base` as a backward-compatible thin wrapper. Existing dense users see no behavior change; new sparse users get explicit insert-at-index support.

This phase delivers only the var_array primitive changes. Hash_map simplifications that consume the new API are deferred to **Phase 23** (separate spec).

## Background

Today var_array only supports sequential allocation: `grow_base` atomically claims the next index. There's no way to write to an arbitrary index — `grow_base(count - 1, value)` would skip every intervening slot, and there's no primitive to do that explicitly.

Callers that want to write to a specific index must:
1. Manually walk the tree, allocating missing chunks via repeated `grow_base` calls until count > target
2. Write via `resolve_base` + manual indexing

This is exactly the dance that hash_map's `hash_map_grow` rehash loop and `hash_map_base_put` insert path do today (~30 lines of grow coordination). With the new primitives, those loops collapse to a single `set_base(idx, &value)` call.

The chunk tree already supports sparse allocation internally — intermediate nodes can be NULL until first access, and the resolver returns NULL for unallocated paths. We just need to expose that capability as a clean public API.

## Design

### New var_array primitives

**`int var_array_reserve_base(VarArrayBase* a, int idx)`**

Lazily allocates the tree path for `idx`. On success, the slot at `idx` exists in the leaf chunk. Does NOT write any value — just ensures the slot is allocated. Updates `count = max(count, idx+1)` via CAS. Returns 0 on success, -1 on OOM.

**`int var_array_set_base(VarArrayBase* a, int idx, const void* value)`**

Same as `reserve_base` plus writes `entry_size` bytes from `value` into the slot at `idx`. Returns 0 on success, -1 on OOM or invalid args.

### Implementation

Both functions share the same tree-walk algorithm. The chunk tree already implements:
- Height promotion via CAS on root (for indices that exceed current height)
- Intermediate node allocation via CAS on slot pointers (siblings stay NULL)
- Leaf chunk allocation with `entry_size` bytes per slot

This is exactly the code in `var_array_grow_base` lines 102-169 of `src/var_array.c`, just parameterized by an explicit `idx` instead of atomic-add'd sequential.

Pseudocode for `set_base`:

```c
int var_array_set_base(VarArrayBase* a, int idx, const void* value) {
    if (!a || idx < 0 || !value) return -1;
    int cs = a->chunk_size;
    int es = a->entry_size;

    /* Promote root to required height (existing CAS loop from grow_base) */
    int needed_height = 0;
    int64_t cap = cs;
    while ((int64_t)idx >= cap) { needed_height++; cap *= cs; }

    void* old_root = vfs_atomic_load_ptr((const void* const*)&a->root);
    while (height_of(old_root) < needed_height) {
        int new_height = height_of(old_root) + 1;
        void* new_level = alloc_level_typed(cs, new_height);
        if (!new_level) return -1;
        *slot_of((VarArrayLevel*)new_level, 0) = old_root;
        void* cas_result = vfs_cas_ptr(&a->root, old_root, new_level);
        if (cas_result == old_root) {
            old_root = new_level;
        } else {
            free(new_level);
            old_root = cas_result;
        }
    }

    /* Walk tree, allocating missing intermediate nodes only (siblings stay NULL) */
    void* node = vfs_atomic_load_ptr((const void* const*)&a->root);
    int h = height_of(node);
    int64_t div = 1;
    for (int i = 0; i < h; i++) div *= cs;
    for (int level = h; level > 0; level--, div /= cs) {
        VarArrayLevel* lv = (VarArrayLevel*)node;
        int slot = (int)(((int64_t)idx / div) % cs);
        void* child = *slot_of(lv, slot);
        if (!child) {
            if (level > 1) {
                child = alloc_level_typed(cs, level - 1);
            } else {
                child = alloc_chunk_typed(cs, (size_t)es);
            }
            if (!child) return -1;
            void** slot_ptr = slot_of(lv, slot);
            void* old = vfs_cas_ptr(slot_ptr, NULL, child);
            if (old != NULL) {
                free(child);
                child = old;
            }
        }
        node = child;
    }

    /* node is now the leaf chunk.  Write value to slot. */
    void* entries = ((VarArrayChunk*)node)->entries;
    void* dst = (uint8_t*)entries + (idx % cs) * es;
    memcpy(dst, value, (size_t)es);

    /* Update count = max(count, idx+1). */
    int32_t expected = a->count;
    while ((int64_t)idx + 1 > (int64_t)expected) {
        if (vfs_cas_i32((int32_t*)&a->count, expected, (int32_t)(idx + 1))) break;
        expected = a->count;
    }
    return 0;
}
```

`reserve_base` is the same but skips the memcpy.

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

### `count` semantics — clarified

**Dense mode** (today, before this phase): `count` = next-to-allocate index. Indices `[0, count)` are all valid.

**Sparse mode** (this phase): `count` = `max(allocated_idx) + 1`. Indices `[0, count)` may include holes where `var_array_resolve_base` returns NULL.

For all existing dense users (sequential `grow_base` calls): `count` semantics unchanged. No holes appear between `[0, count)`.

For new sparse users (calling `set_base(idx, value)` with `idx > old_count`): `count` jumps to `idx+1`. Subsequent `resolve_base` of indices between `(old_count, idx)` returns NULL.

**`var_array_resolve_base` is unchanged** — already returns NULL for both "idx >= count" and "intermediate node NULL". Same observable behavior.

### Backward compatibility

- All 59,487 existing var_array tests must pass unchanged.
- `var_array_grow_base` and `var_array_resolve_base` have identical external behavior to before.
- New `var_array_set_base` and `var_array_reserve_base` are pure additions.
- Existing dense callers (`var_array_append`, `var_array_lookup`, `var_array_update`) work unchanged.

## Files

| File | Change |
|------|--------|
| `src/var_array.h` | Declare `var_array_set_base`, `var_array_reserve_base`. |
| `src/var_array.c` | Implement `set_base` and `reserve_base`. Refactor `grow_base` to use `reserve_base`. |
| `test/test_var_array.c` | Add sparse-mode tests (set at high idx, holes, mixed sparse + dense). |

## Acceptance

- [ ] `var_array_set_base` and `var_array_reserve_base` declared in var_array.h.
- [ ] Both implemented in var_array.c.
- [ ] `var_array_grow_base` refactored to use `reserve_base` internally.
- [ ] All 59,487 existing var_array tests pass unchanged.
- [ ] At least 10 new sparse-mode tests added (set at idx > count, holes between allocations, mixed sparse + dense).
- [ ] All 10 unit test suites pass.

## Out of scope

- **Hash_map simplification**: phase 23 will use `set_base` to eliminate the manual grow coordination in `hash_map_grow` rehash and `hash_map_base_put` insert path.
- **`var_array_unset_base`**: zeroing out a slot. Hash_map uses state-encoded tombstones, not NULL slots. Defer until a use case appears.
- **Compaction**: freeing tree nodes when all slots in a chunk become empty. Var_array never shrinks today. Defer.
- **Concurrent writes to `count`**: existing CAS on count handles sequential `grow_base`. `set_base` uses CAS too. Single-writer assumed; same as before.
- **Migration of existing dense users**: none needed. Dense users get the same behavior.

## Risks

- **`count` semantics shift**: a sparse user sees `count` jump to `max(idx)+1`. Existing dense users see no change. Test must cover this distinction.
- **Performance regression in adversarial cases**: if hash distribution maps every key to bucket `new_capacity-1`, the loop does `new_capacity` grow calls, same as pre-allocate. No regression.
- **Atomic ops**: `set_base` does multiple CASes (one per tree level + one for count). Same as `grow_base`'s CAS pattern.
- **Refactor risk on `grow_base`**: even though external behavior is identical, the implementation now goes through `reserve_base`. Any bug in the refactor could break dense users. Mitigation: keep all 59,487 tests as the regression gate.

## Implementation order

1. Add `var_array_set_base` and `var_array_reserve_base` declarations to var_array.h.
2. Implement them in var_array.c (extracting the shared tree-walk code).
3. Refactor `var_array_grow_base` to use `reserve_base`.
4. Build and run all 59,487 var_array tests — must still pass.
5. Add sparse-mode tests to test_var_array.c.
6. Build, run all 10 unit test suites — no regression.