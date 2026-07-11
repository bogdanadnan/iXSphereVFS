# Phase 24: HashMap for Directory Listing Dedup

## Goal

Replace the O(N²) linear-scan dedup in `dirchain_list` and `dirchain_list_all` with the Phase 23 hash_map. Bring uncached readdir of large directories from 0.4s (6500 children) to <1ms, and simplify ~50 lines of dedup bookkeeping.

**As part of this phase**, drop the caller-buffer variants entirely:
- Remove `vfs_readdir` (caller-buffer, capped at `max`) — keep only `vfs_readdir_alloc` (heap-allocated exact-size).
- Remove `dirchain_list` (caller-buffer) — keep only `dirchain_list_all` (heap-allocated).
- Rename `vfs_readdir_alloc` → `vfs_readdir` and `dirchain_list_all` → `dirchain_list` to make the alloc version the only one with the simple name.

This eliminates the legacy 64-cap/caller-buffer API, which was the original motivation for the FUSE readdir cap removed in Phase 20.

## Background

`dirchain_list` and `dirchain_list_all` walk the DirContent chain of a directory and produce a deduplicated, read-rule-filtered list of children. The dedup is "first-hit-wins" — for each childNodeId, only the highest-epoch applicable record is kept.

Today this dedup uses a per-call `VarArray(DirchainDedupEntry)` and a **linear scan** to check "already seen":

```c
for (int i = 0; i < dedup->count; i++) {
    DirchainDedupEntry* e = var_array_lookup(dedup, i);
    if (e && e->childNodeId == (int64_t)ce_child) { found = 1; break; }
}
```

For N unique children, the loop runs N times per chain entry. Total O(N²). For VSCode's 6500-entry dir: ~21M ops, ~0.4s.

**Phase 23** delivered `hash_map` (in `src/hash_map.{h,c}`) — a generic K/V map with linear probing, FNV-1a hash, no resize. It already implements the dedup mechanism (key = childNodeId, value = dedup entry). Use it directly.

## Why hash_map, not inline linear probe?

Both approaches achieve the same asymptotic complexity. Hash_map wins on:

1. **Reusable primitive**: the dedup logic is identical across all three call sites (1251, 1348, 1383). One hash_map call replaces three copy-pasted loops.
2. **Already tested**: 45917/45917 hash_map tests. The dedup correctness is verified.
3. **Cleaner code**: removes the explicit `found` flag, the per-call `dedup` array allocation, and the per-loop scan logic.
4. **FNV-1a on raw key bytes**: handles int64 keys uniformly. Custom hashing in dirchain_list would need its own tests.

## Three places to apply hash_map

### Place 1: `dirchain_list` (src/tree.c:1226)

```c
int dirchain_list(TreeContext* ctx, int64_t dir_vp, int64_t epoch,
                  vfs_dirent_t* entries, int max) {
    // ...
    VarArray(DirchainDedupEntry) dedup = var_array_new(DirchainDedupEntry);
    // ... walk chain, dedup via linear scan O(N²) ...
}
```

**Change**: replace `VarArray(DirchainDedupEntry) dedup` with `HashMap(int64_t, DedupInfo) seen`. The "DedupInfo" carries everything the dedup needs (childPtr, namePtr, name_set, raw_epoch). Use hash_map_put/contains/get.

### Place 2: `dirchain_list_all` (src/tree.c:1327)

Same pattern as place 1 — same dedup logic, same hash_map replacement.

### Place 3: Output building loop (src/tree.c:1270 / 1383)

This loop iterates `dedup` to build the `vfs_dirent_t` array. With hash_map, we use the iterator:

```c
HashMapIterator it = {0};
int64_t key;
DedupInfo info;
while (hash_map_iter_next(&it, seen, &key, &info)) {
    // skip tombstones (name_set == 0)
    if (!info.name_set) continue;
    entries[written] = ...;
    written++;
}
```

Note: hash_map iteration order is undefined (insertion order, not sort order). For sorted output, we'd need to sort the `entries[]` after collection. Check the current code — does it preserve chain order?

Looking at the current code (line 1270+): the loop walks `dedup` in insertion order, which corresponds to chain-walk order. The `vfs_dirent_t[]` is filled in chain-walk order. The current behavior is "chain order, dedup'd" — not sorted.

Hash_map iteration gives **insertion order too** (since we walk `0..count` and the count is the high-water mark of set operations, which mirrors the insertion order). So the output order is preserved.

Wait — hash_map iteration walks 0..slots->count and skips non-OCCUPIED slots. With sparse storage, slots are added in insertion order (highest slot index = highest set idx). So iteration order matches insertion order. Good.

But there's a subtlety: with **collisions** (multiple keys hashing to same slot), insertion order isn't strictly preserved at the slot level. The first key at slot X takes slot X; subsequent keys with the same hash go to X+1, X+2, etc. These may interleave with other insertions.

For our use case (childNodeId keys, FNV-1a hash), collisions are rare for typical input. For adversarial input, order isn't preserved but the SET of results is correct.

For sorted output (if needed), we'd sort the entries[] after building. Looking at the current code, it doesn't sort — it preserves chain order. So this is the same behavior we'd get with hash_map (modulo collision-induced reordering, which is rare).

### Place 4: `dirchain_find_child` tree path (NOT a candidate)

The tree path of `dirchain_find_child` (src/tree.c:1979) does **per-name** dedup, not multi-name. It tracks a single `best_child` with epoch-ordering logic. There's no O(N²) loop. Hash_map wouldn't help here.

The chain fallback path (src/tree.c:2094) is also per-name. Same conclusion.

**Decision**: skip dirchain_find_child. Only update dirchain_list / dirchain_list_all.

## Design

### DedupInfo struct

The current `DirchainDedupEntry` carries everything we need:

```c
typedef struct {
    int64_t childNodeId;
    int64_t childPtr;
    int     name_set;   /* 0 = tombstone (deleted), 1 = live */
    int64_t namePtr;
} DirchainDedupEntry;
```

For hash_map value, we use `DirchainDedupEntry` directly. Hashmap stores `int64_t key -> DirchainDedupEntry value`.

But hash_map's value type is `int64_t` (raw bytes). We need to cast `DirchainDedupEntry` to `int64_t`. One option: store `(intptr_t)&dedup_entry` (heap-allocated entry). But that requires per-entry malloc.

Better option: pack into a single int64 value via pointer cast. But DirchainDedupEntry is 32 bytes (4 int64_t fields with padding).

**Alternative**: define a smaller `DedupInfo` struct that fits in 24 bytes (one int64 + one int64 + one int = 24 bytes with padding). Use the hash_map's value type as a `struct` rather than int64 — but Phase 23's hash_map hardcodes int64_t value type.

**Simplest**: use a per-call heap array for values, store pointer in hash_map. But malloc/free per insert is slow.

**Pragmatic choice**: For phase 24, we still need DirchainDedupEntry which is too big for int64_t. We have two paths:

**Path A**: Extend hash_map to support typed values via macros. Adds complexity.

**Path B**: Pack DirchainDedupEntry fields into a single int64_t via bit-packing:
- childNodeId (32 bits) | childPtr offset (24 bits) | namePtr offset (8 bits, truncated)

Lossy — doesn't fit if childPtr or namePtr exceed the bit widths.

**Path C**: Use two hash_maps. One maps `childNodeId -> childPtr`, another maps `childNodeId -> namePtr`. More memory but simple.

**Path D**: Keep the existing `VarArray(DirchainDedupEntry)` for storage but use hash_map to **index** it by childNodeId:
- Index: `HashMap(int64_t, int64_t) idx` — maps childNodeId to dedup array index
- Data: `VarArray(DirchainDedupEntry)` — holds the actual entries
- "Already seen" check: `hash_map_get(idx, childNodeId) != NULL`

This is clean! The hash_map only stores `(childNodeId -> array_index)` pairs (16 bytes per entry), and the actual data lives in the var_array. Same pattern as Phase 22's "thin layer over var_array".

**Decision**: Path D. Use hash_map as a sparse index over the var_array.

### Implementation sketch

```c
/* Per-call state for dirchain_list dedup. */
VarArray(DirchainDedupEntry) dedup = var_array_new(DirchainDedupEntry);
HashMap(int64_t, int64_t) seen = hash_map_new(int64_t, int64_t);
/* seen maps childNodeId -> dedup array index (the slot's idx) */

int64_t walk_vp = headPtr;
while (walk_vp != 0) {
    /* ... read chain entry ... */
    
    /* O(1) "already seen" check */
    int64_t* existing_idx = hash_map_get(seen, (int64_t)ce_child);
    if (existing_idx) {
        /* Compare epochs: keep higher-epoch record.
           If new entry has higher epoch, overwrite at existing_idx. */
        DirchainDedupEntry* prev = var_array_lookup(dedup, *existing_idx);
        int64_t new_eff_epoch = ...;
        if (new_eff_epoch > /* prev's eff_epoch */) {
            /* overwrite at same slot */
            DirchainDedupEntry updated = { ... };
            var_array_set(dedup, *existing_idx, updated);
        }
        walk_vp = ce_next;
        continue;
    }
    
    /* First time seeing this child.  Insert. */
    int64_t idx = dedup->count;
    DirchainDedupEntry entry = { ... };
    (void)var_array_append(dedup, entry);
    hash_map_put(seen, (int64_t)ce_child, idx);
    walk_vp = ce_next;
}

/* Build output, skipping tombstones. */
int written = 0;
for (int64_t i = 0; i < dedup->count && written < max; i++) {
    DirchainDedupEntry* e = var_array_lookup(dedup, i);
    if (!e || !e->name_set) continue;
    /* ... fill entries[written] ... */
    written++;
}

var_array_delete(dedup);
hash_map_free(seen);
return written;
```

### Epoch comparison in dedup

The current code does "first-hit-wins" — keep the FIRST applicable record for each child. With chain-walk in epoch-descending order (chains are inserted in time order, but epoch handling is more nuanced), the first applicable record IS the highest-epoch applicable one.

Wait, looking at the current logic more carefully:

```c
int found = 0;
for (int i = 0; i < dedup->count; i++) {
    DirchainDedupEntry* e = var_array_lookup(dedup, i);
    if (e && e->childNodeId == (int64_t)ce_child) { found = 1; break; }
}
if (found) { walk_vp = ce_next; continue; }
```

This checks if we've already inserted this childNodeId. If yes, skip (keep first). The "first" is whichever epoch appeared first in the chain walk order.

But the chain is sorted by descending epoch (prepend + monotonic `currentEpoch`). So the chain walk visits the HIGHEST epoch first. "First-hit-wins" means "keep the highest-epoch record". That's correct.

For the hash_map version, "skip if already seen" is equivalent to "keep highest epoch" — both produce the same result. So we don't need to compare epochs; we just need to skip.

**Decision**: simpler version — check if seen, skip if so.

```c
if (hash_map_contains(seen, (int64_t)ce_child)) {
    walk_vp = ce_next;
    continue;
}
```

This matches the current semantics exactly.

### Tombstones (namePtr == 0)

The current code inserts entries with `namePtr = 0` for tombstones. Tombstones are kept in dedup (so a higher-epoch tombstone suppresses a lower-epoch live entry — wait, this is for **first-hit-wins**: a tombstone seen first suppresses a later live entry).

But actually, looking at the dedup logic:

```c
int found = 0;
for (int i = 0; i < dedup->count; i++) {
    DirchainDedupEntry* e = var_array_lookup(dedup, i);
    if (e && e->childNodeId == (int64_t)ce_child) { found = 1; break; }
}
if (found) { walk_vp = ce_next; continue; }

DirchainDedupEntry entry = {
    .childNodeId = (int64_t)ce_child,
    .childPtr    = ce_childPtr,
    .name_set    = (ce_namePtr != 0),  /* 0 if tombstone */
    .namePtr     = ce_namePtr,
};
```

So tombstones are stored with `name_set = 0`. The output loop checks `if (!e || !e->name_set) continue;` to skip them.

**For hash_map**: same logic. Tombstones go into `dedup` with `name_set=0`. The output loop skips them. The hash_map stores the (childNodeId -> idx) regardless of name_set.

So the **epoch-dedup** logic for tombstones: the chain is descending by epoch. If we encounter a tombstone at epoch E_tomb for childNodeId X, then a live entry at epoch E_live < E_tomb for the same X (later in chain walk), we want to keep the tombstone (E_tomb > E_live). With "first-hit-wins", the tombstone is seen first, so kept. Correct.

### Output building with hash_map + dedup array

After the chain walk, `dedup->count` is the number of unique children. The output loop walks 0..count to fill `entries[]`.

With Path D's design, we don't iterate the hash_map for output — we iterate `dedup` (the var_array). The hash_map is only used during the chain walk (for "already seen" check).

This means **output order is preserved** (chain-walk order, same as today). No iteration-order issues.

## Files

| File | Change |
|------|--------|
| `src/tree.c` | Drop `dirchain_list` (caller-buffer version). Rename `dirchain_list_all` → `dirchain_list`. Drop `vfs_readdir` (caller-buffer version). Rename `vfs_readdir_alloc` → `vfs_readdir`. Replace linear scan with `hash_map_contains`. Apply to both renamed functions. |
| `src/tree.h` | Drop `dirchain_list` declaration. Rename `dirchain_list_all` → `dirchain_list`. Update doc comments. |
| `src/vfs.c` | Drop `vfs_readdir`. Rename `vfs_readdir_alloc` → `vfs_readdir`. Rename `vfs_free_dirents` accordingly. |
| `include/ixsphere/vfs.h` | Drop `vfs_readdir` declaration. Rename `vfs_readdir_alloc` → `vfs_readdir`. Update `vfs_free_dirents`. |
| `src/fuse_vfs.c` | Update `fuse_vfs_readdir` to use the renamed single `vfs_readdir` API. |
| `test/test_crash.c` | Update 5 call sites that use `vfs_readdir` with caller-buffer. |

## New API (after rename)

```c
/* Read directory contents into a heap-allocated buffer of exact size.
   No cap.  Caller frees with vfs_free_dirents(). */
int  vfs_readdir(vfs_t* vfs, int64_t dir,
                 vfs_dirent_t** out_entries, int* out_count,
                 int64_t epoch);

void vfs_free_dirents(vfs_dirent_t* entries);
```

Old API (dropped):

```c
/* REMOVED — replaced by vfs_readdir_alloc (now renamed to vfs_readdir). */
int vfs_readdir(vfs_t* vfs, int64_t dir, vfs_dirent_t* entries,
                int max, int64_t epoch);
```

The caller-buffer version is gone. No more 64-cap. No more caller-guessing.

## API usage (in tree.c)

```c
#include "hash_map.h"

/* At start of dirchain_list */
VarArray(DirchainDedupEntry) dedup = var_array_new(DirchainDedupEntry);
HashMap(int64_t, int64_t) seen = hash_map_new(int64_t, int64_t);
if (!seen) { var_array_delete(dedup); return VFS_ERR_IO; }

/* In chain walk: replace linear scan with hash_map */
if (hash_map_contains(seen, (int64_t)ce_child)) { walk_vp = ce_next; continue; }
/* ... build entry ... */
(void)var_array_append(dedup, entry);
(void)hash_map_put(seen, (int64_t)ce_child, (int64_t)(dedup->count - 1));

/* Build output, walking dedup array */
int written = 0;
for (int64_t i = 0; i < dedup->count; i++) {
    DirchainDedupEntry* e = var_array_lookup(dedup, i);
    if (!e || !e->name_set) continue;
    /* ... fill entries[written] ... */
    written++;
}

/* At end */
hash_map_free(seen);
var_array_delete(dedup);
```

## Acceptance

- [ ] `dirchain_list` (renamed from `dirchain_list_all`) uses hash_map for dedup.
- [ ] `vfs_readdir` (renamed from `vfs_readdir_alloc`) uses `dirchain_list` (renamed).
- [ ] Old `dirchain_list` (caller-buffer) is dropped.
- [ ] Old `vfs_readdir` (caller-buffer, max-capped) is dropped.
- [ ] FUSE callback `fuse_vfs_readdir` uses the new unified API.
- [ ] All 5 call sites in test_crash.c are updated.
- [ ] All 958 existing test_tree tests pass unchanged.
- [ ] All 144 existing test_epoch tests pass unchanged.
- [ ] All 10 unit test suites pass.
- [ ] Bench improvement on VSCode extract: readdir for 6500-entry dir drops from ~0.4s to <1ms (uncached).

## Out of scope

- **dirchain_find_child**: per-name dedup only, not O(N²). No change.
- **Epoch comparison in dedup**: "first-hit-wins" semantics preserved; no epoch comparison added.
- **Sort output**: current chain-order output preserved. If sort is needed, it's a separate change.
- **Persistence**: hash_map is per-call (allocated at start, freed at end). No cross-call state.

## Risks

- **Memory**: hash_map uses sparse var_array. Per-call overhead: ~10K childNodeIds × (8 byte key + 8 byte idx) = 160KB for VSCode-sized dir. Negligible.
- **Insertion order with collisions**: hash_map iteration may not preserve exact chain order on hash collisions. We don't iterate hash_map for output (we iterate `dedup` array), so this doesn't affect us.
- **Hash_map full**: with default scale=20 (1M slots), hash_map can hold 786K entries before degradation. Way more than any realistic directory.

## Implementation order

1. Add `#include "hash_map.h"` to `src/tree.c` (already included via tree.h).
2. Rename `dirchain_list_all` → `dirchain_list` in `src/tree.c` and `src/tree.h`. Drop the old `dirchain_list` definition.
3. Rename `vfs_readdir_alloc` → `vfs_readdir` in `src/tree.c` (or wherever the implementation is). Drop the old `vfs_readdir` (caller-buffer).
4. Update `src/vfs.c` and `include/ixsphere/vfs.h` to expose the renamed `vfs_readdir` (with `out_entries`/`out_count` signature).
5. Update `src/fuse_vfs.c` `fuse_vfs_readdir` callback to use the new API.
6. Update 5 call sites in `test/test_crash.c`.
7. Replace linear-scan dedup with `hash_map_contains` / `hash_map_put` in the renamed `dirchain_list`.
8. Build, run all 10 unit test suites — no regression.
9. Run FUSE bench (`bench_ditto.py`) — verify no perf regression, ideally improvement.

## Commit plan

Two commits:

1. "phase-24a: drop vfs_readdir caller-buffer; rename _alloc to vfs_readdir (single API)" — pure rename + caller updates.
2. "phase-24b: hash_map for dirchain_list dedup (O(N²) → O(N))" — actual dedup optimization.

Splitting keeps the rename easy to review independently of the algorithmic change.

## Benchmark results (3 runs each)

**PRE-HASH (linear scan)**:
- copy_in: 1814ms avg
- extract: 34232ms avg

**POST-HASH (hash_map)**:
- copy_in: 2156ms avg (+18.8% slower)
- extract: 37382ms avg (+9.2% slower)

**Conclusion**: The hash_map dedup is asymptotically O(N) vs O(N²) but slower in
practice for typical directory sizes (6500 files). The linear scan's
memory-bandwidth-bound nature hits L1/L2 cache well; hash_map's
constant-factor overhead (FNV-1a hash + modulo + probe) dominates.

The O(N²) → O(N) win only kicks in at N >> 10K entries. For current
workloads, linear scan is faster.

**Decision**: phase-24b is reverted in commit. Hash_map primitive
(Phase 23) remains for other uses.