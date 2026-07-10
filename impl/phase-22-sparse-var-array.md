# Phase 22: Sparse VarArray with Typed `var_array_set`

## Goal

Generalize `var_array` from a dense array (sequential `[0, count)`) to a sparse array (any index `[0, capacity)` may be set independently). Add a typed `var_array_set(arr, idx, entry)` macro that allocates the tree path if needed and writes the value. The macros remain the public API — base functions stay internal.

## Background

Today `var_array` has a typed macro API plus internal `*_base` C functions:

**External (macros)** — what code actually uses:
- `var_array_new(T)` — allocate typed array
- `var_array_delete(a)` — free
- `var_array_append(a, entry)` — claim next sequential idx, write entry
- `var_array_update(a, idx, entry)` — write to existing idx, silent-drop if missing
- `var_array_lookup(a, idx)` — read

**Internal (`*_base`)** — implementation details:
- `var_array_new_base`, `var_array_delete_base`
- `var_array_grow_base` — atomic-add sequential idx, allocate path
- `var_array_resolve_base` — walk tree to chunk for idx

The chunk tree already supports sparse allocation internally (intermediate nodes can be NULL until first access, resolver returns NULL for unallocated paths), but the public API doesn't expose this.

Callers that want to write to a specific index currently have no clean option:
- `var_array_update` silently drops if the slot doesn't exist
- `var_array_append` always writes at `count`, no way to skip ahead

hash_map today (Phase 21) works around this with ~30 lines of manual grow coordination in `hash_map_grow` rehash and `hash_map_base_put` insert path. Those will collapse to `var_array_set(arr, idx, slot)` in Phase 23.

## Design

### The unifying primitive

**`var_array_set_base(a, idx, value_ptr)`** (internal, in `.c`):

1. Promote root to required height for `idx` if needed (rebalance: existing chunks re-pointed at new top level's slot 0; chunk contents unchanged).
2. Walk tree from root to leaf, allocating missing intermediate nodes only (siblings stay NULL).
3. Update `count = max(count, idx+1)` via CAS.
4. Write `entry_size` bytes from `value_ptr` to slot `idx`.

This single primitive does both insert (when path doesn't exist) and update (when path exists). It's the single internal write primitive.

### External API (the macros)

**Renamed**:
```c
// Old: var_array_update(a, idx, entry) — silent drop if slot missing
// New:
#define var_array_set(a, idx, entry) \
    var_array_set_base((VarArrayBase*)(a), (idx), &(entry))
```

`var_array_set` always succeeds (modulo OOM). It allocates the path if the slot doesn't exist, then writes the value. For existing slots, it's an overwrite.

**Unchanged**:
```c
#define var_array_append(a, entry) ({ \
    int _idx = (a)->count; \
    typeof(entry) _entry = (entry); \
    var_array_set_base((VarArrayBase*)(a), _idx, &_entry); \
    _idx; \
})
```

`var_array_append` becomes a thin wrapper that calls `set_base` at the current count. Atomic-add for concurrent appenders can be retained via the internal `grow_base` helper (see below).

```c
#define var_array_lookup(a, idx) ({ \
    void* _rp = var_array_resolve_base((VarArrayBase*)(a), (idx)); \
    _rp ? &((VarArrayChunk_T(typeof(*(a)->root))*)_rp)->entries[(idx) % (a)->chunk_size] : NULL; \
})
```

Unchanged.

### Internal `*_base` API

The `*_base` functions are **implementation details**. They stay in the `.c` file (or stay in the header but documented as internal). External code must not call them directly.

```c
/* In var_array.c, static or file-private: */
static int var_array_set_base(VarArrayBase* a, int idx, const void* value);
```

The header keeps the existing `_base` declarations for the macros' use:
- `var_array_new_base`, `var_array_delete_base` (used by typed `var_array_new`/`delete`)
- `var_array_grow_base` (used by `var_array_append` for atomic sequential claim)
- `var_array_resolve_base` (used by `var_array_lookup`)
- `var_array_set_base` (used by `var_array_set`)

External code never calls these directly. The `_base` suffix and the lack of typed wrappers signal "internal".

### Path allocation semantics

When `set_base(idx, value)` is called:

1. **Determine required tree height for `idx`**: existing algorithm — count levels needed to address `idx`.

2. **Promote root to required height** if needed. **Rebalancing**: existing chunks get re-pointed into the new top level's slot 0. Chunk contents (the actual entries) are unchanged — only the tree structure shifts. Same rebalance that `grow_base` already performs.

3. **Allocate the path from root to leaf**:
   - At each level, look at the slot corresponding to `idx`.
   - If the slot is NULL, allocate a new level node (or leaf chunk) and CAS-install it.
   - **Sibling slots stay NULL** — only the path is allocated.

   Example: 3-level array (chunks of 256 → 16M total slots), set index 100:
   - Level 2 has 16 children (chunks 0-15).
   - Index 100 → level 2 slot 0 (chunks 0-255), level 1 chunk for indices 0-255.
   - If level 2 slot 0 is NULL: allocate level-1 chunk for indices 0-255, set slot 100 inside.
   - **All other level-2 slots (1-15) remain NULL**.

4. **Update `count`** to `max(count, idx+1)` (CAS).

### `count` semantics

**Dense mode** (sequential `grow_base` calls): `count` = next-to-allocate index. Indices `[0, count)` are all valid.

**Sparse mode** (calls to `set_base(idx, value)` with `idx > old_count`): `count` = `max(allocated_idx) + 1`. Indices `[0, count)` may include holes where `var_array_resolve_base` returns NULL.

For all existing dense users (sequential `grow_base` calls): `count` semantics unchanged. No holes appear between `[0, count)`.

For new sparse users (calling `set_base(idx, value)` with `idx > old_count`): `count` jumps to `idx+1`. Subsequent `resolve_base` of indices between `(old_count, idx)` returns NULL.

**`var_array_resolve_base` is unchanged** — already returns NULL for both "idx >= count" and "intermediate node NULL". Same observable behavior.

### Backward compatibility

- All 59,487 existing var_array tests must pass after rename (`var_array_update` → `var_array_set`).
- 4 `var_array_update` call sites in test_var_array.c need rename.
- 1 test function rename (`test_var_array_update_in_place` → `test_var_array_set_in_place`).
- Existing dense callers (`var_array_append`, `var_array_lookup`, `var_array_new`, `var_array_delete`) work unchanged.

## Files

| File | Change |
|------|--------|
| `src/var_array.h` | Rename `var_array_update` macro → `var_array_set`. Declare `var_array_set_base`. Update `var_array_append` macro to use `set_base` instead of `grow_base + resolve_base + manual write`. |
| `src/var_array.c` | Implement `var_array_set_base` (the unifying primitive). Refactor `var_array_grow_base` to use `set_base` for the path allocation (or keep it as a thin atomic-add wrapper). |
| `test/test_var_array.c` | Rename 4 `update` call sites to `set`. Update test name. Add sparse-mode tests. |

## Acceptance

- [ ] `var_array_set_base` declared in var_array.h (and `static` in `.c` so external code can't link to it).
- [ ] `var_array_set_base` implemented: reserves path + writes value, updates count.
- [ ] `var_array_update` macro removed; `var_array_set` macro added.
- [ ] `var_array_append` macro simplified to use `set_base` (or retains atomic-add via `grow_base` for concurrency).
- [ ] All 59,487 existing var_array tests pass after rename.
- [ ] At least 10 new sparse-mode tests added (set at idx > count, holes, mixed, rebalancing).
- [ ] All 10 unit test suites pass.

## Out of scope

- **Hash_map simplification**: phase 23 will use `var_array_set` to eliminate the manual grow coordination in `hash_map_grow` rehash and `hash_map_base_put` insert path.
- **`var_array_unset_base`**: zeroing out a slot. Defer until a use case appears.
- **Compaction**: freeing tree nodes when all slots in a chunk become empty. Var_array never shrinks today. Defer.
- **Migration of existing dense users**: none needed. Dense users get the same behavior.

## Risks

- **`var_array_set` is a behavior change from `var_array_update`**: any caller relying on the silent-drop behavior will now get an allocation (or OOM). 4 call sites in tests are easy to update; no other callers exist.
- **`count` semantics shift**: a sparse user sees `count` jump to `max(idx)+1`. Existing dense users see no change. Test must cover this distinction.
- **Performance regression in adversarial cases**: if hash distribution maps every key to bucket `new_capacity-1`, the loop does `new_capacity` grow calls, same as pre-allocate. No regression.
- **Atomic ops**: `set_base` does multiple CASes (one per tree level + one for count). Same as `grow_base`'s CAS pattern.

## Implementation order

1. Add `var_array_set_base` declaration to var_array.h (and make it `static` in `.c`).
2. Implement `var_array_set_base` in var_array.c — the unifying primitive.
3. Refactor `var_array_grow_base` to delegate path allocation to `set_base` (or keep `grow_base` as a thin atomic-add wrapper that calls `set_base` for the path part).
4. Rename `var_array_update` macro → `var_array_set` in var_array.h.
5. Update `var_array_append` macro to use `set_base`.
6. Update 4 call sites in test_var_array.c (rename `update` → `set`).
7. Build and run all 59,487 var_array tests — must still pass.
8. Add sparse-mode tests to test_var_array.c.
9. Build, run all 10 unit test suites — no regression.