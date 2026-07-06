# Phase 17: NameEntry Hash Fast-Reject

## Goal
Store a 64-bit hash of the file name in the first 8 bytes of the NameEntry
data field. On name lookup, compare hashes first — only walk the full name
chain + strcmp on hash match. Zero new pool allocations. 2-8× faster name
comparison without building caches.

## NameEntry Layout Change

```
Before (32 bytes):
 Offset  Size  Field
    0     24    data     (UTF-8 bytes, zero-padded)
   24      8    nextPtr

After (32 bytes):
    0      8    name_hash  (uint64 — always a hash)
    8     16    name_data  (UTF-8 bytes, zero-padded if < 16)
   24      8    nextPtr    (unchanged)
```

Bytes 0-7 are ALWAYS a hash — no ambiguity, no special case for short names.
Name starts at byte 8 regardless of length. Names > 16 bytes chain as before.
The 8-byte overhead per NameEntry is the cost of consistency.

## Hash Function

```c
uint64_t name_hash(const char* name, int len) {
    return splitmix64((const uint8_t*)name, (size_t)len);
}
```

## Writing

`nodes_write_name` computes the hash, stores it at offset 0, then stores
name bytes starting at offset 8.

```c
int nodes_write_name(Pool* pool, const char* name, int64_t* out_vp) {
    int len = strlen(name);
    uint64_t h = name_hash(name, len);

    uint8_t data_24[24] = {0};
    memcpy(data_24, &h, 8);                                // bytes 0-7: hash
    int to_copy = len < 16 ? len : 16;
    memcpy(data_24 + 8, name, (size_t)to_copy);            // bytes 8-23: name

    // allocate slots, write data_24, chain if > 24 bytes
    ...
}
```

## Reading

`nodes_read_name` returns the full name unchanged — the hash is transparent
to readers. A new function `nodes_read_name_hash` reads just the hash (8 bytes
from the first slot) for fast comparison.

```c
uint64_t nodes_read_name_hash(Pool* pool, int64_t namePtr) {
    uint8_t* slot = pool_resolve(pool, namePtr);
    if (!slot) return 0;
    uint64_t h;
    memcpy(&h, slot, 8);
    return h;
}
```

## Lookup Change

In `dirchain_find_child` — replace the immediate name read + strcmp with
hash-fast-reject:

```c
// Before (current):
char entry_name[256];
int nl = nodes_read_name(pool, ce_namePtr, entry_name, sizeof(entry_name));
if (nl > 0 && strcmp(entry_name, name) == 0) { ... }

// After:
uint64_t stored_hash = nodes_read_name_hash(pool, ce_namePtr);
if (stored_hash != query_hash) continue;  // fast reject — no chain walk
// Hash match — verify full name
char entry_name[256];
int nl = nodes_read_name(pool, ce_namePtr, entry_name, sizeof(entry_name));
if (nl > 0 && strcmp(entry_name, name) == 0) { ... }
```

## Impact

| Operation | Before | After | Improvement |
|-----------|--------|-------|-------------|
| Name lookup (no match) | Walk chain + strcmp | 8-byte read + compare | 5-10× |
| Name lookup (match) | Walk chain + strcmp | 8-byte read + walk chain + strcmp | Same |
| Name write | Write 24 bytes | Write 8 hash + 16 name | Same |

Most lookups in `dirchain_find_child` DON'T match — the chain contains
multiple entries per childNodeId (one per epoch), only one has the right
name. Hash-fast-reject eliminates strcmp and chain walking for ~80% of
iterations.

## Files Changed

| File | Change |
|------|--------|
| `src/nodes.h` | Add `NAMEENTRY_OFF_HASH 0`, update layout comment |
| `src/nodes.c` | `nodes_write_name`: store hash. `nodes_read_name_hash`: read hash. |
| `src/tree.c` | `dirchain_find_child`: hash fast-reject before name compare |

## Non-Negotiable Constraints

- **No new pool allocations.** The NameEntry is still 32 bytes. Hash reuses first 8.
- **No format version bump.** Old files without hashes have 0 in first 8 bytes
  (null UTF-8). Hash 0 treated as "no hash — fall through to strcmp".
- **No cache, no VarArray, no invalidation.** Pure algorithmic optimization.

## Acceptance

- [ ] Name written with hash in first 8 bytes
- [ ] Short names (≤8 bytes) have hash == UTF-8 name
- [ ] `nodes_read_name_hash` returns correct hash
- [ ] `dirchain_find_child` uses hash fast-reject
- [ ] All existing tree tests pass
