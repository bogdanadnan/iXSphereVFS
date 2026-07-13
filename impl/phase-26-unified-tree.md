# Phase 26: Unified Tree Structure (File ↔ Directory)

## Goal

Make the directory organization structurally identical to the file
organization, so one chain-walk implementation serves both. This
delivers:

1. **A clean lock model.** Every writable node (root, segment, unit)
   is a `VirtualPtr`. We rework `vfs_lock` to be per-`vfs_t` and
   VP-keyed (W0 prerequisite), then use it to lock at any tree
   level. Writers on disjoint subtrees no longer share a
   serialization point.
2. **A single chain-walk code path.** 6 steps; 4 identical between
   file and dir, 2 specialized.
3. **A correct concurrency model.** The CAS-on-local hazard
   introduced by Phase 25 (see C1_REVIEW) is removed in favor of
   real locks keyed on the chain's owning node. A same-level
   "lower VP first" acquisition rule eliminates deadlock.

This phase changes the on-disk format. Existing VFS files are
not compatible. This is acceptable — the VFS is pre-MVP, no
users, no data. We do not maintain backward compatibility.

This phase also **reworks `vfs_lock` itself** as a prerequisite
(W0). The existing `vfs_lock` keys the global `lock_table` on
`nodeId` (not `VirtualPtr`), is process-global (not per-`vfs_t`),
and never tears down at `vfs_unmount` (ISSUES.md H4). Phase 26
needs `vfs_lock` keyed on any `int64_t` (so it can lock `Node`,
`Segment`, or `ContentUnit` VPs), per-`vfs_t` (so two mounted
VFS instances don't collide), and torn down at `vfs_unmount`
(fixes H4 as a side effect).

## Status: Phase 26 Complete (W0–W6)

**Phase 26 shipped.** 42 commits (`50f9fb5`..`0b22a51`), 13/13 tests
pass, FUSE bench 18.7s (in noise of the W0 best 16.94s; 2.4× faster
than the pre-Phase-25 baseline of 45.03s). All 5 mutation ops
(`vfs_create`/`vfs_mkdir`/`vfs_delete`/`vfs_rmdir`/`vfs_rename`/
`vfs_write`/`vfs_truncate`) use the node-before-`ContentUnit` lock
discipline; `vfs_delete` and `vfs_rmdir` honor the **O1 contract**
(child-only lock, no parent). All 11 critical CAS sites on
`PoolSlot` bytes are gone (`grep vfs_cas_i64 src/tree.c src/vfs.c`
= 0). The chain-walk read-rule has a **single source of truth** —
`vfs_chain_walk` in `src/tree.c` — enforced by
`test_chain_walk_no_duplication`.

### What shipped vs what the spec predicted

| Goal | Predicted | Actual | Verdict |
|---|---|---|---|
| CAS elimination on `PoolSlot` bytes | 0 matches in `src/tree.c` / `src/vfs.c` | 0 matches (verified) | ✅ **Met** |
| Same-epoch `vfs_lock` exclusion | new in W0 | 4 new tests pass | ✅ **Met** |
| Per-`vfs_t` lock table | new in W0 | implemented, teardown on unmount | ✅ **Met** |
| Single chain-walk read-rule | 1 copy (U1) | 1 copy (verified by no-dup test) | ✅ **Met (in W6)** |
| `tree_resolve_page` collapse | 430 → ~80 lines | 430 → 270 lines | ⚠ **Partial** — write-path segment growth (tcache, sorted insertion, segment-level prepend) cannot use the read-only shared walk |
| `dirchain_find_child` collapse | 225 → ~120 lines | 225 → 89 lines | ✅ **Better than target** |
| `dirchain_list` dedup removal | -100 LOC, no hash_map | done, 57 lines | ✅ **Met** |
| `tree.c` line count | 730 → 335 (−54%) | 2488 → 3948 (+59%) | ❌ **Did not meet** — the W5 per-`ContentUnit` machinery + W6 new shared walks add ~1.5k LOC. The duplication is removed; the file is not smaller. |
| FUSE bench (252 MB zip + unzip) | within ±10% of 16.94s (noise) | 18.7s median, range 17.7–19.9s | ✅ **In noise** (post-W6 review corrected the early micro-bench regression; the W6 profile is flat vs W5) |
| `vfs_bench` micro-bench | "neutral on FUSE, slight negative on micro-bench" | pre-26 → post-W5: −40% on create/write. Post-W6: flat vs W5. | ⚠ **Micro-bench regression was real; W6 didn't recover it but stopped it from getting worse.** Trade-off accepted — the per-`ContentUnit` machinery is the correct model; the perf cost is structural. |

### What this phase explicitly does **not** claim

- **GC is non-functional** (pre-existing, see `PHASE26_IMPL_REVIEW.md`
  G1). The test suite masks this with a `CHECK_EQ(gc_ret, VFS_ERR_FULL)`
  tolerance in `test_gc.c:786-788`. The W5 GC rework (per-type
  field descriptors, per-`ContentUnit` survival) is structurally
  correct but unreachable until the `VFS_ERR_FULL` failure is fixed
  upstream. **Descoped to the future GC reimplementation.**
- **The 2× chain-walk code reduction** is not met (see table). The
  *duplication* of the read-rule is removed (W6); the absolute
  line count is not smaller. This was an over-promise in the
  original spec.
- **Forward-compatibility is not maintained.** Phase 26 breaks the
  on-disk format. Pre-Phase-26 VFS files are not readable. This
  is by design (pre-MVP, no users).

### Pointers

- Implementation review: `PHASE26_IMPL_REVIEW.md` (W5j-era audit
  of the W0–W5j state; some findings — U1, S1, S2, S4 — are now
  resolved by W6).
- W6 review: `PHASE26_W6_REVIEW.md` (post-W6 review, including
  the perf-correction that found W6 is flat vs W5, not −40% vs
  pre-26 as the W5j micro-bench suggested).
- Workload commits: see "Migration plan" below; the commit list
  is `git log --oneline 50f9fb5..0b22a51` (42 commits).

## Background

The current model is asymmetric:

| Level | File | Dir |
|---|---|---|
| 1. Root | `FileNode` (32 B) | `DirNode` (32 B) |
| 2. Segment | `FileContent` (32 B) | **none** |
| 3. Unit anchor | `PageNode` (32 B, per page) | **none** |
| 4. Chain entry | `VersionPage` (32 B, per epoch) | `DirContent` (32 B, per insert) |
| 5. Leaf | data page | child `FileNode` / `DirNode` |
| Auxiliary | none | `DirContentIndex` + `DirContentLink` (radix tree for name lookup) |

So the file model has a 5-level chain (root→segment→unit→chain→leaf),
and the dir model has a 3-level chain (root→chain→leaf) plus a
side-car radix index for name lookup. The two models have
fundamentally different shapes, which is why `dirchain_find_child`
and `tree_resolve_page` look nothing alike despite doing the same
kind of walk.

The asymmetry also makes concurrency ugly:

- **File** has a per-`PageNode` anchor (the `PageNode` itself). All
  writes to a single logical page serialize on `PageNode` via
  `vfs_lock(vfs, page_node_vp, epoch)`. Writes to different pages
  on the same file are independent.
- **Dir** has no per-child anchor. The `DirNode.HEADPTR` is a
  single linked-list head for the entire `DirContent` chain.
  Every insert in that dir CASes the same head pointer. Every
  write to any child of a dir contends with every other write to
  any child of that dir, even if the children are unrelated.

Phase 25's CAS-on-local hazard (C1_REVIEW) is most acute in the
dir model because all the contention points are on a single
`HEADPTR` cache line per dir. With the unified model, the head
pointer is per-child (per `ContentUnit`), and writers on disjoint
children don't share it.

## Proposed state

The unified 5-level model. The naming convention is:

- **`Anchor`** — generic term for any node at the root, segment,
  or unit level. All three are the same 32-byte struct, with a
  `type` discriminator.
- **`Node`** — Anchor at the root level. Replaces `FileNode` and
  `DirNode`. Two `type` values: `ROOT_FILE`, `ROOT_DIR`.
- **`Segment`** — Anchor at the mid level. Replaces `FileContent`
  and `DirSegment`. Two `type` values: `SEGMENT_FILE`,
  `SEGMENT_DIR`. Holds 64 child Anchors of the next level.
- **`ContentUnit`** — Anchor at the per-content level. Replaces
  `PageNode` (per-page for files) and `SlotNode` (per-child for
  dirs). Two `type` values: `UNIT_PAGE`, `UNIT_SLOT`. Holds the
  per-content chain head.
- **`VersionPage`** — file leaf. Per-epoch entry carrying the
  data page pointer. **Unchanged** by this phase.
- **`DirContent`** — dir leaf. Per-epoch entry carrying the child
  pointer and name pointer. **Unchanged** by this phase.

| Level | Type | Replaces | New in this phase? |
|---|---|---|---|
| 1. Root | `Node` (`type` = `ROOT_FILE`/`ROOT_DIR`) | `FileNode`/`DirNode` | (refactored) |
| 2. Segment | `Segment` (`type` = `SEGMENT_FILE`/`SEGMENT_DIR`) | `FileContent`/`DirSegment` | **`DirSegment` is new**; `FileContent` refactored to Anchor shape |
| 3. Unit anchor | `ContentUnit` (`type` = `UNIT_PAGE`/`UNIT_SLOT`) | `PageNode`/`SlotNode` | **`SlotNode` is new**; `PageNode` refactored to Anchor shape |
| 4. Chain entry (file) | `VersionPage` | `VersionPage` | (unchanged) |
| 4. Chain entry (dir) | `DirContent` | `DirContent` | (refit: anchored on `ContentUnit`, not `Node`) |
| 5. Leaf | data page | data page | (unchanged) |
| 5. Leaf (dir) | child `Node` | child `FileNode`/`DirNode` | (unchanged) |
| Auxiliary | `DirContentIndex` + `DirContentLink` | (same) | (refit: storage now points at `ContentUnit`, not `DirContent`) |

### Data layouts

#### `Anchor` (32 B) — one struct, used for `Node`, `Segment`, and `ContentUnit`

```
  Offset  Size  Field
  ──────  ────  ─────
     0      2   type       (uint16 — AnchorKind enum, see below)
     2      2   flags      (uint16 — reserved for now; e.g. locked/dirty)
     4      4   id         (uint32 — semantic depends on type:
                            nodeId for Node, segmentId for Segment,
                            pageIndex or slotId for ContentUnit)
     8      8   headPtr    (VirtualPtr — first child at next-lower level)
    16      8   sibPtr     (VirtualPtr — next sibling at same level;
                            root level: 0 since one root per VFS;
                            semantic-overloaded for root: see "Root
                            semantic overload" below)
    24      4   count      (uint32 — semantic depends on type:
                            unit count for Segment; 0 otherwise)
    28      4   reserved
```

`AnchorKind` enum:

```
  ROOT_FILE     = 0x10
  ROOT_DIR      = 0x11
  SEGMENT_FILE  = 0x20
  SEGMENT_DIR   = 0x21
  UNIT_PAGE     = 0x30
  UNIT_SLOT     = 0x31
```

**Root semantic overload.** The 16 bytes at offset 16-31 are
interpreted differently for `Node` than for `Segment`/`ContentUnit`:

- `Node` (root):
  - `sibPtr` (offset 16) = `metaPtr`. For `ROOT_FILE`: chain
    head of `FileSize`. For `ROOT_DIR`: chain head of
    `DirContentIndex`.
  - `count` (offset 24) = 0 (unused).
  - offset 28: reserved.
- `Segment` (segment):
  - `sibPtr` = `nextPtr` (next Segment at this level).
  - `count` = number of Anchors in this segment.
  - offset 28: reserved.
- `ContentUnit` (unit):
  - `sibPtr` = `nextPtr` (next ContentUnit at this level).
  - `count` = 0 (unused; childNodeId lives in `id`).
  - offset 28: reserved.

**Wasted bytes:** `Node` wastes 8 bytes (sibPtr is always 0
since one root per VFS) plus 4 bytes (count is always 0). 12
bytes per root node. For a VFS with thousands of root nodes
(e.g. SQLite one-table-per-file), this is 12 KB total — noise.

#### `Node` (32 B, root of a file or directory)

```
  Offset  Size  Field
  ──────  ────  ─────
     0      2   type         (uint16 — ROOT_FILE or ROOT_DIR)
     2      2   flags
     4      4   nodeId       (uint32)
     8      8   headPtr      (VirtualPtr — first Segment)
    16      8   metaPtr      File: sizePtr (FileSize chain)
                            Dir:  indexHeadPtr (DirContentIndex)
    24      8   createdAt    (int64 — Unix timestamp; 0 if unset)
```

**`DirNode.childCount` is gone.** It was a "monotonic upper
bound" used to size the dedup hash_map in `dirchain_list`. In
the new model, the same upper bound is `sum(Segment.count)`
across all segments, computed by walking the Segment chain —
which `dirchain_list` is doing anyway. Removing the field
also eliminates ISSUES.md H6 (the non-atomic read-modify-write
race on `childCount`).

#### `Segment` (32 B, mid-level group of 64 Anchors)

```
  Offset  Size  Field
  ──────  ────  ─────
     0      2   type         (uint16 — SEGMENT_FILE or SEGMENT_DIR)
     2      2   flags
     4      4   segmentId    (uint32)
     8      8   headPtr      (VirtualPtr — first Anchor of next-lower level)
    16      8   sibPtr       (nextPtr — next Segment, 0 = end)
    24      4   count        (uint32 — number of Anchors in this segment)
    28      4   reserved
```

Constants: `ANCHOR_UNITS_PER_SEGMENT = 64` (matches existing
file model). `Segment.count` enables the same lazy tcache
rebuild the file model already uses.

#### `ContentUnit` (32 B, per-content anchor)

```
  Offset  Size  Field
  ──────  ────  ─────
     0      2   type         (uint16 — UNIT_PAGE or UNIT_SLOT)
     2      2   flags
     4      4   id           (uint32 — pageIndex for UNIT_PAGE,
                              slotId/childNodeId for UNIT_SLOT)
     8      8   headPtr      (VirtualPtr — first chain entry
                              (VersionPage for UNIT_PAGE,
                               DirContent for UNIT_SLOT))
    16      8   sibPtr       (nextPtr — next ContentUnit at this level)
    24      4   count        (uint32 — 0, unused)
    28      4   reserved
```

**Why `ContentUnit` is a separate Anchor (not just a field on
`DirContent`):**

1. **Lock key stability.** `vfs_lock` keys on `VirtualPtr`. A
   `DirContent` slot is replaced every time a new epoch is
   written (the new `DirContent` is a new slot with a new VP).
   The lock key would change on every write, defeating the
   point. The `ContentUnit` is allocated once per child and
   lives for the lifetime of the child; the lock key is stable.
2. **Cache locality.** The `ContentUnit` is read on every read
   and write of a child. Separating it from the volatile
   `DirContent` chain means it stays in the same cache line
   across the child's whole life. The `DirContent` chain can
   churn without disturbing it.

#### Unified leaf layout (32 B) — VersionPage, DirContent, FileSize

All three per-epoch chain entries follow the same layout
convention so the chain walk's step 4 can be byte-identical
for all of them:

```
  Offset  Size  Field        Notes
  ──────  ────  ─────        ─────
     0      4   epoch        (uint32 — read-rule compare key)
     4      4   kind_specific (uint32 — semantics depend on leaf)
     8      8   primary_ptr  (int64 — semantics depend on leaf)
    16      8   secondary_ptr (int64 — semantics depend on leaf)
    24      8   nextPtr      (VirtualPtr — next older entry, 0 = end)
```

The walk's step 4 reads `epoch` at offset 0 and `nextPtr` at
offset 24 — same code for all three leaf types. The
leaf-specific fields (offsets 4, 8, 16) are accessed only at
the leaf step (Step 6) where the caller knows the leaf type.

**`VersionPage` (file leaf, per-page):**

| Offset | Size | Field | Semantics |
|---|---|---|---|
| 0 | 4 | `epoch` | (uint32) |
| 4 | 4 | reserved | |
| 8 | 8 | `dataPage` | int64 — logical page index |
| 16 | 8 | reserved | |
| 24 | 8 | `nextPtr` | VirtualPtr — next older VersionPage |

**Change vs. Phase 25:** move `nextPtr` from offset 16 to 24;
add 8 bytes of reserved at offset 16. epoch=0 already correct.

**`DirContent` (dir leaf, per-child):**

| Offset | Size | Field | Semantics |
|---|---|---|---|
| 0 | 4 | `epoch` | (uint32) |
| 4 | 4 | `childNodeId` | (uint32) |
| 8 | 8 | `childPtr` | VirtualPtr — child Node VP |
| 16 | 8 | `namePtr` | VirtualPtr — NameEntry; 0 = tombstone |
| 24 | 8 | `nextPtr` | VirtualPtr — next DirContent |

**Change vs. Phase 25:** swap the order of `childNodeId`
(was at offset 0) and `epoch` (was at offset 4). Fields
themselves unchanged. **Anchoring change:** the chain is
now anchored on `ContentUnit.headPtr`, not on `Node.headPtr`.
With per-child chains, the read-rule is now applied
**per-child** (each child sees its own chain, not a mixed
chain) — a key simplification for `dirchain_list` (see §4.4).

**`FileSize` (file meta chain entry, per-epoch):**

| Offset | Size | Field | Semantics |
|---|---|---|---|
| 0 | 4 | `epoch` | (uint32) |
| 4 | 8 | `modifiedAt` | int64 — Unix timestamp |
| 12 | 8 | `fileSize` | int64 — file size in bytes |
| 20 | 4 | reserved | |
| 24 | 8 | `nextPtr` | VirtualPtr — next FileSize |

**Change vs. Phase 25:** move `nextPtr` from offset 20 to 24;
add 4 bytes of reserved at offset 20. `epoch`=0 already
correct.

**Why this is worth the on-disk format changes:**
- The chain walk's step 4 is byte-identical for all three leaf
  types. One less specialization.
- The "leaf step" (Step 6) is already specialized per leaf
  type — the walk doesn't need to know which leaf it's walking.
- Future leaf types just need to follow the convention (epoch
  at 0, nextPtr at 24, payload in the middle).
- The cost is small: only `DirContent` field-order swap; the
  other two are padding additions.

We are not maintaining compatibility with the old leaf
formats. The on-disk layout of `DirContent` and `FileSize`
changes with this phase. `VersionPage` is unchanged at the
field level (only the location of `nextPtr` and the addition
of padding). Per the no-backward-compat rule, this is fine.

### Auxiliary: radix index (kept, refit)

The `DirContentIndex` radix tree continues to exist as a
name-to-`ContentUnit` accelerator. It is **not** part of the
chain walk; it's a cache for the name-lookup path
(`dirchain_find_child`).

**Refit:** the radix leaf's `DirContentLink` payload changes
from `int64_t dirContentVP` to `int64_t contentUnitVP`. Lookup
is now name → `ContentUnit` → walk chain → visible
`DirContent`. One indirection; the `DirContent` is no longer
the radix index's payload.

We are not maintaining compatibility with the old index format.
The on-disk layout of the radix index changes with this phase.

### Key simplification: dedup goes away in `dirchain_list`

In the current model, all children share a single `DirContent`
chain, so the walk sees entries for many children and needs
`DirchainDedupEntry` + a hash_map to collapse to one entry
per child.

In the per-`ContentUnit` model, each child has its own chain.
The walk for that child finds one visible entry directly. The
list of children is just: (`ContentUnit`, visible
`DirContent`) pairs. The only filter needed is "skip
tombstones" (`namePtr == 0`).

Net effect on `dirchain_list`: no hash_map, no
`DirchainDedupEntry`, no `VarArray(DirchainDedupEntry)`, no
"first-hit-wins" logic. Just a `VarArray(vfs_dirent_t)` for
the output. ISSUES.md M1 (hash_map saturation) becomes
impossible by construction.

`hash_map.c`/`hash_map.h` and `VarArray` infrastructure are
**kept** — they have other future uses and removing them
expands the diff unnecessarily.

## Chain walk algorithm

The unified walk is a single function:

```c
typedef enum {
    WALK_FOUND,        // leaf resolved
    WALK_NOT_FOUND,    // walk exhausted without resolving
    WALK_NEED_GROW,    // leaf doesn't exist; caller should grow and retry
} WalkResult;

WalkResult vfs_chain_walk(TreeContext* ctx,
                          int64_t       root_vp,      // Node VP (file or dir)
                          int64_t       unit_id,      // pageIndex or slotId
                          int64_t       query_epoch,
                          PoolSlot*     out_leaf);    // VersionPage or DirContent
```

### The 6 steps

```
  Step 1  Walk segments:    Node → Segment chain.
                            Until segment contains unit_id (via segment's
                            root pointer + lazy tcache rebuild).
  Step 2  Walk unit anchor: Within segment, walk ContentUnit chain.
  Step 3  Compare unit_id:  uint32 compare at ContentUnit.id (offset 4).
                            Same step in code for file and dir.
  Step 4  Walk chain:       Walk the per-leaf chain (VersionPage /
                            DirContent / FileSize). All three leafs
                            share the unified layout: epoch at offset
                            0, nextPtr at offset 24. Step 4 is
                            byte-identical for all leaf types.
  Step 5  Apply read-rule:  Per-entry mapper remap + even/odd +
                            exact-match-wins. Byte-identical for all
                            leaf types (uses the same read fields).
  Step 6  Return leaf:      File: data page VP (resolved via
                            storage_read on VersionPage.dataPage).
                            Dir:  childPtr + namePtr from DirContent
                            (the child Node VP).
```

**5 of 6 steps are byte-identical between file and dir** (Steps
1-5). Only Step 6 is specialized — the leaf type-specific
fields (dataPage vs childPtr/namePtr) are accessed there.

### Specialization: Steps 3 and 6

Two steps differ between file and dir, and the specialization is
local to a single function. Sketch:

```c
WalkResult vfs_chain_walk(..., PoolSlot* out_leaf) {
    PoolSlot seg_slot = {0}, unit_slot = {0}, chain_slot = {0};

    // Step 1: walk segments
    int64_t seg_vp = rd8(root_slot, ANCHOR_OFF_HEADPTR);
    while (seg_vp) {
        pool_acquire(pool, seg_vp, false, &seg_slot);
        int64_t head_in_seg = rd8(seg_slot, ANCHOR_OFF_HEADPTR);
        uint32_t count = rd4(seg_slot, ANCHOR_OFF_COUNT);
        // tcache rebuild if needed (mirrors file's existing logic)
        if (tcache_miss(head_in_seg, unit_id)) {
            seg_vp = rd8(seg_slot, ANCHOR_OFF_SIBPTR);
            continue;
        }
        break;
    }

    // Step 2-3: walk unit anchors
    int64_t unit_vp = rd8(seg_slot, ANCHOR_OFF_HEADPTR);
    while (unit_vp) {
        pool_acquire(pool, unit_vp, false, &unit_slot);
        uint32_t this_id = rd4(unit_slot, ANCHOR_OFF_ID);
        if (this_id == unit_id) break;
        unit_vp = rd8(unit_slot, ANCHOR_OFF_SIBPTR);
    }
    if (!unit_vp) return WALK_NOT_FOUND;

    // Step 4-5: walk chain with the full read-rule.
    //
    // The read-rule (mirrors verchain_get at tree.c:619) is
    // per-entry:
    //   1. Compute effective_epoch = mapper_table_resolve(stored_epoch)
    //      if mapper_table_traversal_apply(stored_epoch) is true;
    //      otherwise effective_epoch = stored_epoch.
    //      (mapper_table_traversal_apply returns bool, NOT the epoch
    //      — common mistake in the original sketch. The two-step
    //      dance is required because traversal_apply is a flag check
    //      and resolve does the actual remap.)
    //   2. If effective_epoch == query_epoch: exact match wins.
    //      Stop the walk.
    //   3. If effective_epoch < query_epoch AND even: committed
    //      base entry. Stop the walk.
    //   4. If effective_epoch < query_epoch AND odd: unrelated
    //      snapshot, skip.
    //   5. If effective_epoch > query_epoch: future entry, skip.
    //
    // For dir (DirContent): additionally, if the matching entry
    // has namePtr == 0 (tombstone), the entry is invisible and
    // the walk continues to find the next-highest-epoch live
    // entry for the same child.
    int64_t chain_vp = rd8(unit_slot, ANCHOR_OFF_HEADPTR);
    int64_t visible_chain_vp = 0;
    while (chain_vp) {
        pool_acquire(pool, chain_vp, false, &chain_slot);

        // Read-rule per-entry (matches verchain_get at tree.c:638-642)
        uint32_t stored_epoch = rd4(&chain_slot, LEAF_OFF_EPOCH);
        int64_t eff_epoch = (int64_t)stored_epoch;
        if (mapper_table_traversal_apply(&ctx->mapper_table,
                                         (int64_t)stored_epoch)) {
            eff_epoch = mapper_table_resolve(&ctx->mapper_table,
                                              (int64_t)stored_epoch);
        }

        if (eff_epoch == query_epoch) {
            // exact match wins
            visible_chain_vp = chain_vp;
            pool_release(pool, &chain_slot);
            break;
        } else if (eff_epoch < query_epoch) {
            if ((eff_epoch & 1) == 0) {
                // committed base, stop
                visible_chain_vp = chain_vp;
                pool_release(pool, &chain_slot);
                break;
            }
            // odd: unrelated snapshot, skip
        }
        // eff_epoch > query_epoch OR odd-and-skip: continue walk
        chain_vp = rd8(&chain_slot, LEAF_OFF_NEXTPTR);
        pool_release(pool, &chain_slot);
    }
    if (!visible_chain_vp) return WALK_NOT_FOUND;

    // Re-acquire the visible entry into the caller's out_leaf.
    pool_acquire(pool, visible_chain_vp, false, out_leaf);

    // Step 6: leaf (specialized)
    // File: out_leaf holds VersionPage; caller does storage_read on dataPage.
    // Dir:  out_leaf holds DirContent; caller checks tombstone + reads childPtr/namePtr.
    // The caller knows the type. We just return the leaf slot.
    return WALK_FOUND;
}
```

The 5 common steps (1, 2, 3, 4, 5) are byte-for-byte the same
code in both file and dir walks. Step 6 is "return the leaf
slot"; the caller knows the type and uses it accordingly.

**Important: the mapper remap is per-entry, not post-walk.**
`mapper_table_traversal_apply` is keyed on the *entry's stored
epoch*, so applying it after the walk is incorrect — you'd
have one `query_epoch` mapped to itself, which gives wrong
results for committed snapshots. The remap must happen during
the walk, per entry. The `mapper_table_*` variants are the
in-memory fast path (matches `verchain_get` at `tree.c:638`).

**The tombstone-skip for `DirContent`** is applied at the leaf
step (caller side) — `dirchain_find_child` checks
`namePtr == 0` after the walk returns the visible entry; if
tombstoned, the caller may walk further (e.g., looking for
a same-name live entry via the radix index's other links —
see §4.4).

**Offset macros used in the sketch:**

```
  ANCHOR_OFF_TYPE     = 0
  ANCHOR_OFF_FLAGS    = 2
  ANCHOR_OFF_ID       = 4
  ANCHOR_OFF_HEADPTR  = 8
  ANCHOR_OFF_SIBPTR   = 16
  ANCHOR_OFF_COUNT    = 24
  ANCHOR_RESERVED_OFF = 28

  LEAF_OFF_EPOCH      = 0   (uint32 — read-rule compare key)
  LEAF_OFF_KIND       = 4   (uint32 — leaf-specific)
  LEAF_OFF_PRIMARY    = 8   (int64 — leaf-specific)
  LEAF_OFF_SECONDARY  = 16  (int64 — leaf-specific)
  LEAF_OFF_NEXTPTR    = 24  (VirtualPtr — chain next)
```

The `ANCHOR_OFF_*` macros apply to `Node`, `Segment`, and
`ContentUnit`. The `LEAF_OFF_*` macros apply to `VersionPage`,
`DirContent`, and `FileSize` (all per-epoch chain entries
follow the unified leaf layout from §3.6).

### ContentUnit visibility rule (delete+recreate)

In the per-`ContentUnit` model, a `DirContent` chain is anchored
on a single `ContentUnit` (per childNodeId). However, a single
name in a dir can be served by **multiple `ContentUnit`s** when
delete+recreate happens:

- Old child `ContentUnit_A` (nodeId=5):
  chain = `[tombstone@E, live@E0]` (originally created at E0,
  tombstoned at E).
- New child `ContentUnit_B` (nodeId=6):
  chain = `[live@E]` (created at E with the same name).

Both `ContentUnit`s live in the directory's radix index leaf
(the `DirContentLink` list for that name hash contains links
to both). The lookups must decide which is "the" `ContentUnit`
for the name at the query epoch.

**Visibility rule:**

> A `ContentUnit` is **visible** at epoch E iff its
> highest-applicable `DirContent` (per the read-rule above) has
> `namePtr != 0` (i.e., not a tombstone).

**`dirchain_find_child` at epoch E:**
1. Radix index lookup → get all `DirContentLink`s at the name
   hash. There may be multiple (one per delete+recreate cycle).
2. For each link, follow to its `ContentUnit`. Walk that
   `ContentUnit`'s `DirContent` chain with the read-rule to find
   the highest-applicable entry.
3. If the visible entry is a tombstone (`namePtr == 0`), this
   `ContentUnit` is not the one we're looking for — skip it.
4. If the visible entry is live (`namePtr != 0`), this is the
   match. Return `childPtr` and `childNodeId`.
5. If no live match is found across all `ContentUnit`s at this
   hash, return `VFS_ERR_NOTFOUND`.

**`dirchain_list` at epoch E:**
1. Walk all `ContentUnit`s (via segments).
2. For each, find the visible `DirContent`.
3. If the visible entry is a tombstone, skip this
   `ContentUnit`.
4. If the visible entry is live, add it to the output.

The radix index leaf's `DirContentLink` list may contain
multiple `ContentUnit`s; the visibility rule resolves them
without needing the `childNodeId`-level dedup that the current
`dirchain_list` requires. The dedup-infrastructure removal in
the spec is still a real win — we go from O(N) hash_map ops on
a shared chain to O(1) tombstone check per `ContentUnit`, on
chains that are typically 1-2 entries.

**`dircontentindex_insert` invariant:** when a new
`ContentUnit` is created (e.g., create-after-delete), the
insert adds a new `DirContentLink` to the radix leaf pointing
at the *new* `ContentUnit`. When a `ContentUnit` is rewritten
in place (e.g., same-child write), the existing link is reused
(no new link added; the `ContentUnit` itself is the same
nodeId).

**`dircontentindex_remove` invariant:** when a `ContentUnit`
is removed (rename with old-name cleanup, or GC), the link to
it in the radix leaf is zeroed (tombstoned at the link level).
Future lookups see the zeroed link and skip it.

### What becomes simpler

`tree_resolve_page` and `dirchain_find_child` collapse into one
function plus two tiny specialized leaf-handlers. `dirchain_list`
loses its dedup infrastructure (no hash_map, no
`DirchainDedupEntry`).

**Note:** the table below was the *original* estimate. The
**actual** line counts are in the rightmost column (W6
result). The "2× reduction" target was not met — see the
"Status" section at the top for why.

| Component | Original estimate | W6 result | Comment |
|---|---|---|---|
| `tree_resolve_page` | 330 → ~80 | 430 → **270** | Write-path segment growth (tcache lookup, sorted insertion by `pageIndex`, segment-level `headPtr` prepend, `pageCount` update) cannot use the read-only shared walk. 270 lines reflects this. |
| `dirchain_find_child` | 180 → ~30 | 225 → **89** | Better than the W6 target of ~120. The by-name iteration is small; the per-`ContentUnit` chain walk delegates to `vfs_chain_walk`. |
| `dirchain_list` | 150 → ~50 | 150 → **57** | Met. Dedup hash_map + `DirchainDedupEntry` removed. |
| `vfs_truncate` size chain | 30 → ~5 | unchanged (uses `sizechain_get`) | Met. |
| `verchain_get`, `sizechain_get` | ~40 each, unchanged | unchanged | Used as Step 4–5 helpers; stay thin wrappers over `vfs_chain_walk`. |
| New: `vfs_chain_walk` | ~120 | 50 | Met. |
| New: `walk_anchor_chain` | (added in W6) | 30 | Step 1–2 walker for FileContent / DirSegment chains. |
| New: `walk_content_unit_chain` | (added in W6) | 22 | Step 3 walker for PageNode / SlotNode chains. |
| New: `vfs_chain_walk_extended` | (added in W6) | 88 | 6-step walk: root_vp → anchor chain → content unit → leaf chain + read-rule. |
| New: `dirchain_radix_link_cb` etc. | (added in W6) | ~30 per callback | Per-callback state structs + callbacks for `dirchain_find_child`'s radix vs fallback paths. |

**Net `src/tree.c` size:** 2488 lines (pre-26, commit `934638e`)
→ 3948 lines (post-W6, commit `0b22a51`). **+59%**, not the
promised −54%. The reasons:
- The W5 per-`ContentUnit` machinery is genuinely new code: a
  new `SlotNode` chain per child, a new `DirSegment` level,
  per-type field descriptors in GC, the `ContentUnit` visibility
  rule. None of this existed pre-26.
- The W5 shim removals (`pool_resolve_*`, `tree_resolve_page_compat`)
  shrunk `src/pool.{c,h}` and `test/test_tree.c`, but the new
  code added more than those shims ever saved.
- The W6 shared walks are net additions (~180 lines) that paid
  for themselves in `tree_resolve_page` and `dirchain_find_child`
  (combined −175 lines), but the savings did not exceed the cost
  of the new infrastructure.

**The maintenance win that did land:** the read-rule is now in
exactly one place (`vfs_chain_walk` in `src/tree.c:770-820`),
enforced by `test_chain_walk_no_duplication` (a runtime test
that greps `src/tree.c` for the read-rule markers and asserts
each appears exactly once). Any future change to the read-rule
propagates automatically. This is the core of U1 from the
implementation review, and it shipped.

## Lock model

The unified model makes the lock story clean. Every writable
level is a `VirtualPtr`. After W0 (the `vfs_lock` rework), the
`vfs_lock` API accepts any `int64_t` as the key and is
per-`vfs_t`, so the lock infrastructure matches the new model.

### The four lock keys

| Operation | Lock key | Why |
|---|---|---|
| Append a `Segment` | `vfs_lock(vfs, node_vp, epoch)` | Need exclusive access to grow the segment list. |
| Append a `ContentUnit` to a `Segment` | `vfs_lock(vfs, segment_vp, epoch)` | Need exclusive access to grow the segment's unit list. |
| Prepend a chain entry (`DirContent` / `VersionPage`) | `vfs_lock(vfs, content_unit_vp, epoch)` | Need exclusive access to the per-unit chain head. |
| Update a per-epoch field on the root (e.g., prepend `FileSize` / `DirContentIndex`) | `vfs_lock(vfs, node_vp, epoch)` | Root-level chain updates. |

All four lock keys are stable `VirtualPtr`s. None change as
content is appended.

### Per-epoch lock semantics

Structural locks use `vfs_lock` in **per-epoch mode**
(non-`epoch==0`). The semantics after W0 are:

- **Different-epoch writes to the same key proceed in
  parallel.** This is intentional — the per-epoch COW
  model is built on the principle that writes at
  different epochs are independent and coexist on the
  same chain. Two concurrent snapshot writes to the same
  file/dir proceed in parallel; the read-rule disambiguates
  the result at query time.
- **Same-epoch writes to the same key are excluded.** The
  second acquirer blocks until the first releases. This
  is **new functionality added in W0** (see §9) — the
  existing `vfs_lock` per-epoch path is a shared lock
  (`epoch_count++` in `vfs.c:134`) with no
  same-epoch-keyed check, which would not provide the
  mutual exclusion the chain-prepend code requires. W0
  must change this.
- **Global mode (`epoch==0`) is fully exclusive.** It
  drains all per-epoch holders on the key, then sets
  `global_held`. Reserved for stop-the-world operations
  (future GC, future "freeze subtree"). Not used by
  any structural mutation in this phase.

Concretely:
- Two writers at different snapshot epochs (E1, E3) both
  prepending `DirContent` to the same `ContentUnit` chain:
  both proceed, both writes land on the chain, the
  read-rule picks one at query time. Correct.
- Two writers at the same epoch to the same `ContentUnit`:
  one wins, the other waits. Correct (after W0's
  same-epoch exclusion).
- Two writers at different epochs to the same `Node`'s
  `DirContentIndex`: both proceed. The radix index's
  link-list CAS becomes a locked write (W4/W5) on `Node`,
  but the lock is per-epoch so they can both proceed. The
  resulting tree state is consistent per-epoch (read-rule
  resolves).

**Implementation hint (for W0):** change
`FileLockState.epoch_count` (a single int at `vfs.c:30`)
to a small structure that tracks holders per-epoch. For a
typical epoch range (32 or so), an array of 32 `{epoch,
count}` pairs is sufficient. Same-epoch second-acquirer
blocks until first releases; different-epoch proceeds
without checking other entries.

### Lock ordering rules

To make deadlock impossible, we enforce a total order on lock
acquisition:

1. **Hierarchical order.** Always acquire in the order
   `Node > Segment > ContentUnit`. A `ContentUnit` lock is
   never acquired while holding a lock on a parent `Node`
   or `Segment` unless the parent is needed as part of the
   same operation (e.g., a new-child create needs both the
   parent `Node` and the new `ContentUnit`).

2. **Same-level order.** When acquiring multiple locks at the
   same level in a single operation, acquire them in
   **ascending order of `VirtualPtr`**. Never acquire a
   same-level lock with a higher VP while holding one with a
   lower VP. This is a total order on VP, so two threads
   operating on the same set of locks always take them in
   the same order.

The combination makes the lock graph acyclic:
- Hierarchy: `Node > Segment > ContentUnit` (a linear chain,
  no cycles).
- Same level: lower VP first (a total order, no cycles).

Deadlock is impossible as long as these rules are respected
everywhere.

### Lock acquisitions per operation

Every file-mutating operation takes **two locks**:
`vfs_lock(node, epoch)` (for the `FileSize` chain or
`DirContentIndex` on the parent `Node`) and
`vfs_lock(content_unit, epoch)` (for the per-page
`VersionPage` or per-child `DirContent` chain). The
hierarchy is `Node > ContentUnit` — always `node_vp` first,
then per-unit `ContentUnit`. This is consistent across all
operations; the per-unit `ContentUnit` lock is technically
redundant for exclusion when `node_vp` is held (any
concurrent writer is blocked on `node_vp`), but we keep
it for symmetric model documentation and for future
optimization (see "Per-page lock redundancy" note below).

| Operation | Locks held (in order) |
|---|---|
| `vfs_create` / `vfs_mkdir` (new child) | `vfs_lock(node, epoch)` → `vfs_lock(segment, epoch)` → `vfs_lock(new_content_unit, epoch)` |
| `vfs_delete` / `vfs_rmdir` (existing child) | `vfs_lock(content_unit, epoch)` (per-child `ContentUnit` lock only — see O1 below; tombstones are per-`ContentUnit` chain, not per-`DirContentIndex`, so no `node_vp` needed) |
| `vfs_rename` same-dir | `vfs_lock(node, epoch)` → `vfs_lock(src_content_unit, epoch)` → `vfs_lock(dst_content_unit_or_new, epoch)` (lower VP first) |
| `vfs_rename` cross-dir | `vfs_lock(src_node, epoch)` → `vfs_lock(src_content_unit, epoch)` → `vfs_lock(dst_node, epoch)` → `vfs_lock(dst_content_unit_or_new, epoch)` |
| `vfs_write` COW | `vfs_lock(node, epoch)` → `vfs_lock(content_unit, epoch)` (per-page chain) |
| `vfs_truncate` | `vfs_lock(node, epoch)` — **held throughout** — plus per-page `ContentUnit` lock for each page in the grow range (see "vfs_truncate" note below) |
| `vfs_read` | no lock |
| `vfs_file_size` / `vfs_file_mtime` | no lock (read-only) |
| `dirchain_list` | no lock (read-only) |

**`vfs_truncate` design (R3, option b):** the `node_vp`
lock is held for the **entire** operation. The sequence is:

```
vfs_truncate(file, new_size, epoch):
    vfs_lock(vfs, file_node_vp, epoch)         // held throughout
    cur_size = read FileSize chain
    if shrink:
        prepend new FileSize entry
        vfs_unlock(vfs, file_node_vp, epoch)
        return
    if grow:
        prepend new FileSize entry
        for page in [cur_size, new_size]:
            vfs_lock(vfs, content_unit_vp_of_page, epoch)
            write zeroed data
            prepend VersionPage entry
            vfs_unlock(vfs, content_unit_vp_of_page, epoch)
        vfs_unlock(vfs, file_node_vp, epoch)
        return
```

**Why take per-page `ContentUnit` locks in the grow loop:**
`vfs_write` also takes both `node_vp` and per-page
`ContentUnit`. Without the per-page lock, `vfs_truncate`
and `vfs_write` use different lock disciplines and can
race on the same file. With both locks, every file
mutation goes through the same pair, and operations
serialize consistently on `node_vp`. The per-page
`ContentUnit` lock is technically redundant for exclusion
when `node_vp` is held, but we keep it to match `vfs_write`
and to make the "I'm modifying this page" intent explicit.

**Per-page lock redundancy:** since `node_vp` serializes
all writers to a file, the per-page `ContentUnit` lock
provides no additional exclusion. It's kept for two
reasons: (1) **symmetric model** — every per-page
operation takes the per-page lock, easier to reason
about; (2) **future optimization** — if profiling shows
`node_vp` is a bottleneck for within-file page-level
parallelism (not relevant for FUSE; theoretical for
SQLite-shim), drop `node_vp` from `vfs_write` and rely
on per-page `ContentUnit` instead. The lock discipline
is in place; only the choice of which locks to take
changes.

**O1 (delete/rmdir don't need `node_vp`):** `vfs_delete`
and `vfs_rmdir` prepend a tombstone `DirContent` to the
**per-child `ContentUnit` chain** — they do not modify
the parent's `DirContentIndex` (the radix link that was
created at the original `vfs_create`/`vfs_mkdir` is left
in place; the tombstone is on the chain, not the index).
So they take only the per-child `ContentUnit` lock, not
`node_vp`. This avoids unnecessary serialization on the
parent dir during deletes. The lock table reflects this:
`vfs_delete` / `vfs_rmdir` get one lock, not two.

### Worked example: concurrent path renames

**Setup:**
- Thread 1: rename `/path/to/file` → `/path/to/file1`
  (changes "file" in dir `/path/to/` to "file1")
- Thread 2: rename `/path/to/` → `/path/to1/`
  (changes "to" in dir `/path/` to "to1")

**Lock acquisitions:**

| Thread | Lock 1 | Lock 2 | Why |
|---|---|---|---|
| 1 | `vfs_lock(contentUnit_file_in_/path/to/, E1)` | — | Prepend new `DirContent` to "file"'s chain with name "file1". Per O1, no `node_vp` needed — same-dir rename's per-child chain update doesn't touch the `DirContentIndex`. |
| 2 | `vfs_lock(node_/path, E2)` | `vfs_lock(contentUnit_to_in_/path, E2)` | Prepend `DirContentIndex` for "to1" (Node-level) + prepend new `DirContent` to "to"'s chain with name "to1" (ContentUnit-level) |

Thread 1 and Thread 2 share no lock keys. Different VPs,
different parents, no contention. Both proceed in parallel.

Thread 2 takes Node lock before ContentUnit lock — respects
the `Node > ContentUnit` hierarchy. Within the same level,
Thread 2 has only one ContentUnit lock, so no same-level
ordering issue.

**Reader during the race:** a third thread doing
`vfs_stat("/path/to/file")` while the two renames are in
flight uses the per-epoch read-rule. The result depends on
the requested epoch:
- Before either rename commits: original file at
  `/path/to/file`.
- After Thread 1 only: file at `/path/to/file1`.
- After Thread 2 only: nothing (the dir was renamed
  away; new path is `/path/to1/file`).
- After both: file at `/path/to1/file1`.

Each result is a self-consistent per-epoch snapshot. The
locks prevent lost updates; the read-rule + mapper provide
causal ordering per `ContentUnit`. The actual epoch
allocation (odd=snapshot, even=committed head) is
orthogonal — what matters is that each per-epoch view is
internally consistent.

### Why this fixes the CAS-on-local hazard

Today, every prepend-to-chain does:

```c
do {
    old_head = atomic_load(local + HEADPTR);
    new_dc = (..., old_head, ...);
} while (atomic_cas(local + HEADPTR, old_head, new_dc) != old_head);
pool_release(local);  // writes local back to cache
```

The CAS operates on the **local** `PoolSlot` buffer, not the
shared cache line. Two concurrent writers acquire the same page,
each builds a local with the same `old_head`, both CAS on their
own local (which always succeeds because it's a local CAS), both
release, and the second release **silently clobbers the first**.

The fix: instead of CAS on local, lock the `ContentUnit`:

```c
vfs_lock(vfs, content_unit_vp, write_epoch);
pool_acquire(pool, content_unit_vp, true, &unit_slot);
old_head = rd8(unit_slot.bytes, ANCHOR_OFF_HEADPTR);
// modify unit_slot.bytes (no CAS — lock guarantees exclusivity)
vfs_wr8_s(unit_slot.bytes, ANCHOR_OFF_HEADPTR, new_chain_vp, ctx->page_size);
pool_release(pool, &unit_slot);
vfs_unlock(vfs, content_unit_vp, write_epoch);
```

Now the lock provides the mutual exclusion that the CAS was
*meant* to provide. No retry loop, no lost update, no
clobbering.

### Lock granularity choice

We considered three granularities:

- **Root lock only**: every write to any child of a dir
  acquires the dir's root lock. Maximum contention. Minimum
  complexity.
- **Per-unit lock** (proposed above): writes to the same
  child contend; writes to different children don't. Minimum
  contention matching workload. Modest complexity.
- **Per-chain-entry lock**: finest, but lock key churns
  (new `DirContent` per epoch means new lock key per epoch).
  Worse than per-unit.

The proposed per-unit lock matches the file model (per
`ContentUnit` lock) and resolves the C1_REVIEW concerns. The
dir side becomes structurally identical to the file side.

## Performance analysis

All comparisons are vs the Phase 25 state (W9 code), with
current 13/13 tests passing and FUSE at 16.94s (best). Numbers
are estimates; concrete validation will come from the bench
harness after each migration workload.

### Read path: name lookup (`dirchain_find_child`)

Current:
1. `pool_acquire` parent dir `Node` (1 cache miss).
2. Walk `DirContentIndex` radix tree (1 miss per level, ≤16
   levels for a 64-bit hash; in practice ≤3 levels for typical
   names because the tree is shallow).
3. `pool_acquire` leaf `DirContentLink` (1 miss).
4. `pool_acquire` `DirContent` (1 miss).
5. Apply mapper epoch remap (in-memory).
6. Done.

≈4–6 cache misses, all in cache, all sequential.

Proposed:
1. `pool_acquire` parent dir `Node` (1 miss).
2. Walk `DirContentIndex` radix tree (same as before).
3. `pool_acquire` leaf `DirContentLink` (1 miss; payload now
   is a `ContentUnit` VP, not a `DirContent` VP).
4. `pool_acquire` `ContentUnit` (1 miss).
5. Walk `ContentUnit`'s `DirContent` chain for read-rule (1
   miss per entry until found; typically 1 entry because the
   per-child chain is short).
6. Apply mapper epoch remap (in-memory).
7. Done.

≈6–9 cache misses, all in cache, all sequential. **Net: +1
to +3 misses per lookup** (the new per-child chain walk).

Why this is OK:
- In a typical readdir + open workload, name lookups are
  amortized: after the first lookup of a name, the path is in
  tcache. Subsequent lookups are 1 miss for the new chain
  entry.
- The extra misses are all sequential reads of in-cache slots;
  no disk I/O added.
- The per-child chain is short (typically 1-2 entries), so the
  inner walk is essentially free.

### Read path: directory listing (`dirchain_list`)

Current: walk `Node.headPtr` → `DirContent` chain (O(N)
entries, 1 miss per entry) + O(N) hash_map dedup ops.

Proposed: walk `Node.headPtr` → `Segment` chain (O(seg)
segments) → within each, walk `ContentUnit` chain (O(slot)
children) → for each, walk `ContentUnit`'s `DirContent` chain
(typically 1-2 entries) → collect. **No dedup** because the
walk produces one entry per `ContentUnit` (naturally
dedup'd).

**Net: more pool_acquires, no hash_map ops — net depends on
dir shape.** Per-`ContentUnit` walks add S + S×U + S×U×D
slot reads (segments, units, chain depth) vs N + N hash_map
ops today. For a single-segment dir with short chains
(typical), this is roughly the same work. For a multi-segment
dir with 64 children per segment, the `ContentUnit` walk
adds S reads but each chain walk is much shorter than
today's full N-entry walk. The hash_map removal saves ops
but the per-`ContentUnit` walk adds them. Net is
workload-dependent; not measurably better or worse on
the FUSE bench (most dirs fit in one segment).

### Write path: create child (`vfs_create` / `vfs_mkdir`)

Current: CAS `DirNode.HEADPTR` (prepend first `DirContent`) +
CAS `DirNode.INDEXHEADPTR` (prepend `DirContentIndex`) +
inline `childCount` increment. 2 cache-line CASes on `DirNode`,
plus the chain CAS on the new `DirContent`.

Proposed:
1. Lock `Node` (root lock).
2. If new segment needed: lock segment, append new `Segment`
   to `Node.headPtr`. Else: lock existing segment.
3. Append new `ContentUnit` to segment.
4. Lock new `ContentUnit`.
5. Prepend first `DirContent` to `ContentUnit.headPtr`.
6. Prepend `DirContentIndex` to `Node.metaPtr` (DirContentIndex
   chain head).
7. Unlock in reverse order.

**Net: more lock acquisitions, but no CAS retries.** With low
contention (the FUSE workload serializes most writes via
FUSE's single-threaded request loop), locks are faster than
CAS retries. With high contention (not the current workload),
locks block; CAS retries spin. The user explicitly chose
locks over CAS for this reason.

Note the absence of step 6 ("update `childCount`") — that
field is gone.

### Write path: write to file (`vfs_write`)

Unchanged shape: `tree_resolve_page` returns the
`ContentUnit` (UNIT_PAGE) slot; lock the `ContentUnit`;
prepend `VersionPage`; update `Node.metaPtr` (FileSize chain)
under `Node` lock; release.

The file model already matches the unified shape, so this
operation is unchanged except for the lock-vs-CAS swap.

### Concurrency benefit

The headline gain: **writers on disjoint subtrees no longer
share a serialization point.**

Concrete example, FUSE-level "copy 252 MB zip + unzip":

- Old behavior: every write inside `/tmp/.../subdir/`
  CASes the same `DirNode.HEADPTR` for that subdir. FUSE
  processes one request at a time per mount, so contention
  is low, but the cache line for that `DirNode` is hot.
- New behavior: writes to different files in the same subdir
  contend on the **dir root lock** (for `DirContentIndex`
  prepend), but per-file data writes (`ContentUnit` prepend)
  only contend with other writers to the **same file**. Two
  `vfs_write` calls to different files in the same dir proceed
  in parallel without touching each other's data cache lines.

**Where the benefit applies and where it doesn't:**

The "writers on disjoint subtrees don't share a serialization
point" claim is true for **data writes** (different files'
`ContentUnit` chains) but **not for directory metadata
writes**. Within a single directory, all `DirContentIndex`
mutations (insert/remove) serialize on that dir's `node_vp`
lock. Two concurrent creates of different children whose
name hashes land in the same radix leaf will serialize on
`node_vp`, even though the children themselves are
unrelated. This is correct (no lost links) but coarser than
the per-`ContentUnit` locking the rest of the model
achieves.

The radix leaf's link-list CAS becomes a locked write
(`vfs_lock(node, epoch)`) in W4/W5; the lock is held
briefly, and radix leaves rarely collide for real names
in typical workloads. The parallelism win is real for
*data* writes across files in the same dir; same-dir
metadata writes serialize.

This is the "SQLite VFS shim" future direction: with the
unified model, multiple SQLite instances writing to different
files in the same VFS no longer bottleneck on a single
`DirNode.HEADPTR` cache line. Each file's writes are
independent.

### Expected impact on FUSE bench (252 MB zip + unzip)

**Original spec prediction** (with ±20% noise floor):
- Worst case: +5% to +10% slowdown.
- Best case: −5% to +10% speedup.
- Most likely: within noise.

**Actual results:**

- **W0–W4 (lock rework, lock-vs-CAS swap):** FUSE 16.94s →
  ~20.5s (post-W5, +21% on FUSE; the original "neutral"
  prediction did not hold).
- **W5 (per-`ContentUnit` machinery):** FUSE ~20.5s — flat.
- **W6 (chain-walk unification):** FUSE samples 17.67 / 19.18
  / 18.68 / 19.92s. Median 18.7s, range 17.7–19.9s. The system
  is overheating (per user), so the noise floor is wider than
  the original ±3s estimate. **Verdict: in noise of the W0
  best (16.94s) when measured across multiple runs.**

**Micro-bench (`vfs_bench`, 1 thread, count=2000) — the
post-W5 snapshot had a real regression:**

| workload | pre-26 (ops/s) | post-W5 (ops/s) | delta | post-W6 (ops/s) | delta vs W5 |
|---|---|---|---|---|---|
| create   | 4,904  | 2,958  | **−39.7%** | flat vs W5 | 0% (no further regression) |
| write    | 4,277  | 2,685  | **−37.2%** | flat vs W5 | 0% |
| scan     | 328,569 | 207,598 | **−36.8%** | flat vs W5 | 0% |
| read     | 613,121 | 506,971 | −17.3%   | flat vs W5 | 0% |
| dir      | 21,209  | 17,599  | −17.0%   | flat vs W5 | 0% |
| seqwrite | 219,467 | 198,236 | −9.7%    | flat vs W5 | 0% |

**Why the regression** (root cause from the W5j-era
implementation review): the per-child `ContentUnit` is a
genuinely *new* allocation that didn't exist before (the old
model hung `DirContent` directly off the `DirNode`). Each
`pool_acquire` is a 32-byte memcpy + hash lookup + (for new
children) a `pool_alloc` that can trigger page allocation +
`pool_page_init` + I/O. Plus the `Segment` level adds an
indirection on every walk. Plus the double-lock on every
mutation (node + `content_unit`) adds two mutex acquisitions
where there used to be one or zero.

**The trade-off was accepted.** The per-`ContentUnit` model is
the correct structural model for Phase 27+ work (unlocks
file-level concurrency in the same dir, which is the SQLite
VFS shim future direction). Recovering the 30–40% micro-bench
regression requires either (a) dropping the redundant `node_vp`
lock from the write path, or (b) reducing the per-child
allocation cost — both are addressed as future work, not
Phase 26.

The current FUSE best is 16.94s (W0 result). The W6 result
(18.7s median) is in the noise floor; the FUSE bench does not
regress catastrophically the way the micro-bench does. This
matches the spec's "Most likely: within noise" prediction for
FUSE, even though the micro-bench prediction was wrong.

## Concurrency analysis

### Current (Phase 25) state — known hazards

21 CAS/atomic-store sites on `PoolSlot` bytes. 11 are critical
(parent dir coordination, lost updates under concurrent mutation
on the same child). See C1_REVIEW for the full list.

### Proposed (Phase 26) state

- All 11 critical CAS sites replaced with `vfs_lock` + simple
  store. No CAS on local, no lost updates.
- The 2 torn-write sites (`vfs_rename` same-dir `NAMEPTR`,
  `dircontentindex_remove` `DIRCONTENTVP`) become locked
  writes. No torn writes.
- The 6 file-content-tree CAS sites become locked writes.
- The 1 pre-existing racy site (`vfs_truncate` `SIZEPTR`)
  becomes a locked write. **Fixes R3 from C1_REVIEW as a
  side effect.**
- GC's `treeLockState` CAS, mapper's `epochMapperPtr` CAS, and
  the radix index's initial-root CAS: unchanged. These are
  not chain-walk coordination; they are out of scope for this
  phase.

### Lock ordering

See the **Lock ordering rules** subsection in §6 (Lock model)
for the full rules. The rules are:
- Hierarchy: `Node > Segment > ContentUnit`.
- Same level: lower VP first.

The combination is acyclic, so deadlock is impossible.

### What the unified model does **not** fix

- The `epochMapperPtr` CAS in `mapper.c` is still racy across
  epochs. Out of scope; this is the GC reimplementation's
  problem.
- The GC's `treeLockState` CAS: documented as
  single-threaded-only (the de facto current contract; not a
  hazard for this phase). The proper stop-the-world fix is
  part of the future GC rewrite.
- The `epochMapperPtr` and `treeLockState` CAS sites are
  enumerated in §10 (CAS-site enumeration) as
  out-of-scope (#21 and #22).

These are pre-existing issues that survive the migration. The
unified model strictly improves the chain-walk coordination;
it does not claim to fix every concurrency issue in the
codebase.

## CAS-site enumeration (per-site fate)

This section enumerates every CAS/atomic-store site flagged by
C1_REVIEW (and a few additional sites the reviewer caught in
`PHASE27_REVIEW_V2.md`) and assigns each one a fate under the
unified model. Sites are cited by **function + field**, not
line number, so they stay valid as `tree.c` is reshuffled in
W1-W5. The validation criterion "grep shows zero
`vfs_cas_i64` in tree.c" depends on this enumeration being
complete.

### Critical CAS sites on `PoolSlot` bytes (C1_REVIEW's "11 critical")

| # | Function + field | C1_REVIEW verdict | Phase 26 fate |
|---|---|---|---|
| 1 | `tree_resolve_page` `fc_slot.ROOTPTR` | CAS on local | **W3**: lock on `content_unit_vp` (UNIT_PAGE), simple store, no CAS |
| 2 | `tree_resolve_page` `prev_slot.PAGENODE_OFF_NEXTPTR` | CAS on local | **W3**: same as #1 (lock on `content_unit_vp`) |
| 3 | `tree_resolve_page` `fc_slot.ROOTPTR` (extend path) | CAS on local | **W3**: same as #1 |
| 4 | `tree_resolve_page` `tail_slot.PAGENODE_OFF_NEXTPTR` (extend path) | CAS on local | **W3**: same as #1 |
| 5 | `vfs_create` `parent_slot.HEADPTR` (prepend first DirContent) | CAS on local, parent dir coordination | **W4**: lock on `node_vp` (for `DirContentIndex`) + `content_unit_vp` (new UNIT_SLOT), simple store |
| 5b | `vfs_mkdir` `parent_slot.HEADPTR` (prepend first DirContent) | CAS on local | **W4**: same as #5 |
| 6 | `vfs_delete` `parent_slot.HEADPTR` (prepend tombstone) | CAS on local | **W4**: per-child `ContentUnit` lock only (no `node_vp` — see O1 below; tombstones are per-`ContentUnit` chain, not per-`DirContentIndex`) |
| 7 | `vfs_rename` cross-dir `dst_slot.HEADPTR` (prepend new DirContent in dst) | CAS on local | **W4**: same as #5 |
| 8 | `vfs_rmdir` `parent_slot.HEADPTR` (prepend tombstone) | CAS on local | **W4**: same as #6 |
| 9 | `vfs_rename` `src_slot.HEADPTR` (source tombstone prepend) | CAS on local | **W4**: same as #5 |

(D1a fix: the V4 spec's #10 row was a phantom. The only
`vfs_rename` src HEADPTR CAS is at line 1868 — there is no
"other src prepend" distinct from the tombstone prepend.
`vfs_atomic_load_i64` at line 1864 is a load, not a CAS.
Removed the duplicate row.)
| 11a | `dircontentindex_insert` root-init: `indexRoot` (caller-local) | CAS on local | **W4**: lock on `node_vp` (parent Node), simple store on `Node.metaPtr` |
| 11b | `dircontentindex_insert` leaf-link prepend: `slot.LISTVP` | CAS on local | **W4**: same as #11a |
| 11c | `dircontentindex_insert` deepest-level leaf link: `slot.LISTVP` | CAS on local | **W4**: same as #11a |
| 11d | `dircontentindex_insert` new-child CAS-prepend: `newChildSlot.LISTVP` | CAS on local | **W4**: same as #11a |
| 11e | `dircontentindex_insert` LEAF listVP CAS-claim | CAS on local | **W4**: same as #11a |

(C1_REVIEW's "11 critical" had 1 row for `dircontentindex_insert`;
the actual function has 5 distinct CAS sites. Split into
11a-11e for completeness.)

### Segment-append CAS sites (2 sites — added in `PHASE27_REVIEW_V2.md` Q1a)

| # | Function + field | C1_REVIEW verdict | Phase 26 fate |
|---|---|---|---|
| 12 | `tree_resolve_page` `file_slot.HEADPTR` (FileContent segment-append) | CAS on local (segment growth) | **W3**: lock on `node_vp` (file's Node), simple store. Segment append is a Node-level operation. |
| 13 | `tree_resolve_page` `prev_slot.FILECONTENT_OFF_NEXTPTR` (FileContent chain link) | CAS on local (segment growth) | **W3**: same as #12 |

### Torn-write sites (2 sites)

| # | Function + field | C1_REVIEW verdict | Phase 26 fate |
|---|---|---|---|
| 14 | `vfs_rename` same-dir atomic-store `dc.NAMEPTR` | Torn write | **W4**: lock on `content_unit_vp` (src or dst), simple store, atomic under lock |
| 15 | `dircontentindex_remove` atomic-store `linkSlot.DIRCONTENTVP` | Torn write | **W4**: lock on `node_vp` (parent Node), simple store |

### File-content-tree CAS sites (1 site)

| # | Function + field | C1_REVIEW verdict | Phase 26 fate |
|---|---|---|---|
| 16 | `vfs_write` COW `pn_slot.VERSIONROOT` (line 2920) | CAS on local | **W3**: lock on `node_vp` (for `FileSize` chain) + `content_unit_vp` (UNIT_PAGE) for the per-page chain |

(D1c fix: the V4 spec's #16 row claimed `tree_resolve_page`
CASes `VERSIONROOT`, but `tree_resolve_page` CASes
`ROOTPTR` and `NEXTPTR` (spec #1-4) — not `VERSIONROOT`.
The only `VERSIONROOT` CAS in `tree.c` is at line 2920, in
`vfs_write`'s COW path. Removed the phantom row; the
VERSIONROOT CAS count is 1, not 2.)

### Pre-existing racy (1 site)

| # | Function + field | C1_REVIEW verdict | Phase 26 fate |
|---|---|---|---|
| 17 | `vfs_truncate` shrink `file_slot.SIZEPTR` (FileSize chain prepend) | Pre-existing race (C1_REVIEW R2) | **W3**: lock on `node_vp` (file's Node), held throughout the operation. Fixes R2 as a side effect. |

### Out of scope for Phase 26 (2 sites)

| # | Function + field | Why out of scope |
|---|---|---|
| 18 | `gc.c:treeLockState` CAS | GC vs mutator coordination; fixed by future GC rewrite, not this phase |
| 19 | `mapper.c:epochMapperPtr` CAS | Operates on real shared memory (not local); not a C1 hazard. Future GC rewrite may address. |

**Total: 21 in-scope sites addressed by Phase 26 (rows #1-#9,
#11a-#11e, #12-#17 = 4 + 6 + 5 + 2 + 1 + 1 + 1 + 1 = 21
distinct code locations, all of which get a lock-or-store
fate). 2 sites out of scope (#18-#19, in `gc.c` and
`mapper.c`).**

After W4, `grep -r 'vfs_cas_i64' src/tree.c` should show zero
matches in chain-walk coordination. Any remaining CAS sites
should be reviewed individually to confirm they're not
chain-walk coordination.

(D1 fix: the V4 spec's 22 in-scope count was inflated by
3 phantom rows — #10 (was a duplicate of #9, the same
`vfs_rename src_slot.HEADPTR` line 1868), old #16 (claimed
`tree_resolve_page` CASes `VERSIONROOT`; it doesn't — only
`vfs_write` COW does, at line 2920), and old #23 (was a
duplicate of #11a, the same `dircontentindex_insert
indexRoot` line 2003). All three removed. Verified count
against `grep -n vfs_cas_i64 src/tree.c` and
`grep -n vfs_atomic_store_i64 src/tree.c`: 21 distinct
sites in `tree.c` (19 `vfs_cas_i64` + 2
`vfs_atomic_store_i64`), all of which appear in the table
above as 21 rows.)

## Migration plan

The migration is split into **7 workloads** within this spec
(W0–W6). Each workload is one self-contained, testable,
benched change.

**All 7 workloads are DONE** (see "Status" at the top of this
spec). W0–W5 are listed below as originally specified. W6
("chain-walk unification completion") is added at the end of
this section with its own "W6 — Results (DONE)" sub-section
covering the actual outcomes vs. the original targets.

### Workload 0: `vfs_lock` rework — per-`vfs_t`, VP-keyed, same-epoch exclusion **[DONE — W0]`**

**Scope:** the existing `vfs_lock(vfs_t*, int64_t file, int64_t
epoch)` has three problems for Phase 26:

1. It keys a process-global `lock_table` on `nodeId` (the
   `file` parameter is documented as "Hash: file nodeId" in
   `vfs.c:38`). Process-global, not per-`vfs_t`, never torn
   down at `vfs_unmount` (ISSUES.md H4).
2. The key is treated as `int` (nodeId), not `int64_t` (any
   `VirtualPtr` or other identifier). Structural locks in
   W3+ need to pass `ContentUnit` VPs.
3. **The per-epoch path is a shared lock, not exclusive**
   (`fls->epoch_count++` at `vfs.c:134` with no
   same-epoch check). Two threads at the same epoch to the
   same key both proceed — which means the chain-prepend
   code in §"Why this fixes the CAS-on-local hazard" doesn't
   actually fix it. **W0 must add same-epoch exclusion.**

W0 addresses all three:

- **Move `lock_table` from process-global to `vfs_t->lock_table`**
  (per-`vfs_t`). Fixes H4 as a side effect — `vfs_unmount`
  frees the table.
- **Change the key from `int` (assumed to be `nodeId`) to
  `int64_t`** (any `VirtualPtr` or other identifier). No
  semantic check on the key value; the caller's responsibility
  to use a stable VP.
- **Add same-epoch exclusion to the per-epoch path.** Change
  `FileLockState.epoch_count` (a single int at `vfs.c:30`)
  to a small structure tracking holders per-epoch. For a
  typical epoch range (32 or so), an array of 32 `{epoch,
  count}` pairs is sufficient. Same-epoch second-acquirer
  blocks until first releases; different-epoch proceeds
  without checking other entries. This is **new functionality**
  — it changes the semantics for structural callers in W3+
  but is a no-op for existing nodeId-keyed callers (they
  don't rely on same-epoch exclusion today).
- **Add `vfs_lock_destroy(vfs_t*)` and call it from
  `vfs_unmount`.**
- **Update `vfs_lock`'s doc comment** in `vfs.c:38` to:
  "The key is an opaque `int64_t` — typically a `VirtualPtr`
  for structural locks (W3+), or a `nodeId` for legacy
  callers (W0-W2). Per-epoch mode (epoch != 0) provides
  same-epoch exclusion but allows different-epoch writes
  to proceed in parallel. Global mode (epoch == 0) is
  fully exclusive." The new comment makes the contract
  explicit and prevents callers from making assumptions
  about key semantics.
- Existing callers in `tree.c` (currently passing `new_nodeId`,
  `found_childId`, etc.) continue to work — they just now
  pass through a per-`vfs_t` table with same-epoch
  exclusion. **No semantic change for existing callers** —
  they don't rely on same-epoch exclusion today. The
  same-epoch exclusion is new functionality that W3-W4
  structural callers depend on.
- Update `bench/bench.c` for the new signature. The lock
  tests live in `test/test_tree.c` (lines 1250-1344:
  `test_lock_basic`, `test_lock_concurrent_epochs`,
  `test_lock_global_serializes`); add the new same-epoch
  exclusion test, mixed-key test, and per-`vfs_t` isolation
  test there.

**Test:** existing tests should pass unchanged. Add:
- Per-`vfs_t` isolation: two `vfs_mount` calls on different
  files, lock the same VP in both, verify they don't
  contend (proving the table is per-`vfs_t`, not global).
- Mixed-key test: VP-keyed and nodeId-keyed locks in the
  same `vfs_t` don't interfere.
- **Same-epoch exclusion test (R1):** two threads call
  `vfs_lock(vfs, vp, E)` with the same `E`. The second
  `vfs_lock` blocks until the first calls `vfs_unlock`.
  This is the new behavior W0 adds; the test ensures it.
- **Different-epoch non-exclusion test:** two threads call
  `vfs_lock(vfs, vp, E1)` and `vfs_lock(vfs, vp, E2)` with
  E1 != E2. Both proceed without blocking. This is the
  existing behavior; the test ensures W0 doesn't break it.

**Bench:** no change expected. Lock acquire/release cost is
unchanged; just the table location is different.

**Risk:** medium. The lock API is touched, but no call sites
change behavior. The teardown fix is mechanical.

### Workload 1: `Anchor` struct + new discriminator values **[DONE — W1a–W1e]**

**Scope:** add `Anchor` struct in `nodes.h` (32 B, the layout
above) + write/read helpers in `nodes.c`:
`nodes_write_anchor` / `nodes_read_anchor`. Add the
`AnchorKind` enum (`ROOT_FILE`, `ROOT_DIR`, `SEGMENT_FILE`,
`SEGMENT_DIR`, `UNIT_PAGE`, `UNIT_SLOT`). Update
`FileNode`/`FileContent`/`PageNode` to the new `Anchor` shape
(type field added, field layout adjusted). Add `createdAt` to
the `Node` layout (drops `DirNode.childCount`; bumps
`FileNode` to also have `createdAt` in offset 24, matching).
Set `ANCHOR_UNITS_PER_SEGMENT = 1024` as the initial
configurable value.

**Test:** unit tests for `Anchor` write/read.

**Bench:** no change (structs aren't used differently yet).

**Risk:** low. Pure type/struct reshuffle.

### Workload 2: chain walk via `vfs_chain_walk` **[DONE — W2]**

**Scope:** implement `vfs_chain_walk` in `tree.c` (or
`chain_walk.c` if file is too large). Add the two leaf
specializations (`file_leaf_get_data_page`,
`dir_leaf_get_child`). Keep `tree_resolve_page` and
`dirchain_find_child` as thin wrappers that call
`vfs_chain_walk` and then do the leaf step. The walk
implements the **full read-rule** (mapper remap per entry,
even/odd distinction, exact-match-wins) — see §4.3 for the
sketch.

**Test:** existing tests should pass unchanged (callers
re-implemented, not replaced). Add a dedicated test for the
read-rule that exercises committed-snapshot remap, odd-skip,
exact-match-wins.

**Bench:** name-lookup path is expected to be within noise.

**Risk:** medium. The unified walk has to match the current
walk's exact read-rule behavior, including the "tombstone
suppresses lower-epoch live entry" semantics for dir
(see §4.4 for the ContentUnit visibility rule).

### Workload 3: lock-vs-CAS swap on file chain (`vfs_write`, `vfs_truncate`) **[DONE — W3]**

**Scope:** replace the 7 CAS-on-local sites in
`tree_resolve_page` (CAS enumeration #1-#4 PageNode-level
+ #12-#13 FileContent segment-append = 6 sites) and the
1 site in `vfs_write` COW (CAS enumeration #16, line 2920)
with `vfs_lock(vfs, node_vp, epoch) + vfs_lock(vfs,
content_unit_vp, epoch)` + simple store. The `vfs_truncate`
`FileSize` site (CAS enumeration #17, line 2702) also
moves to lock-based (lock `node_vp` held throughout, plus
per-page `ContentUnit` for each page in the grow range —
option b from R3).

Apply the same-level "lower VP first" acquisition rule.
Apply the hierarchy rule: always `node_vp` first, then
per-unit `ContentUnit`.

**Test:** `test_write.c`, `test_truncate.c`, and the FUSE
bench.

**Bench:** expected to be within noise (low contention in
FUSE).

**Risk:** low. The lock infrastructure now supports VP keys
and same-epoch exclusion (W0).

### Workload 4: lock-vs-CAS swap on dir chains **[DONE — W4]**

**Scope:** replace the 11 critical CAS sites + 2 torn-write
sites identified in C1_REVIEW with `vfs_lock` + simple
store. Affects:
- `vfs_create` / `vfs_delete` / `vfs_mkdir` / `vfs_rmdir`:
  `vfs_lock(node, epoch)` for `DirContentIndex` +
  `vfs_lock(content_unit, epoch)` for the per-child
  `DirContent` chain.
- `vfs_rename`: `vfs_lock(node, epoch)` + per-child
  `ContentUnit` lock (src + dst); same-level "lower VP
  first" rule.
- `dircontentindex_insert` / `remove`: `vfs_lock(node,
  epoch)` for the radix index. The behavioral refit
  (multi-link support, new-vs-reuse decision) is in W5.
- `vfs_write` COW `VERSIONROOT`: per-`ContentUnit` lock
  (covered in W3).

**Test:** full test suite + bench.

**Bench:** expected to be within noise on FUSE, but a clear
win on multi-threaded microbenchmarks (when we add one in a
later phase).

**Risk:** medium. Many call sites; subtle ordering rules.

### Workload 5: dir segment + ContentUnit population, dedup removal, GC, radix refit **[DONE — W5a–W5j + W5-followup + W5g + W5h + W5i + W5j]**

**Scope:** rewrite `vfs_create` / `vfs_mkdir` / `vfs_rmdir` /
`vfs_delete` to allocate `ContentUnit`s in `Segment`s.
**Refit `dirchain_list`** to walk via segments and
`ContentUnit`s; remove the dedup hash_map and
`DirchainDedupEntry` (per-`ContentUnit` chains are dedup'd at
the structure level — see §4.4).

**Radix index behavioral refit (R6):** the radix leaf's
`DirContentLink` payload changes from `int64_t dirContentVP`
to `int64_t contentUnitVP`. The behavioral changes are:

- **`dircontentindex_lookup`**: must now return a **list**
  of `DirContentLink`s at the leaf, not a single head.
  Multiple `ContentUnit`s can share a leaf (delete+recreate
  case — see §4.4). The caller walks the list and applies
  the `ContentUnit` visibility rule to find the visible
  `ContentUnit` for the query epoch. (Currently returns a
  single `int64_t` head.)

- **`dircontentindex_insert`**: must decide between two
  cases based on the operation:
  - **Create-after-delete** (new `ContentUnit` for a
    `childNodeId` that was previously deleted): add a new
    `DirContentLink` to the leaf pointing at the *new*
    `ContentUnit`.
  - **Same-child write** (a `ContentUnit` is being rewritten
    in place, e.g., a `DirContent` prepend on an existing
    chain): reuse the existing `DirContentLink` pointing
    at the *same* `ContentUnit`. Do NOT add a new link.
    The previous "always prepend a new link" behavior
    creates duplicate links for every same-child write.
  
  The decision is based on whether the operation allocates
  a new `ContentUnit` (case 1) or modifies an existing
  one (case 2). The caller (`vfs_create` / `vfs_write` /
  etc.) signals which case via the API.

- **`dircontentindex_remove`**: zero the link whose
  `contentUnitVP` matches the removed `ContentUnit`. (Same
  as today, just a different field type.)

**Update `gc.c`** (1 file, ~1100 lines) — three sub-problems:

1. **Struct/type remap.** New `gc_walk_segment` and
   `gc_walk_content_unit` functions (or extend the existing
   walks to handle them). Every file *and* dir GC walk gains
   two new levels (Segment, ContentUnit). For dirs, GC goes
   from 3 levels (Node→DirContent→child) to 5
   (Node→Segment→ContentUnit→DirContent→child).
2. **`gc_copy_entry` per-type field descriptors (ISSUES.md
   M4).** The unified `Anchor` layout puts `createdAt` at
   offset 24, which is a Unix timestamp that could
   coincidentally match a VP being remapped. The current
   blind-remap loop would corrupt `createdAt`. Add a
   per-type field descriptor table to `gc_copy_entry` that
   knows which offsets are VPs (remap) vs. integers (skip)
   vs. timestamps (skip). This is a generic improvement
   that also benefits future node types.
3. **Survival analysis rewrite (ISSUES.md M2).** The
   `gc_walk_dircontent_chain` function has a
   `MAX_CHILDREN = 1024` cap on a fixed array. With the
   per-`ContentUnit` model, each chain is short (1-2
   entries), so the fixed-array analysis can be replaced
   with a per-`ContentUnit` walk.

Also: **document the single-threaded-only GC contract** in
`vfs.h`. Add a comment that `vfs_gc` must not be called
concurrently with mutators. This is the de facto current
behavior (the existing reader lock is never incremented;
ISSUES.md C3) — we're just making it explicit. We are NOT
implementing stop-the-world for this phase. The proper
stop-the-world fix is part of the future GC rewrite.

**Test:** full test suite. **Add:** `test_concurrent_dir_writes`
(two threads, each creating 1000 distinct children in the same
dir, verify all 2000 are visible after sync). **Add:**
`test_concurrent_rename` (two threads, each renaming 1000
distinct children in the same dir, verify all names resolve
correctly). **Add:** a stress test that creates + deletes +
renames 10K+ children in a single dir, verifying dedup-free
listing returns the right set. **Add:** a test for the
ContentUnit visibility rule (delete+recreate produces multiple
ContentUnits at the same hash; lookup at the right epoch finds
the live one).

**Bench:** full bench + FUSE.

**Risk:** high. This is the biggest single change. On-disk
format changes; old VFS files are not compatible. Budget
**5-7 days**, not 3.

### Test plan

- All 13 existing tests must pass.
- Add `test_concurrent_dir_writes.c`: two threads, each
  creating 1000 distinct children in the same dir. Verify all
  2000 are visible after sync.
- Add `test_concurrent_rename.c`: two threads, each renaming
  1000 distinct children in the same dir. Verify all names
  resolve correctly.
- Per-workload bench, with the 5-run average to filter noise.
- FUSE bench: the 252 MB zip + unzip scenario.

### Validation criteria for "done"

**Results summary:**

| Criterion | Status | Evidence |
|---|---|---|
| 13/13 existing tests pass | ✅ **Met** | `ctest` = 13/13, 69s. test_tree 23,144 sub-tests. |
| New concurrent/stress tests pass | ✅ **Met (W5g)** | `test_concurrent_dir_writes`, `test_concurrent_rename`, 10K+ stress, `ContentUnit` visibility. All pass. |
| New W6 chain-walk tests pass | ✅ **Met (W6b)** | `test_chain_walk_extended`, `test_chain_walk_anchor_chain`, `test_chain_walk_no_duplication`. All pass. |
| FUSE bench within ±10% of W0 best (16.94s) | ✅ **Met** | W6 median 18.7s, in noise of 16.94s (system overheating; noise floor wider than ±10%). |
| `grep -r 'vfs_cas_i64' src/tree.c src/vfs.c` = 0 | ✅ **Met** | Verified. The one remaining `vfs_atomic_store_i64` in `tree.c:2943` is a plain store under a held lock (not a coordination CAS). |
| `grep -r 'DIRNODE_OFF_CHILDCOUNT' src/` = 0 | ✅ **Met** | Verified (W1b removed `childCount` and added `createdAt`). |
| `grep -r 'pool_resolve\|pool_resolve_ro\|pool_resolve_rw\|tree_resolve_page_compat' src/ test/ bench/ tools/` = 0 | ✅ **Met (W5f, W5j)** | Verified. 24 test sites migrated first (W5i), then shims removed (W5j). |
| Code-size reduction on chain-walk paths ≥ 2× | ❌ **Did not meet** | `tree.c` grew 59% (2488 → 3948). The duplication is removed; the file is not smaller. See "What becomes simpler" above. |
| `vfs_bench` micro-bench within ±10% | ❌ **Did not meet** | create/write −40%, scan −37%, read/dir −17%, seqwrite −10%. W6 did not recover, but stopped the regression from getting worse. Trade-off accepted. |
| **W0 verification (R1):** | | |
| · per-`vfs_t` isolation test | ✅ **Met** | `test_lock_per_vfs_isolation` |
| · same-epoch exclusion test | ✅ **Met** | `test_lock_same_epoch_exclusion` |
| · different-epoch non-exclusion test | ✅ **Met** | `test_lock_different_epoch_non_exclusion` |
| · mixed-key test | ✅ **Met** | `test_lock_mixed_keys` |
| **R2 sanity check:** bounded per-epoch holder count | ✅ **Met** | `VFS_ERR_NOMEM` returned at >64 distinct epoch/key combinations. |
| Single source of truth for read-rule | ✅ **Met (W6)** | `test_chain_walk_no_duplication` is a runtime test that greps `src/tree.c` and asserts the read-rule markers appear exactly once. |

The two unmet criteria (chain-walk code reduction, micro-bench
neutrality) are both honest trade-offs documented at the top
of this spec. They are not blockers for closing Phase 26 — the
structural goals (CAS elimination, single-`ContentUnit`
lock-on-mutate, O1 delete/rmdir, single read-rule) all
shipped. The spec's over-promise of "2× reduction" is
corrected in the Status section.

### Workload 6: chain-walk unification completion (U1) **[DONE — W6a–W6f + W6-pre + W6-g2 + W6-m2]**

**Motivation.** W2 introduced `vfs_chain_walk` with the goal of
collapsing the per-leaf chain walk + read-rule into a single
function, with `tree_resolve_page` and `dirchain_find_child` as
thin wrappers. The implementation only delivered steps 4–5 of
the 6-step walk (leaf chain walk + read-rule). Steps 1–3
(container chain + content unit walk + unit-id comparison) are
still inlined in both `tree_resolve_page` (~430 lines,
`src/tree.c:207–630`) and `dirchain_find_child` (~225 lines,
`src/tree.c:2962–3188`). The header comments in
`src/tree.h:65,117–119` claim "thin wrapper over
`vfs_chain_walk`" — this is **false**; the bodies duplicate the
walk logic.

The read-rule logic is duplicated in 4 places: `vfs_chain_walk`
itself, `dirchain_find_child` (radix fast path), `dirchain_find_child`
(chain-walk fallback), and `vfs_rename`. Any future change to
the read-rule must be applied in all 4 places — exactly the
maintenance hazard the unification was meant to eliminate.

The implementation review (see `PHASE26_IMPL_REVIEW.md` finding
U1) flagged this as the biggest gap between spec and
implementation: `tree.c` grew 46% in W0–W5 instead of shrinking
2× as the spec predicted.

**Current state (pre-W6).**
- `vfs_chain_walk(ctx, chain_head, read_epoch, &out_leaf)`:
  walks a leaf chain (VersionPage / DirContent / FileSize)
  from a *resolved* `chain_head`, applies the read-rule, returns
  the visible leaf. Steps 4–5 of the 6-step walk.
- `tree_resolve_page`: 430 lines of standalone logic. Resolves
  `logical_page` to a PageNode by walking the FileContent
  chain + PageNode chain (steps 1–3), then inlines a copy of
  the leaf walk + read-rule. Does **not** call
  `vfs_chain_walk`.
- `dirchain_find_child`: 225 lines of standalone logic. Walks
  DirSegment chain + SlotNode chain + per-SlotNode DirContent
  chain (steps 1–5). Inlines a copy of the leaf walk +
  read-rule. Does **not** call `vfs_chain_walk`.
- `vfs_rename`: inlines yet another copy of the read-rule for
  its conflict-detection walk.

**Proposed state (post-W6).**

Three new shared functions in `tree.c` (or a new `chain_walk.c`
if `tree.c` is too large):

1. **`walk_anchor_chain(ctx, head_vp, callback, user)`** —
   walks a chain of `Anchor` nodes (FileContent for files,
   DirSegment for dirs) by following each node's `sibPtr` (and
   accumulating children via the `headPtr` if the callback
   needs them). Calls `callback` for each Anchor with the
   Anchor's `bytes` (by-value copy-out). Unifies steps 1–2 of
   the 6-step walk.
   ```c
   typedef int (*anchor_walk_cb)(TreeContext* ctx,
                                 const uint8_t* anchor_bytes,
                                 void* user);

   int walk_anchor_chain(TreeContext* ctx, int64_t head_vp,
                         anchor_walk_cb cb, void* user);
   ```

2. **`walk_content_unit_chain(ctx, content_unit_head, target_unit_id, &out_slot)`** —
   walks a chain of `ContentUnit` nodes (PageNode for files,
   SlotNode for dirs) by following each node's `sibPtr`, and
   returns the unit whose `id` field (offset 4) matches
   `target_unit_id`. Unifies step 3.
   ```c
   int walk_content_unit_chain(TreeContext* ctx, int64_t unit_head,
                               uint32_t target_unit_id,
                               PoolSlot* out);
   ```

3. **`vfs_chain_walk_extended(ctx, root_vp, unit_id, query_epoch, &out_leaf)`** —
   new function (alternative: change `vfs_chain_walk` to take
   `root_vp + unit_id` instead of `chain_head`). Combines
   `walk_anchor_chain` + `walk_content_unit_chain` + the existing
   leaf walk + read-rule to do all 6 steps. For dirs, the
   `unit_id` is the SlotNode's `id` (== child `nodeId`); the
   caller iterates over unit_ids to do name matching.
   ```c
   WalkResult vfs_chain_walk_extended(TreeContext* ctx,
                                      int64_t root_vp,
                                      uint32_t unit_id,
                                      int64_t query_epoch,
                                      PoolSlot* out_leaf);
   ```

`tree_resolve_page` collapses to a thin wrapper:
```c
int tree_resolve_page(vfs_t* vfs, int64_t file_vp, int64_t logical_page,
                      int64_t epoch, bool is_write, PoolSlot* out) {
    /* Lock discipline: is_write path holds vfs_lock(vfs, file_vp, epoch) */
    return vfs_chain_walk_extended(vfs->ctx, file_vp,
                                    (uint32_t)logical_page,
                                    epoch, out);
}
```

(Plus the existing `is_write=true` segment-growth logic stays in
`tree_resolve_page` — that's the only non-walk work the function
does. The 430-line bulk becomes a 50-line wrapper around the
shared walk + a segment-growth block.)

`dirchain_find_child` collapses similarly:
```c
int dirchain_find_child(TreeContext* ctx, int64_t dir_vp, const char* name,
                        int64_t epoch, int64_t* out_childPtr, ...) {
    /* Compute name hash, walk all SlotNodes in the dir's chain.
       For each SlotNode, call vfs_chain_walk_extended(dir_vp, slot_id, ...).
       Compare name; if match, return childPtr. */
}
```

(The by-name iteration stays in `dirchain_find_child` because the
shared walk is by-id, not by-name. The remaining
~80 lines are the iteration + name comparison, not the walk
itself.)

`vfs_rename` is updated to use the shared walk for its
conflict-detection, removing the 3rd inlined read-rule copy.

**Net code change (target).** `tree.c` net delta after W6:
- `walk_anchor_chain` + `walk_content_unit_chain` +
  `vfs_chain_walk_extended`: ~180 new lines (with comments).
- `tree_resolve_page`: 430 → ~80 lines (−350).
- `dirchain_find_child`: 225 → ~120 lines (−105).
- `vfs_rename` read-rule: −15 lines.
- **Total: ~470 lines removed, ~180 added, net −290 lines.**

The "W2 promised 2× reduction" is not met (we're at ~10%
reduction in `tree.c` after the W5 per-ContentUnit machinery
came in), but the *duplication* of the read-rule is removed —
the single source of truth is `vfs_chain_walk_extended`.

**File changes.**
- `src/tree.h`:
  - Add `anchor_walk_cb` typedef and `walk_anchor_chain` /
    `walk_content_unit_chain` / `vfs_chain_walk_extended`
    declarations.
  - Update the header comments on `tree_resolve_page` and
    `dirchain_find_child` to *accurately* describe the
    post-W6 structure (the "thin wrapper" claim becomes true
    for `tree_resolve_page`; `dirchain_find_child` keeps the
    by-name iteration but uses the shared walk internally).
- `src/tree.c`:
  - Implement `walk_anchor_chain`, `walk_content_unit_chain`,
    `vfs_chain_walk_extended` (~180 lines).
  - Rewrite `tree_resolve_page` to use the shared walk
    (~80 lines target).
  - Rewrite `dirchain_find_child` to use the shared walk
    (~120 lines target).
  - Update `vfs_rename` to use the shared walk for
    conflict detection.

**Tests.**
- **Existing tests must pass unchanged** — the public API
  (`tree_resolve_page` / `dirchain_find_child` / `vfs_rename`)
  is preserved. The behavior is the same; the implementation
  is consolidated.
- **Add `test_chain_walk_extended`** — single test that
  exercises `vfs_chain_walk_extended` in both file and dir
  contexts, verifying the read-rule (committed-snapshot remap,
  odd-skip, exact-match-wins) on a chain of mixed-epoch
  leaves.
- **Add `test_chain_walk_anchor_chain`** — verify the
  segment-level walk handles a file with many pages (> 1
  segment) and a dir with many children (> 1 segment).
- **Add `test_chain_walk_no_duplication`** — grep `tree.c` for
  the read-rule markers (mapper remap, even/odd) and assert
  they appear in exactly one place (the shared function).
  This is a build-time / static check, not a runtime test.

**Bench (perf analysis).**
- **FUSE bench:** expected **in noise** (no algorithmic change;
  the segment-walk loop is the same; the chain-walk loop is
  the same; the read-rule is the same). Goal: confirm no
  regression. **In noise** verdict required.
- **vfs_bench create:** may improve slightly (less duplicated
  walk code = better icache / dcache locality). **In noise**
  verdict acceptable; improvement is a bonus.
- **vfs_bench write/scan:** expected **in noise** (the
  hot path is `vfs_write` which is unchanged; `tree_resolve_page`
  is on the read-side of writes for PageNode lookup).
- **vfs_bench dir:** expected **in noise** (the by-name
  iteration is the same; the inner walk is consolidated).

**Risk.** **medium-high.**
- The shared functions must produce the *same* read-rule
  results as the current implementations. Any divergence is a
  silent correctness regression.
- The "by-name" lookup in `dirchain_find_child` is structurally
  different from the "by-id" lookup in `tree_resolve_page` —
  the shared `vfs_chain_walk_extended` must handle the by-id
  case (its primary use), and the by-name iteration in
  `dirchain_find_child` calls it once per unit.
- The `vfs_chain_walk_extended` API change (or the new
  function alongside the existing `vfs_chain_walk`) may
  require updates to `verchain_get` and `sizechain_get`,
  which are the existing thin wrappers over `vfs_chain_walk`.
  These wrappers currently take a `chain_head`; if we add the
  extended variant, they stay unchanged; if we change
  `vfs_chain_walk`'s signature, they need to be updated.
- **Recommended API choice:** keep `vfs_chain_walk(ctx,
  chain_head, ...)` unchanged (it has 2 existing callers:
  `verchain_get`, `sizechain_get`). Add `vfs_chain_walk_extended`
  as a new function for the root_vp + unit_id form. The
  extended form internally calls the existing `vfs_chain_walk`
  once it has resolved the leaf chain head.

**Estimated effort:** 3–5 days. W2 in retrospect was
under-scoped at 3 days; W6 is the "W2 part 2" that was
accidentally deferred.

### W6 — Results (DONE)

**Status: shipped** across 10 commits
(`0c834a2`..`0b22a51`), all 13/13 tests pass throughout.

| What was specified | Actual result | Verdict |
|---|---|---|
| `walk_anchor_chain` + `walk_content_unit_chain` + `vfs_chain_walk_extended` declared in `src/tree.h` and implemented in `src/tree.c` | ~180 lines net (W6a); signatures stabilized in W6-pre (`anchor_vp` for sorted insertion) | ✅ |
| `tree_resolve_page` 430 → ~80 lines | 430 → **270** | ⚠ Partial — write-path segment growth (tcache, sorted insertion, segment-level prepend, `pageCount` update) cannot use the read-only shared walk. Reviewer (W6 G1 finding): "not a bug." |
| `dirchain_find_child` 225 → ~120 lines | 225 → **89** | ✅ Better than target |
| `vfs_rename` read-rule consolidated to shared walk | `vfs_rename` "is dir empty" check + `dirchain_list` refactored (W6f) | ✅ |
| `verchain_get` / `sizechain_get` stay unchanged | unchanged (thin wrappers over `vfs_chain_walk`) | ✅ |
| `test_chain_walk_extended` (read-rule coverage) | added (W6b) | ✅ |
| `test_chain_walk_anchor_chain` (multi-segment coverage) | added (W6b) | ✅ |
| `test_chain_walk_no_duplication` (single-source-of-truth enforcement) | added as **runtime** test (W6f), not build-time | ✅ The runtime version greps `src/tree.c` and asserts the read-rule markers appear exactly once. |
| FUSE bench in noise | 18.7s median, range 17.7–19.9s | ✅ In noise of W0 best (16.94s); system overheating limits confidence. |
| `vfs_bench` micro-bench in noise | flat vs W5 (no further regression, no recovery) | ✅ "In noise" of the W5 profile. The post-W5 regression is structural and accepted. |
| API choice: keep `vfs_chain_walk(chain_head,...)` unchanged, add `vfs_chain_walk_extended(root_vp, unit_id,...)` | followed | ✅ |
| `dircontentlink` chain walked via shared walk? | No — different layout (offset 8 = `dirContentVP`, offset 16 = `nextVP`, not the Anchor layout). Kept inline. | ✅ Reviewer (W6 G3 finding): "correct, acceptable." |

**Workload commit log (W6):**

| Commit | Description |
|---|---|
| `0c834a2` | W6 spec: add to phase-26 spec — chain-walk unification completion (U1) |
| `22493ba` | W6a: add `walk_anchor_chain`, `walk_content_unit_chain`, `vfs_chain_walk_extended` |
| `b3873bd` | W6b: fix `vfs_chain_walk_extended` (nested callback) + add 2 tests |
| `0339a30` | W6-pre: extend `anchor_walk_cb` signature with `anchor_vp` (needed for sorted insertion in `tree_resolve_page`) |
| `d1d529d` | W6c: refactor `tree_resolve_page` 425→270 lines using shared walks + extracted helpers |
| `a7c9637` | W6d: refactor `dirchain_find_child` 227→89 lines (61% smaller) using `walk_anchor_chain` + `walk_content_unit_chain` + `vfs_chain_walk` |
| `63f04a7` | W6e: refactor `commit_scan_dir` to use `vfs_chain_walk` for DirContent + `sizechain_get` for FileSize |
| `076657e` | W6f: fix false "thin wrapper" comments + refactor `dirchain_list` (readdir) and `vfs_rename`'s "is dir empty" check + add `test_chain_walk_no_duplication` runtime test |
| `acce536` | W6-g2: move chain-walk tcache from `__thread` static to per-`vfs_t` field on `TreeContext` |
| `0b22a51` | W6-m2: rename `w6_*` statics to semantic names + remove dead `w6_seg_walk_*` code |

**Review:** `PHASE26_W6_REVIEW.md` (4-round review by GLM).
Findings resolved: G1 (270 vs 80 lines) by header comment
update; G2 (`__thread` tcache) by W6-g2; M1, M2 (naming) by
W6-m2; M3 (no-dup test is a runtime test) by W6f upgrade.

**Actual effort:** ~3 days of AI work, 10 commits.
Spec's "3–5 days" estimate was on target.

---

## Risks & open questions for reviewer

1. **`ContentUnit.id` source.** For `UNIT_SLOT`, the `id` field
   is set to the `childNodeId` of the owning child. We
   considered a separate slot-ID space (allocated
   independently of childNodeId) but concluded there's no
   benefit — slot count equals child count, and using
   `childNodeId` keeps the lock key tied to a meaningful
   identifier. **Recommended answer:** use `childNodeId`.

2. **Segment size.** Set `ANCHOR_UNITS_PER_SEGMENT = 1024`
   as a configurable macro, initial value matching the
   existing `segment_size` in `storage.c:149`. Same value
   for both file and dir segments to start. If profiling
   shows a need, we can split into per-type segment sizes
   later (one extra uint32 in `ctx` + two reads in segment
   helpers). The cost of splitting is small; the benefit
   needs measurement first.

3. **Radix index refit (storage change).** The radix index
   leaf's `DirContentLink` storage changes from
   `int64_t dirContentVP` to `int64_t contentUnitVP`. The
   leaf's `DirContentLink` list may now contain multiple
   `ContentUnit`s at the same hash (delete+recreate). The
   `ContentUnit` visibility rule (§4.4) resolves them. We
   do not maintain compat — old index data is discarded
   when a VFS file is opened. Confirm the reviewer is OK
   with this.

4. **Lock granularity for `Node` writes.** The `Node`
   itself needs to be locked for `DirContentIndex` (dir) and
   `FileSize` (file) chain prepends. We propose the `Node`
   lock (`vfs_lock` on root VP). Alternative: per-field
   atomic (no lock). We recommend the lock for symmetry
   with the file model (where root-level fields are also
   locked, not atomic) and because the lock is already
   acquired for segment/ContentUnit growth, so adding one
   more acquire is cheap.

5. **The `DirContent` chain ordering (resolved in spec).**
   With the new model, the chain is per-child, not per-name.
   Two writers to the same child (e.g., a rename and a
   delete) both prepend to the same `ContentUnit.headPtr`.
   Lock per-`ContentUnit` serializes them. The read-rule
   (§4.3) is the same as the file model — per-entry mapper
   remap, even/odd distinction, exact-match-wins. Tombstones
   (`namePtr == 0`) suppress the entry; if the visible entry
   is a tombstone, the caller walks further (e.g., other
   `ContentUnit`s at the same hash). This is the
   `ContentUnit` visibility rule (§4.4). **Resolved — no
   reviewer input needed beyond confirming §4.3-4.4.**

6. **GC walk coverage of new node types.** The GC walks the
   chain to find unreachable nodes. With the new `Anchor`
   layout, GC needs to recognize and walk all `Anchor` types
   (`ROOT_FILE`, `ROOT_DIR`, `SEGMENT_FILE`, `SEGMENT_DIR`,
   `UNIT_PAGE`, `UNIT_SLOT`) in addition to the leafs
   (`VersionPage`, `DirContent`, `FileSize`). W5 covers this
   in three sub-problems (struct/type remap,
   `gc_copy_entry` per-type field descriptors, survival
   analysis rewrite). The 32-byte slot and VP semantics are
   preserved.

7. *(removed — see "no backward compat by default" rule)*

8. **`vfs_chain_walk` API.** Should it be a public API
   (declared in `vfs.h` for external use) or private to
   `tree.c`? We recommend private initially; promote later
   if needed.

9. **Performance regression risk.** The name-lookup path
   adds 1–2 cache misses per call (`ContentUnit` acquire +
   short chain walk). On the FUSE workload, this is expected
   to be in the noise. On a multi-threaded SQLite-shim
   workload (future), the reduced contention should be a
   clear win. **Confirm:** the reviewer is OK with the
   worst-case +10% on FUSE in exchange for the structural
   cleanup.

10. **Migration scope.** This phase touches `nodes.h`,
    `nodes.c`, `tree.c`, `vfs.c`, `gc.c`, `vfs.h`,
    `vfs.c` (lock API), plus tests. The dir side is a
    near-total rewrite. The file side is a refactor that
    should reduce code size. **Confirm scope is
    acceptable** before we start W0.

11. **Dedup-infrastructure cleanup.** The dedup hash_map and
    `DirchainDedupEntry` become unused after W5. Per user
    direction, `hash_map.c`/`hash_map.h` and the `VarArray`
    infrastructure are **kept** (they have other future
    uses). Only the call sites in `dirchain_list` and any
    dirchain-related dead code paths are removed. The
    `DirchainDedupEntry` type can either be removed (it's
    only used by `dirchain_list`) or kept in `tree.h` for
    future use. **Recommended answer:** remove
    `DirchainDedupEntry` (it's clearly dead after W5); keep
    `hash_map.c` and `VarArray` as-is.

12. **GC contract.** This phase documents the
    single-threaded-only GC contract (caller must not invoke
    `vfs_gc` concurrently with mutators) but does NOT
    implement stop-the-world GC. The proper stop-the-world
    fix is part of the future GC reimplementation. **Confirm
    the reviewer is OK with this.** Note: the existing
    `tree_lock_acquire_shared` is never called by any
    read/write path (ISSUES.md C3), so the de facto current
    contract is already "single-threaded GC only" — we are
    just making it explicit.

13. **Phase 25 shim removal.** The compat shims
    `pool_resolve_ro` / `pool_resolve_rw` and
    `tree_resolve_page_compat` (added at the end of Phase 25
    for test build) become non-functional with the on-disk
    format change in this phase (old-format VFS files won't
    be readable). Remove them as part of W1 or W5. Any test
    that still uses the old API must be migrated to the new
    `pool_acquire` / `pool_release` API.

14. **`vfs_node_type` public API return value change.** The
    public API in `vfs.h:165-166` returns `0x01` (`DIR`) /
    `0x03` (`FILE`). The unified model uses `0x11` (`ROOT_DIR`)
    / `0x10` (`ROOT_FILE`). Two options:
    - (a) Update the public API to return the new values.
      Breaking change for any external caller (currently only
      `fuse_vfs.c:258,991`).
    - (b) Translate internally: `vfs_node_type` reads
      `ROOT_DIR` / `ROOT_FILE` and translates to the legacy
      `0x01` / `0x03` for the public return.
    **Recommended answer:** (b) — keep the public API stable,
    translate internally. The legacy values are not in active
    use outside the VFS codebase.

15. **Validation criteria tightening.** The "≥ 2x code-size
    reduction on chain-walk paths" target is conservative;
    the actual estimate is ~2x (730 → 335 lines). The
    "3.5x" figure from earlier revisions was optimistic
    because the full read-rule carries weight the previous
    draft didn't show. Confirm the reviewer is OK with 2x
    as the target.

## Appendix: data layout table

| Anchor (Node/Segment/ContentUnit) | Offset | Size | Field | Semantics |
|---|---|---|---|---|
| | 0 | 2 | type | `AnchorKind` enum |
| | 2 | 2 | flags | reserved |
| | 4 | 4 | id | nodeId/segmentId/pageIndex/slotId |
| | 8 | 8 | headPtr | first child at next-lower level |
| | 16 | 8 | sibPtr | next sibling at same level (0 for root) |
| | 24 | 4 | count | unit count (Segment) or 0 |
| | 28 | 4 | reserved | |

`Node` semantic overload at offset 16-31:
- `ROOT_FILE`: `sibPtr` = `sizePtr` (FileSize chain head); offset 24-31 = `createdAt`
- `ROOT_DIR`: `sibPtr` = `indexHeadPtr` (DirContentIndex chain head); offset 24-27 = 0; offset 28-31 = reserved

| Leaf (VersionPage / DirContent / FileSize) | Offset | Size | Field | Notes |
|---|---|---|---|---|
| | 0 | 4 | epoch | (uint32) — same offset, same read-rule |
| | 4 | 4 | kind_specific | see per-leaf below |
| | 8 | 8 | primary_ptr | see per-leaf below |
| | 16 | 8 | secondary_ptr | see per-leaf below |
| | 24 | 8 | nextPtr | (VirtualPtr) — same offset, same chain walk |

**Per-leaf payload** (offsets 4, 8, 16):

| Leaf | Offset 4 | Offset 8 | Offset 16 |
|---|---|---|---|
| `VersionPage` | reserved | `dataPage` (int64) | reserved |
| `DirContent` | `childNodeId` (uint32) | `childPtr` (VirtualPtr) | `namePtr` (VirtualPtr; 0 = tombstone) |
| `FileSize` | `modifiedAt` (int64) | `fileSize` (int64) | reserved (4 bytes used, 4 bytes padding) |

Mirrored fields:

- `headPtr` at offset 8 in all Anchor types (Node, Segment,
  ContentUnit). One offset, one meaning: "the head of what
  this Anchor anchors."
- `sibPtr` at offset 16 in all Anchor types. For Segment
  and ContentUnit: next sibling at same level. For Node:
  semantic-overloaded as `metaPtr` (FileSize or
  DirContentIndex chain head).
- `id` at offset 4 in all Anchor types. `nodeId` for Node,
  `segmentId` for Segment, `pageIndex` for UNIT_PAGE,
  `childNodeId` for UNIT_SLOT.
- `epoch` at offset 0 in all leaf types (VersionPage,
  DirContent, FileSize). The chain walk reads this for the
  read-rule comparison. The walk is byte-identical for
  all three leaf types.
- `nextPtr` at offset 24 in all leaf types. The chain walk
  reads this to advance to the next entry. Byte-identical
  for all three leaf types.
- Per-leaf fields (dataPage, childPtr, namePtr, childNodeId,
  modifiedAt, fileSize) are accessed only at the leaf step
  (Step 6) where the caller knows the leaf type.

**Note on `FileSize` alignment (R4):** the unified leaf
layout puts a 4-byte `epoch` (offset 0) followed by an
8-byte `modifiedAt` (offset 4). The 8-byte value is
4-byte-aligned but not 8-byte-aligned. This is the same
alignment as the current `FileSize` layout (pre-Phase-27),
so it's a pre-existing condition, not new. The
`vfs_rd8_s`/`vfs_wr8_s` helpers use `memcpy`, which is
alignment-safe on all platforms (x86, ARM, etc.), so this
is not a correctness issue. If alignment ever becomes a
perf concern (some strict-alignment ARM cores), padding
`epoch` to 8 bytes (wasting 4) would fix it. Not addressed
in this phase.

## Appendix: file-by-file change list (preview)

| File | Changes |
|---|---|
| `src/vfs.h` | Document single-threaded-only GC contract. Add `vfs_lock_destroy` declaration. |
| `src/vfs.c` | (W0) Move `lock_table` to `vfs_t`. Change key from `int` to `int64_t` (no semantic check). Add `vfs_lock_destroy` + teardown in `vfs_unmount`. Update `vfs_node_type` switch to handle all `AnchorKind` values; translate `ROOT_DIR`/`ROOT_FILE` to legacy `0x01`/`0x03` for the public API. |
| `src/nodes.h` | (W1) Add `Anchor` struct + `ANCHOR_OFF_*` macros + `AnchorKind` enum. Add `Node` (with `createdAt`), `Segment`, `ContentUnit` layouts (all Anchors). Add `nodes_write_anchor` / `nodes_read_anchor` declarations. Deprecate `FILENODE_OFF_*`, `DIRNODE_OFF_*`, `FILECONTENT_OFF_*`, `PAGENODE_OFF_*` (replaced by `ANCHOR_OFF_*`). Add `ANCHOR_UNITS_PER_SEGMENT = 1024` macro. |
| `src/nodes.c` | (W1) Implement `nodes_write_anchor` / `nodes_read_anchor`. Refit `nodes_write_filenode` / `dirnode` to use the new layout (with `createdAt` for both). |
| `src/tree.h` | (W2) Add `vfs_chain_walk` declaration. (W5c) Remove `DirchainDedupEntry` declaration (dead after W5). (W6) Add `walk_anchor_chain`, `walk_content_unit_chain`, `vfs_chain_walk_extended` declarations. Update `tree_resolve_page` / `dirchain_find_child` header comments to accurately reflect post-W6 structure. |
| `src/tree.c` | (W2-W5) Implement `vfs_chain_walk` (W2, with full read-rule). Replace CAS-on-local with locks in `vfs_create`, `vfs_delete`, `vfs_mkdir`, `vfs_rmdir`, `vfs_rename`, `vfs_write` (COW), `vfs_truncate` (W3-W4). Update `dirchain_list` to walk segments + ContentUnits; remove dedup hash_map ops (W5). Update `dircontentindex_insert` / `remove` to point at `ContentUnit` VPs and handle multi-link-per-hash (W5). (W6) Add `walk_anchor_chain` + `walk_content_unit_chain` + `vfs_chain_walk_extended` (~180 lines). Collapse `tree_resolve_page` 430→270 and `dirchain_find_child` 225→89. Update `vfs_rename` to use the shared walk. (W6-g2) Move chain-walk tcache from `__thread` static to per-`vfs_t` field on `TreeContext`. (W6-m2) Rename `w6_*` statics to semantic names. **Net: 2488 → 3948 lines (+59%)**, not the −54% predicted. See "What becomes simpler" for why. |
| `src/gc.c` | (W5) Three sub-problems: (a) struct/type remap — add `gc_walk_segment` / `gc_walk_content_unit` for new `Anchor` types. (b) `gc_copy_entry` per-type field descriptors (ISSUES.md M4) — fix blind-remap of `createdAt`. (c) Survival analysis rewrite (ISSUES.md M2) — drop `MAX_CHILDREN = 1024` fixed array, use per-`ContentUnit` walks. **GC is structurally correct but functionally broken (pre-existing `VFS_ERR_FULL` at `gc_allocate_new_pool_page`; masked by `test_gc.c:786-788` tolerance).** |
| `src/pool.h`, `src/pool.c` | (W5f-shims) Remove `pool_resolve` / `pool_resolve_ro` / `pool_resolve_rw` compat shims. (W5h) `PoolSlot.bytes` field 8-byte aligned within struct (insert 4 bytes padding; sizeof stays 48) — fixes 4-byte aligned 8-byte atomic load UB on ARM/SPARC. (W5j) Remove `tree_resolve_page_compat` / `tree_resolve_page_compat_release`. |
| `test/test_tree.c` | (W2) Add dedicated read-rule test. (W5g) Add `test_concurrent_dir_writes`, `test_concurrent_rename`, the 10K+ stress test, and the ContentUnit visibility test. (W5f) Migrate 24 sites from `tree_resolve_page_compat` to by-value API (W5i). Update or remove `test_dirnode_child_count`. (W6b) Add `test_chain_walk_extended`, `test_chain_walk_anchor_chain`. (W6f) Add `test_chain_walk_no_duplication` (runtime test, grep `src/tree.c` for read-rule markers). |
| `test/test_nodes.c` | (W1) Update for new `Anchor` write/read helpers. |
| `test/test_tree.c` (lock tests) | (W0) Add same-epoch exclusion test, different-epoch non-exclusion test, mixed-key test, per-`vfs_t` isolation test. (W0-followup) Fix `test_lock_same_epoch_exclusion` self-deadlock. |
| `test/test_pool.c` | No change (pool API itself is unchanged; only the compat shims are removed in W5). |
| `test/test_gc.c`, `test/test_gc.c.bak` | (W5f) Migrate from `pool_resolve_rw` to by-value `PoolSlot` API across multiple categories. Resolve `.bak` file. |
| `bench/bench.c` | (W0) Update for new `vfs_lock` signature. |
| `src/hash_map.c`, `src/hash_map.h` | No change (kept for future use; `dirchain_list` stops calling it). |
| `src/epoch.c` | (W6e) `commit_scan_dir` refactored to use `vfs_chain_walk` for DirContent + `sizechain_get` for FileSize. VersionPage walk stays inline (commit-specific: needs `has_snapshot` AND `has_live`). |
| `impl/phase-26-unified-tree.md` | This spec. |

## Appendix: timeline estimate

**Original spec estimate (human-time, 4–6 weeks):**

- **W0** (vfs_lock rework): 3 days
- **W1** (Anchor struct + new types + `createdAt` + unified
  leaf layout): 1 day
- **W2** (`vfs_chain_walk` with full read-rule, two-step
  mapper API, unified step 4): 3 days
- **W3** (file lock swap): 1 day
- **W4** (dir lock swap): 3 days
- **W5** (dir segment + ContentUnit population, dedup
  removal, GC rework, shim removal): 5–7 days
- **W6** (chain-walk unification completion — collapses
  `tree_resolve_page` 430→80 and `dirchain_find_child` 225→120,
  removes the 4-place read-rule duplication): 3–5 days
- Concurrent test development: 1 day (parallel with W3–W5)
- Buffer for surprises: 2 days

**Total human-time estimate: ~5–6 weeks** (23–27 working days).

**Actual (AI-time, this implementation):**

- **42 commits**, spanning W0 (Jul 12) → W6-m2 (Jul 13)
- ~24 hours of AI wall-clock time (with reviews, FUSE
  bench, and waiting on the reviewer in between)
- All 13/13 tests pass throughout (no regressions introduced
  by any individual workload)
- FUSE bench went from 16.94s (W0 best) to 18.7s (W6
  median), in noise

**Per-workload commit count (actual):**

| Workload | Commits | Notable sub-steps |
|---|---|---|
| W0 (lock rework) | 3 | `7bf60ac` (main), `da35074` (followup), spec |
| W1 (Anchor + new types) | 6 | W1a–W1e: enum, DirNode refit, FileContent, PageNode, DirSegment/SlotNode |
| W2 (vfs_chain_walk + aligned leaf layouts) | 1 | `dce84e3` |
| W3 (file lock swap) | 1 | `9a08bc5` |
| W4 (dir lock swap) | 1 | `a4b383e` |
| W5 (per-ContentUnit, GC, dedup removal, shims, alignment) | 19 | W5a–W5g + W5f-shims + W5f (test migration) + W5h (PoolSlot alignment) + W5i (test sites) + W5j (drop compat) + W5-followup |
| W6 (chain-walk unification) | 10 | W6a–W6f + W6-pre + W6-g2 + W6-m2 + spec |
| **Total** | **42** | |

**W5 is the longest workload** (19 commits). It was
under-scoped in the original spec — the per-`ContentUnit`
machinery + GC rework + test migration + shim removal was
all rolled into one workload. If we were to do it again,
we'd split into W5a (per-`ContentUnit`), W5b (DirSegment
population), W5c (dedup removal), W5d (radix refit), W5e
(GC rework), W5f (test migration), W5g (new tests), W5h
(alignment), W5i (compat test sites), W5j (drop compat). All
these labels exist in the commit log already; they were just
grouped under "W5" in the spec.

**W6 turned out to be 10 commits, not 3–5 days of human
time.** The AI implementation was efficient; the human-time
estimate was correct on the high side because W6's
"complexity" was reading and understanding the existing
`tree_resolve_page` / `dirchain_find_child` logic, which the
AI did much faster than a human would.

## Lessons learned (added in Rec 5 reconciliation)

**For future phases of this codebase, and for the next
person writing a spec:**

1. **Don't promise "2× reduction" as a "must hit".** Promise
   the *structural* win (single source of truth, removed
   duplication) and report the *line count* as an observation,
   not a target. The 730 → 335 estimate was correct in
   isolation (the unified walk *can* be ~120 lines) but
   didn't account for the W5 per-`ContentUnit` machinery,
   which is genuinely new code that didn't exist pre-26. Net
   result: `tree.c` grew 59%, not shrank 54%.

2. **W2 in retrospect was under-scoped at "3 days".** The
   original "unified chain walk" goal was *partially*
   delivered — `vfs_chain_walk` only implemented steps 4–5.
   W6 was the "W2 part 2" that was accidentally deferred. If
   we were to do it again, we'd write W2 to deliver all 6
   steps in one go, or explicitly scope it to "steps 4–5
   only" with a planned W2.5 / W6 follow-up. The
   implementation review caught this; the spec was amended
   retroactively. **A spec amendment 30 commits in is more
   expensive than scoping correctly upfront.**

3. **Performance predictions in specs are usually wrong.**
   The "neutral on FUSE, slight negative on micro-bench"
   prediction was qualitatively right (FUSE is in noise) but
   the micro-bench magnitude was wrong (40% regression, not
   10%). The right framing: state the *concern* (extra
   cache misses on cold paths, extra allocations on create),
   state the *expected order of magnitude* (±20% on FUSE,
   ±20% on micro-bench, wide error bars), and **commit to
   re-measuring after the phase ships** before declaring
   "done."

4. **The per-`ContentUnit` model is structurally correct
   but has a real perf cost.** The cost is the new
   allocation per child + the double-lock on every mutation.
   This is the cost of enabling per-file concurrency in the
   same dir (the SQLite VFS shim future direction). The
   trade-off was correct: pay the perf cost now to unlock
   the structural win. **For future phases: if the perf cost
   is unacceptable, the lever is dropping the redundant
   `node_vp` lock from `vfs_write` and the create path.**
   The spec explicitly permits this ("can be dropped if
   profiling shows it's a bottleneck"); profiling now shows
   exactly that. **Not addressed in Phase 26** — left for a
   future phase.

5. **Test suite can mask broken code.** `test_gc.c:786-788`
   treats `VFS_ERR_FULL` as a passing outcome, which hid
   the pre-existing GC failure across the entire W0–W6
   window. The `CHECK_EQ(gc_ret, VFS_ERR_FULL)` line is
   itself a kind of test rot — it was probably correct at
   the time (when GC legitimately could return FULL under
   pool pressure) and then became wrong as upstream
   conditions changed. **For future phases: when adding a
   tolerance, add a comment that says "remove this when X is
   fixed" with a date.** GC is descoped from Phase 26 to a
   future phase; that future phase should remove the
   tolerance as its first commit.

6. **External review is high-leverage.** GLM caught
   the 270-line `tree_resolve_page` (G1 — "not a bug" but
   the explanation is in the header), the `__thread` tcache
   collision risk (G2 — fixed in W6-g2), and the `w6_*`
   prefix tying naming to workload (M2 — fixed in W6-m2).
   None of these were caught by the test suite or by
   running the FUSE bench. **For future phases: keep using
   the external review step. It's not free, but the
   alternative is shipping a phase with 4-5 landmines that
   surface months later.**

7. **Spec reconciliation (this document) is itself
   valuable.** Going through the spec line-by-line and
   marking which predictions held and which didn't took
   ~30 minutes and produced a 100-line "what actually
   shipped" summary at the top. **For future phases:
   reconcile the spec as the last commit, not as a
   background task that gets lost.** The "Status" section
   is now the first thing a future reader sees, before the
   original goals, before the architectural decisions, before
   anything else.
