# Phase 23: HashMap as Thin Layer Over Sparse VarArray

## Goal

Replace `hash_map`'s complex implementation (open-addressing with own `HashSlot` struct, FNV-1a hashing, load-factor tracking, resize/rehash logic, tombstone management) with a **thin layer** over Phase 22's sparse `VarArray`. The hash_map becomes: a fixed-capacity ring buffer + a hash function + linear-probe collision handling. No resize, no rehash, no separate struct.

## Background

Today `hash_map` (Phase 21) is a self-contained primitive:
- `HashSlot` struct (key + value + state)
- `VarArray(HashSlot)` for storage
- FNV-1a 64-bit hash on raw key bytes
- Load factor 0.75, capacity doubles on overflow
- Resize pre-grows new array and rehashes all entries
- Robin Hood tombstone reuse for delete

This is over-engineered for the common use case. With Phase 22's `var_array_set` (allocates path on demand, no upfront cap), we can have a **fixed-capacity hash table** that:
- Never resizes (the "very large" capacity is plenty)
- Uses `var_array_set(arr, slot, slot_entry)` to write
- Uses `var_array_lookup(arr, slot)` to read
- Sparse storage means we only allocate chunks for actually-used slots

The result: hash_map implementation shrinks from ~250 LOC to ~80 LOC, with simpler reasoning and same external API.

## Design

### Configurable capacity and granularity

Two log2-based parameters, both fit in a small integer:

- **`scale`** (range 1..32): capacity = `2^scale` slots. Default 20 = 1M slots.
- **`granularity`** (range 1..16): chunk_size = `2^granularity` entries per chunk. Default 8 = 256 entries/chunk.

Trade-offs:

- **Higher scale** → more capacity, more memory in worst case. Scale 32 = 4B slots = 96GB worst case.
- **Lower scale** → less capacity, less memory. Scale 16 = 65K slots.
- **Higher granularity** → fewer tree levels (less CAS promotion), but larger chunks waste memory in sparse workloads.
- **Lower granularity** → deeper tree (more CAS), but tighter memory in sparse workloads.

Defaults chosen for the "very large sparse" use case: scale=20 (1M slots) + granularity=8 (256/chunk). With 6500 typical entries: ~26 chunks used, ~150KB. Sparse-friendly.

For dense workloads (500K+ entries), bump granularity to 12 or 14 to reduce tree depth. For ultra-sparse (thousands of entries in huge capacity), lower granularity is fine.

### Data structure — slim

```c
typedef struct {
    int64_t key;
    int64_t value;
    int8_t  state;        /* 0=EMPTY, 1=OCCUPIED, 2=TOMBSTONE */
} HashSlot;
```

Stored in `VarArray(HashSlot)` via `var_array_set(slots, slot_idx, &entry)`. The var_array handles sparse allocation — we never allocate chunks for slots that aren't in any probe path.

**Wait** — with this approach, we still need the `state` field. Why? Because `var_array_set(arr, idx, value)` doesn't distinguish "EMPTY (never written)" from "OCCUPIED with value" from "TOMBSTONE (cleared)". We'd need:

- **EMPTY** (slot was never set): `var_array_lookup` returns NULL or non-NULL with `*slot = 0` (calloc'd). Hard to distinguish from a slot that was `set` to all-zeros.
- **OCCUPIED**: slot was set via `var_array_set`.
- **TOMBSTONE**: slot was `var_array_unset` after being set.

The state field encodes these in the entry itself. The hash_map logic checks `slot->state` to decide:
- EMPTY → not in map, can insert here
- OCCUPIED + key matches → found, return value
- OCCUPIED + key doesn't match → continue probing
- TOMBSTONE → not in map, but can insert here (preserves probe path)

The state field makes `HashSlot` 24 bytes (16 + 1 + padding). That's fine.

Alternatively, **drop the state field** and use `var_array_unset` for tombstones:
- Insert: `var_array_set(arr, slot, entry)` (allocates path if needed)
- Lookup: walk probe, compare keys. If slot is unallocated, we know it's EMPTY (lookup returns NULL or non-NULL with zero — but we can't easily distinguish).
- Delete: `var_array_unset(arr, slot)` — but unset does NOT allocate path, so it can fail to mark a slot that wasn't set.

Hmm. Without state field, we lose the distinction between:
- "Slot exists but is deleted" (TOMBSTONE) → probe should continue past
- "Slot never existed" (EMPTY) → probe should stop

If we drop tombstones entirely and use EMPTY = deleted, the linear probe breaks:
- Insert A at slot 5
- Delete A (slot 5 → EMPTY)
- Lookup A → probe starts at hash(A), reaches slot 5 (now EMPTY), stops, returns "not found"
- But if a key B is later inserted at slot 5 (hash collision), lookup A would find B instead

This is the classic "deleted slot" problem. Tombstones solve it.

So **state field is needed** for correctness of delete. Keep the 24-byte HashSlot.

### Hash function

FNV-1a 64-bit on raw key bytes, same as today. Modulo `& (capacity - 1)` for fast bitwise hash.

### Operations

**`put(key, value)`**:
```c
i = hash(key) & (cap - 1)
first_tombstone = -1
while (1):
    e = var_array_lookup(slots, i)
    if (!e || e->state == EMPTY):
        target = (first_tombstone >= 0) ? first_tombstone : i
        var_array_set(slots, target, entry_with_state_OCCUPIED)
        size++
        return 1
    if (e->state == TOMBSTONE):
        if (first_tombstone < 0) first_tombstone = i
    else if (e->key == key):
        e->value = value  // update
        return 0
    i = (i + 1) & (cap - 1)
```

**`get(key)`**:
```c
i = hash(key) & (cap - 1)
while (1):
    e = var_array_lookup(slots, i)
    if (!e || e->state == EMPTY) return NULL
    if (e->state == OCCUPIED && e->key == key) return &e->value
    i = (i + 1) & (cap - 1)
```

**`delete(key)`**:
```c
i = hash(key) & (cap - 1)
while (1):
    e = var_array_lookup(slots, i)
    if (!e || e->state == EMPTY) return 0
    if (e->state == OCCUPIED && e->key == key):
        entry = { .key = key, .value = 0, .state = TOMBSTONE }
        var_array_set(slots, i, entry)  // overwrites with tombstone
        size--
        tombstones++
        return 1
    i = (i + 1) & (cap - 1)
```

### No resize — ever

With fixed capacity 2^20, the load factor stays low for any realistic use. **No resize logic**. The hash_map is a fixed-size table.

If a caller fills the table to >75% (786K entries), probe sequences degrade but correctness is maintained. They'd need to switch to a different data structure.

### Tombstones

Tombstones accumulate over time. With no resize, we can't rehash to clear them. Options:
1. **Track tombstone count**. When tombstones > threshold, refuse further deletes.
2. **Lazy clear**: when load factor (size + tombstones) / capacity > 0.5, sweep tombstones (walk table, rebuild without tombstones).
3. **No tombstone tracking**: just trust that delete is rare.

For phase 23, **option 3** is simplest. Track `size` only. Tombstones don't grow the "live" count; they're just markers. If a long-running app does many inserts and deletes, tombstones accumulate and probe sequences degrade. This is acceptable for the typical use case.

For the **dedup use case**, no deletes happen at all (chain walk is read-only). Tombstones are never created.

### Iteration

Same as Phase 21: walk 0..count, skip non-OCCUPIED slots.

```c
HashMapIterator it = {0};
while (hash_map_iter_next(&it, map, &key, &value)) { ... }
```

With Phase 22's sparse var_array, `count` is the high-water mark of set operations. For hash_map's "very large array", `count` = highest occupied slot + 1. The iterator walks 0..count and skips non-OCCUPIED slots — same as Phase 21.

### API

Same external API as Phase 21:

```c
HashMap(K, V) hash_map_new(K, V);
/* Defaults: scale=20 (1M slots), granularity=8 (256/chunk) */

HashMap(K, V) hash_map_new_cap(K, V, int scale, int granularity);
/* scale: 1..32, capacity = 2^scale
   granularity: 1..16, chunk_size = 2^granularity */

void hash_map_free(HashMap(K, V) map);
int  hash_map_put(HashMap(K, V) map, K key, V value);
V*   hash_map_get(HashMap(K, V) map, K key);
int  hash_map_contains(HashMap(K, V) map, K key);
int  hash_map_delete(HashMap(K, V) map, K key);
int64_t hash_map_size(HashMap(K, V) map);
```

**API change from Phase 21**: `hash_map_new_cap(K, V, initial_capacity)` (linear capacity) is replaced by `hash_map_new_cap(K, V, scale, granularity)` (log2 parameters). Both encode the same information; the new form is cleaner for power-of-2 semantics.

No other API changes. The deleted-old-capacity API had only one caller (`test_hash_map.c`) which is updated.

## Files

| File | Change |
|------|--------|
| `src/hash_map.h` | Update comments to reflect new design (var_array-backed, fixed capacity). No API change. |
| `src/hash_map.c` | Replace `hash_map_grow` with simpler logic. Use `var_array_set` for put. Use `var_array_set` for tombstone write. Drop `hash_map_base_new_cap` floor — capacity is whatever the caller picks. |

## What goes away

- **`hash_map_grow`**: no resize logic. Capacity is fixed.
- **Load factor tracking**: still tracked (for tombstone reuse in put), but no automatic resize.
- **Pre-grow loop in `hash_map_base_put`**: replaced with `var_array_set` directly.
- **Manual lazy grow in `hash_map_grow`**: replaced with `var_array_set`.

## What stays

- `HashSlot` struct (key + value + state)
- FNV-1a 64-bit hash
- Linear probe with Robin Hood tombstone reuse
- Iterator over occupied slots
- Thread-safety model (single-writer, lock-free reads — same as Phase 21)

## LOC reduction

Phase 21's `hash_map.c`: ~270 LOC (with sparse allocate, manual grow loops, hash_map_grow).
Phase 23 target: ~80-100 LOC.

## Acceptance

- [ ] `hash_map.c` rewritten using `var_array_set` for put/delete-tombstone.
- [ ] No `hash_map_grow` function (no resize).
- [ ] Capacity and granularity configurable via `hash_map_new_cap(scale, granularity)`. Defaults: scale=20, granularity=8.
- [ ] All 45,942 existing hash_map tests pass unchanged (after API rename `initial_capacity` → `scale, granularity`).
- [ ] All 10 unit test suites pass.
- [ ] LOC of `hash_map.c` ≤ 120 (excluding comments).

## Out of scope (explicit non-goals)

- **Automatic resize**: not implemented. With 2^20 capacity, the load factor stays low for all realistic use.
- **Concurrent resize**: not implemented. Same single-writer model as Phase 21.
- **Tombstone sweep**: not implemented. Acceptable for typical use; dedup use case never deletes.
- **VFS integration** (using hash_map in `dirchain_list_all`): phase 24. This phase just simplifies the hash_map primitive itself.
- **Drop tombstones**: not in this phase. Tombstones are still needed for delete correctness.

## Risks

- **Capacity too small**: if a caller exceeds 75% of capacity, probe sequences degrade. For 2^20 = 1M slots, that's 786K entries before degradation. Realistic uses stay far below.
- **Hash quality**: FNV-1a is fine for integer keys, may be adversarial for string keys (not used in this codebase).
- **Memory for large dirs**: at 24B/slot and 2^20 slots, worst case is 24MB if every slot is allocated. Sparse allocation means actual memory = `O(unique_keys × 24B)`.

## Implementation order

1. Update `src/hash_map.c` to use `var_array_set` directly.
2. Remove `hash_map_grow` and the manual grow coordination.
3. Update `hash_map_new_cap` API to take `(scale, granularity)` instead of `initial_capacity`.
4. Update `test_hash_map.c` for the API rename.
5. Run all existing hash_map tests — must still pass (semantic compatibility).
6. Run all 10 unit test suites — no regression.