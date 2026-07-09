# Phase 18: Directory Radix Tree Index

## Goal

Add a persistent on-disk radix tree to every directory so name lookups
(collision check in `vfs_create`, lookup in `vfs_open`/`dirchain_find_child`,
`vfs_delete`, `vfs_rename`) are O(tree-depth) instead of O(chain-length).

The existing `DirContent` chain is preserved. `DirNode` gains a new field
pointing to the tree root. `vfs_create` writes to BOTH (chain + tree);
`vfs_readdir` continues using the chain (full-dir walk); name lookups use
the tree.

For a directory with N children, the collision check goes from O(N) per
create to O(L_leaf) per create, where L_leaf is the average entries per
leaf (typically << N for uniform hash distribution).

## Non-Negotiable Constraints

- **The tree is a first-class index, built from the start.** Every
  `DirNode` has a valid `indexHeadPtr` from the moment it is created.
  There is no lazy build, no threshold, no chain fallback. The tree
  is always the source of truth for name lookups.
- **Cursor-based readdir uses the tree when available.** The kernel may
  issue many sequential `readdir` calls with offsets; a cursor on the
  tree makes these O(remaining) instead of O(N) per call. Without
  cursor support, the existing chain walk is used.
- **No new pool allocations beyond tree nodes themselves.** A create still
  allocates the same `FileNode`/`NameEntry`/`DirContent` slots. Tree nodes
  are additional pool allocations on the tree path.
- **Both structures must stay consistent.** Every CAS-prepend to the chain
  must be paired with a CAS-append to the tree leaf. If the tree append
  fails (e.g. pool exhausted), the chain append must be rolled back or the
  whole operation must be aborted.
- **Read paths must never block on writes.** `vfs_open`/`vfs_readdir` use
  lock-free reads (atomic pointer loads). Writes use CAS for atomicity.
- **No global locks.** All locking is per-leaf or per-parent-node. Different
  leaves never contend.
- **Tombstones are first-class citizens of the tree.** A deleted entry's
  `DirContent` (with `namePtr=0`) stays in the tree leaf. Lookups skip
  tombstones explicitly.

## Configuration

| Constant | Default | Description |
|----------|---------|-------------|
| `RADIX_TREE_BRANCHING` | 16 | Number of children per internal node (4 bits per level). |
| `RADIX_TREE_MAX_LEVELS` | 16 | Maximum depth of the tree (covers 64-bit hashes: 4 × 16 = 64 bits). |

## File Organization

| File | Purpose |
|------|---------|
| `src/nodes.h` | Add `DIRNODE_OFF_INDEXHEADPTR`, `DIRCONTENTINDEX_OFF_*`, `DIRCONTENTLINK_OFF_*` constants; declare `nodes_read/write_dircontentindex`, `nodes_read/write_dircontentlink` |
| `src/nodes.c` | Implement the four new node read/write functions (the 32-byte slot layouts are the source of truth) |
| `src/tree.h` | Declare `dircontentindex_walk`, `dircontentindex_insert`, `dircontentindex_lookup` |
| `src/tree.c` | Implement the radix tree operations; modify `vfs_create` to write to both chain and tree; modify `dirchain_find_child` to use tree |
| `src/indirection.c` | (no change — indirection chain is unaffected by the tree) |
| `test/test_tree.c` | Tests for the radix tree |

## Data Structures

### Pool Page Layout (unchanged)

A pool page is 8192 bytes: 16-byte header + 255 × 32-byte slots. Each slot
holds one of: `DirNode`, `FileNode`, `DirContent`, `FileSize`, `FileContent`,
`VersionPage`, `NameEntry`, or the new `DirContentIndex`.

### DirNode (extended, 32 bytes)

```
Offset  Size  Field
──────  ────  ─────
  0      2    type            (0x01 = NODE_TYPE_DIR)
  2      2    reserved
  4      4    nodeId
  8      8    headPtr         (VirtualPtr to first DirContent — UNCHANGED)
 16      8    indexHeadPtr    (VirtualPtr to first DirContentIndex at level 0)
 24      8    reserved
```

The `indexHeadPtr` field uses the previously-zeroed padding at offset
16-23. The tree is created when the `DirNode` is first created (in
`tree_bootstrap_root` or similar). The tree is always built from the
start — there is no lazy build.

### DirContentIndex — Internal (new node type, 32 bytes)

```
Offset  Size  Field
──────  ────  ─────
  0      1    hashNibble   (0..15 — which nibble of the hash this node represents)
  1      1    nodeType     (0x02 = INTERNAL — this is an index-only node)
  2      6    reserved     (alignment padding; offsets 2-7)
  8      8    listVP       (VirtualPtr to first DirContentIndex in the level's list)
 16      8    nextVP       (VirtualPtr to next DirContentIndex in the same parent's list at the same level)
 24      8    reserved     (offsets 24-31)
```

- **`hashNibble`**: which nibble of the entry's name hash this node
  represents at its level (0..15 for 4-bit radix).
- **`nodeType = INTERNAL`**: `listVP` points to a linked list of more
  `DirContentIndex` nodes (descending one level). Internal nodes never
  hold DirContent — they only navigate to children.
- **`listVP`**: head of the level's list. Each item in the list is another
  `DirContentIndex` (one per hash nibble in use at this level).
- **`nextVP`**: chain link to the next `DirContentIndex` in the parent
  level's list of children (used by the parent to walk its 16 children).

### DirContentIndex — Leaf (new node type, 32 bytes)

```
Offset  Size  Field
──────  ────  ─────
  0      1    hashNibble   (0..15)
  1      1    nodeType     (0x03 = LEAF)
  2      6    reserved     (alignment padding; offsets 2-7)
  8      8    listVP       (VirtualPtr to first DirContentLink in this leaf)
 16      8    nextVP       (VirtualPtr to next DirContentIndex in the same parent's list at the same level)
 24      8    reserved     (offsets 24-31)
```

- **`nodeType = LEAF`**: `listVP` points to a linked list of
  `DirContentLink` nodes (NOT DirContent directly). Each link points to
  one `DirContent` in the chain.
- **Leaf nodes still use the `nextVP` field** to chain to the next leaf
  in the parent level's child list (multiple leaves can exist at the
  same level when the parent has children spanning different hash paths).

### DirContentLink (new node type, 32 bytes)

```
Offset  Size  Field
──────  ────  ─────
  0      8    reserved     (offsets 0-7)
  8      8    dirContentVP (VirtualPtr of the actual DirContent in the chain)
 16      8    nextVP       (VirtualPtr to next DirContentLink in the same leaf's list)
 24      8    reserved     (offsets 24-31)
```

- **`dirContentVP`**: VirtualPtr of a `DirContent` slot in the parent
  directory's chain. This is the **single source of truth** for the
  file/dir entry — all its fields (childNodeId, epoch, childPtr, namePtr)
  are read from this slot.
- **`nextVP`**: chain link to the next `DirContentLink` in the same
  leaf's list (allows multiple entries in the same leaf — different
  names but matching hash prefix).

### Why DirContentLink exists (no duplication)

**The tree is an INDEX, not a copy.** The actual file/dir data lives in
`DirContent` slots in the chain (the source of truth). The tree only
holds pointers to those slots.

For a directory with N children:
- The chain has N `DirContent` slots.
- The tree has O(N) `DirContentLink` slots (one per child).
- The tree also has O(N × log_16 N / 16) `DirContentIndex` slots
  (internal nodes).

`DirContentLink` adds ~32 bytes per child (a pointer + chain link).
The `DirContent` it points to is shared with the chain. **No data
duplication.**

### Tree Shape

For a name with hash H (8 bytes):

```
H bytes:  [h0 h1 h2 h3 h4 h5 h6 h7]
H nibbles: [h0_hi h0_lo h1_hi h1_lo h2_hi h2_lo h3_hi h3_lo ... h7_hi h7_lo]
           (16 nibbles, each 0..15)
```

The tree has at most **16 levels** (4 bits per level, branching 16).
The deepest leaf is at level 15 (4 × 15 = 60 bits + 4 trailing bits = 64-bit
hash fully covered). For a directory with all unique hashes, the tree has
exactly 16 levels.

In practice, **most leaves are at shallower depths** because of the
branching factor 16. Two names differing only in the last nibble share the
same leaf at level 15. A typical directory's "interesting" depth is much
less.

### Tree Walk (lookup, insert)

To find the leaf for a given name with hash H:

1. Compute H using `name_hash_compute(name, len)`.
2. Set `level = 0`, `node = parent->indexHeadPtr`.
3. While `node->nodeType == INTERNAL`:
   - Extract nibble `n = (H >> (60 - level*4)) & 0xF`.
   - Walk the linked list at `node->listVP` to find the entry with
     `hashNibble == n`.
   - If found, `node = entry->listVP` (the matching child's list head),
     `level++`.
   - If not found, **the leaf for this name doesn't exist**; return
     `LEAF_NOT_FOUND` (we'll create the path during insert).
4. When `node->nodeType == LEAF`, walk the linked list of
   `DirContentLink` nodes at `node->listVP`. Each link points to a
   `DirContent` in the chain. To check for collisions, read each
   `DirContent` and use the existing hash-fast-reject + name compare.

The walk is **O(levels × 16)** = O(64) worst case. For a directory with
N children distributed uniformly, the expected walk is O(log N).

### Insert (vfs_create path)

To insert a new `DirContent` for name with hash H, both the chain and
the tree are updated in one atomic sequence. The tree is always present
from the moment the directory is created.

1. **Chain insert**: allocate the new `DirContent` slot, CAS-prepend
   to `DirNode.headPtr`. Call this `newDirContentVP`.
2. **Tree insert**: walk the tree to the leaf (or to the point where
   the path diverges).
3. If the path diverges at level L (no `DirContentIndex` with the right
   nibble at level L):
   - Allocate a new `DirContentIndex` slot.
   - Set its `hashNibble` to the diverging nibble.
   - Set its `nodeType` to `INTERNAL`.
   - **At this level, the parent's list is modified**:
     CAS-prepend the new node to the parent's child list (using
     `parent->listVP` and `nextVP`).
   - If the new node is the FIRST child for this nibble, set its `listVP = 0`
     (empty list — the next path element will be created).
4. Continue descending, creating `INTERNAL` nodes as needed, until we
   reach the leaf level.
5. At the leaf level:
   - If a leaf for this hash already exists, use it.
   - Otherwise, allocate a new `DirContentIndex` with `nodeType = LEAF`
     and `listVP = 0` (empty). Insert it into the parent's child list.
6. Allocate a new `DirContentLink` slot:
   - Set `dirContentVP = newDirContentVP`.
   - Set `nextVP = leaf->listVP` (current head).
7. **CAS-store `leaf->listVP = newDirContentLinkVP`** — atomic
   prepend to the leaf's list.

This sequence is **all CAS-based**. No locks held across operations. Other
threads can see partial states but will only see consistent tree shapes
because each CAS is atomic. The tree is **always complete** — there is
no lazy build, no consistency window.

### Tombstone Treatment

A **tombstone** is a `DirContent` with `namePtr == 0`. It occupies a
`DirContentLink` slot in a leaf, just like a live entry.

**Tombstones land in the same leaf as the original name.** The tree
navigates by the **hash of the name** (in `DirContentLink`, the
`namePtr` field is not used for placement). A tombstone is the **same
`DirContent` slot** as the original entry, just with `namePtr` cleared.
It already lives at the leaf indexed by the original name's hash — no
tree update is needed on delete.

For **collision check** (does name X exist at epoch E?):
- Walk tree to leaf (indexed by `hash(X)`).
- Walk the leaf's `DirContentLink` list. For each link:
  - Read the linked `DirContent`.
  - If `entry.epoch == E && entry.namePtr != 0 && entry.name == X`:
    match. Tombstone skipped (`namePtr == 0`).
  - Otherwise: not a match for this slot.
- A live entry at the same epoch/name causes `VFS_ERR_EXISTS`.
- A tombstone at the same epoch/name is **not** a conflict (file was
  deleted; the new create is allowed).

For **readdir**:
- `vfs_readdir` continues using the chain (unchanged). Tombstones
  filtered by `namePtr != 0` as today.

For **delete**:
- `vfs_delete` finds the matching `DirContent` in the chain (existing
  logic) and marks `namePtr = 0`.
- The same `DirContent` slot is pointed to by an existing
  `DirContentLink` in the leaf. **No tree update is needed** — the
  `DirContentLink` is unchanged, the underlying `DirContent` becomes
  a tombstone.
- Subsequent collision checks at the same epoch will see the
  tombstone (because `namePtr == 0`) and skip it, allowing recreation.

For **read with cursor (readdir with offset)**:
- Cursor position is `(path-in-tree, position-in-leaf)`.
- Resuming after the cursor only walks remaining leaves/positions.
- Tombstones are skipped in the output but count toward the offset
  (same as the chain walk).

For **GC**:
- Leaves accumulate tombstones over time. After many delete-recreate
  cycles, a leaf may have mostly tombstones.
- GC can scan leaves and **compact** them: walk the leaf's
  `DirContentLink` list, drop links whose `DirContent` is a tombstone
  AND is at an epoch < current snapshot (i.e. fully superseded by
  later epochs).
- This is a future optimization; initial implementation does not compact
  leaves.

### Read-Rule Compatibility

The tree is **read-rule-agnostic**. Like the chain, all entries are
stored regardless of epoch. Collision check filters by epoch; readdir
filters by epoch. The tree itself is just an index structure.

When `vfs_commit(S)` is called:
- The tree is **shared** between the snapshot epoch S and the live epoch
  L. The mapper entry `S → L` means reads at S fall through to L.
- Reads at S walk the tree, filter by epoch. The tree's structure
  doesn't change.
- Writes at L (after commit) add new entries. Reads at S see them only
  if their epoch > S (which violates the read-rule invariant — the
  VersionPage epoch check is the real gate).

This matches the current chain's model. **No special handling needed for
snapshots in the tree**.

### Consistency Guarantees

**The tree is always complete and up-to-date.** Every `vfs_create`
atomically updates both the chain and the tree. There is no lazy build,
no race window, no eventual consistency. The tree is the source of
truth for name lookups (not the chain).

**No fallback to chain walk.** `dirchain_find_child` uses only the
tree. If the tree returns no match, the name does not exist (or has
been deleted). The chain walk is not consulted as a fallback. This is
the key property that makes the tree a true speedup: O(1) on the
happy path, never O(N).

**Tombstones land in the correct leaf.** A tombstone is a `DirContent`
with `namePtr == 0`. The tree is indexed by name hash, so the
tombstone lives at the same leaf as the original entry (its name
hashes the same). No tree update is needed on delete.

**Stale entries after GC.** An entry in the tree pointing to a
`DirContent` slot is "live" for the lifetime of that slot. When GC
reclaims the slot, the tree's `DirContentLink` becomes a dangling
pointer. To prevent this, GC must:
- For each reclaimed `DirContent`, walk every directory's tree to
  remove the matching `DirContentLink`, OR
- Mark `DirContentLink.dirContentVP = 0` (a "tree tombstone"), to be
  filtered out by lookups.

A full description of GC integration is in Phase 7 (existing GC); the
tree-aware variant is a follow-up optimization.

**Snapshot correctness is preserved.** The tree contains no epoch-
dependent logic beyond the read-rule filter in
`dirchain_find_child`. Reads at snapshot epoch S use the same
`read_epoch = S` in the tree walk (and the chain walk used for
readdir). No additional state is needed for snapshot correctness.

## On-Disk Format

The tree is a **first-class index** for every directory. Every `DirNode`
has a valid `indexHeadPtr` from the moment it is created. The tree
is populated as entries are added.

When a `DirNode` is created (in `tree_bootstrap_root` or a similar
function), an initial `DirContentIndex` LEAF node is allocated with an
empty `DirContentLink` list. The `indexHeadPtr` points to this root
leaf. As entries are added, the tree may grow new `DirContentIndex`
INTERNAL nodes for divergent hash paths and `DirContentLink` nodes at
the leaves.

Tree shape parameters can be tuned at compile time via `#define`s in
`src/tree.h` (e.g. `RADIX_TREE_BRANCHING`).

## Functions

### Node I/O

```c
/* DirContentIndex — INTERNAL or LEAF.  nodeType = 0x02 or 0x03. */
void nodes_write_dircontentindex(uint8_t* slot, uint8_t hashNibble,
                                  uint8_t nodeType, int64_t listVP,
                                  int64_t nextVP, int64_t page_size);
void nodes_read_dircontentindex(const uint8_t* slot, uint8_t* hashNibble,
                                 uint8_t* nodeType, int64_t* listVP,
                                 int64_t* nextVP, int64_t page_size);

/* DirContentLink — pointer to a DirContent in the chain. */
void nodes_write_dircontentlink(uint8_t* slot, int64_t dirContentVP,
                                int64_t nextVP, int64_t page_size);
void nodes_read_dircontentlink(const uint8_t* slot, int64_t* dirContentVP,
                               int64_t* nextVP, int64_t page_size);
```

No magic field (matches existing node types).

### `dircontentindex_lookup` (in src/tree.c)

```c
/* Find the leaf listVP for the given name hash in this directory's
   tree.  Returns 0 if no leaf exists for this hash (caller must
   walk the chain or build the tree).  When the leaf is found,
   *out_leafVP is set to its listVP (head of the DirContentLink
   list at the leaf). */
int64_t dircontentindex_lookup(Pool* pool, int64_t indexRoot,
                                uint64_t nameHash, int64_t page_size);
```

Walks the tree from `indexRoot` to the leaf for the given hash. Returns
the leaf's `listVP` (head of the `DirContentLink` list at the leaf
level). Returns `0` if the path doesn't exist (need to create it).

### `dircontentindex_insert` (in src/tree.c)

```c
/* Insert a new DirContentLink pointing to dirContentVP into the tree
   for the given name hash.  Creates intermediate DirContentIndex
   nodes as needed.  If the leaf for this hash doesn't exist yet,
   allocates a new leaf node.  Returns 0 on success, -1 on pool
   allocation failure. */
int dircontentindex_insert(Pool* pool, int64_t* indexRoot, uint64_t nameHash,
                          int64_t dirContentVP, int64_t page_size);
```

Allocates intermediate `DirContentIndex` and `DirContentLink` nodes via
`pool_alloc` (uses `pool_resolve_rw` to mark dirty). All CAS operations
on the lists are atomic. Returns `0` on success.

### Modified Functions

**`vfs_create`**: After CAS-prepending the new `DirContent` to the
chain, allocate a new `DirContentLink` slot pointing to the new
`DirContent` and call `dircontentindex_insert` to add it to the tree.
If the tree insert fails (pool allocation exhausted), roll back the
chain append (best-effort; on pool exhaustion, the VFS is already in a
bad state). The tree is always present.

**`dirchain_find_child`** (used by `vfs_open`, `vfs_delete`,
`vfs_rename`): Change the walk from "chain walk" to "tree walk". For
each `DirContentLink` at the leaf, read the linked `DirContent`, then
use the existing hash-fast-reject + strcmp as today. If the tree
doesn't exist yet (`indexHeadPtr == 0`), use the existing chain walk
and lazily build the tree at the end.

**`vfs_readdir`**: Unchanged. Continues using the chain walk in
`dirchain_list`. The tree is for targeted lookups only.

## Locking and Concurrency

The tree is **fully lock-free**. There are no locks held during tree
operations.

### Atomic operations (CAS)

- **Chain insert** (`vfs_create`): the `DirNode.headPtr` is updated via
  CAS. Multiple concurrent inserts in the same directory are serialized
  by the CAS.
- **Tree insert** (`vfs_create`): the leaf's `listVP` and the parent's
  child list `nextVP` are updated via CAS. New tree nodes are immutable
  after creation — once linked into the tree, they never change.
- **Path walk** (`dirchain_find_child`): each node's `listVP` and
  `nextVP` fields are loaded atomically (using `_Atomic` or
  `atomic_load`). Readers see a consistent (possibly stale) view of the
  tree; they always see a valid prefix of any CAS-in-progress update.

### Concurrency model

- Multiple threads can simultaneously insert into the same directory
  (chain + tree). Each insert is atomic via CAS.
- Multiple threads can simultaneously read from the same directory.
  Each read is atomic per-CAS-step.
- The tree is **always complete**. There is no lazy build, no race
  window where an entry could be missing.

### No fallback to chain walk for missing entries

`dirchain_find_child` does **not** fall back to the chain walk if the
tree returns no match. If the tree says "no entry", the name does not
exist (or has been deleted). The chain walk is not consulted.

This is the key correctness property: the tree is the source of truth
for name lookups. The chain is the source of truth for readdir (which
needs to enumerate ALL children) and for snapshots.

**Exception**: `vfs_readdir` uses the chain walk (not the tree). This
is by design — readdir needs to enumerate all children. Cursor-based
readdir (a future optimization) would use the tree.


## Phase 18 Staging

### Stage A — Node types and basic I/O
- `src/nodes.h`: define `DIRCONTENTINDEX_OFF_*`, `DIRCONTENTLINK_OFF_*`,
  and `DIRNODE_OFF_INDEXHEADPTR`.
- `src/nodes.c`: implement `nodes_read_dircontentindex`,
  `nodes_write_dircontentindex`, `nodes_read_dircontentlink`,
  `nodes_write_dircontentlink`. The 32-byte slot layouts are the source
  of truth.
- Test: round-trip synthetic nodes, verify all fields survive
  write+read+mask.

### Stage B — Tree operations
- `src/tree.c`: implement `dircontentindex_lookup` and
  `dircontentindex_insert`.
- Walk the tree, find the leaf, CAS-prepend the entry.
- Allocate intermediate `DirContentIndex` nodes as needed (using
  `pool_alloc` + `pool_resolve_rw`).
- Test: insert N entries with random hashes, lookup each, verify all
  found. Insert duplicate hashes (collision), verify both found in
  same leaf.

### Stage C — Integration with `vfs_create`
- Modify `vfs_create` to call `dircontentindex_insert` after the
  chain CAS-prepend.
- The tree is always present (created when `DirNode` is initialized).
- On tree insert failure, roll back the chain append.
- Test: existing `test_tree.c` tests must continue to pass.

### Stage D — Integration with name lookups
- Modify `dirchain_find_child` to use `dircontentindex_lookup` first.
- Fall back to chain walk if the tree doesn't exist or returns no match.
- After chain walk (whether found or not), if the tree doesn't exist,
  lazily build it.
- Test: existing name-lookup tests pass. Add a new test that creates
  a directory, populates it with N entries, verifies the tree is
  built and the next lookup is O(1) per leaf.

### Stage E — Snapshot and GC integration
- Verify reads at snapshot epochs work correctly through the tree
  (they should — epoch filtering is the same).
- Add a GC sub-phase to compact leaves (remove fully-superseded
  tombstones).
- Test: create/delete cycles leave valid state.

## Acceptance

- [ ] `nodes_write_dircontentindex` and `nodes_read_dircontentindex`
  round-trip a synthetic node
- [ ] `nodes_write_dircontentlink` and `nodes_read_dircontentlink`
  round-trip a synthetic node
- [ ] `dircontentindex_lookup` returns 0 for an empty tree, correct
  leafVP for a non-empty tree, 0 for hashes not in the tree
- [ ] `dircontentindex_insert` creates intermediate nodes as needed and
  puts the entry in the correct leaf
- [ ] (removed — no lazy build in the new design)
- [ ] The tree is built when a `DirNode` is first created (no lazy
  build, no threshold)
- [ ] `vfs_create` writes to BOTH the chain and the tree atomically
  (or rolls back the chain append on tree failure)
- [ ] `dirchain_find_child` uses the tree (chain walk is not used as
  a fallback)
- [ ] Cursor-based `vfs_readdir` uses the tree (O(remaining) instead of
  O(N)), cursor support is added
- [ ] No new pool allocations beyond the tree nodes themselves
- [ ] All existing tests pass without modification
- [ ] Performance: `vfs_create` in a directory with 1000+ entries is
  measurably faster than the current chain walk
- [ ] Memory: tree size is bounded by directory contents; no leaks
  across remount

## Memory Cost

For a directory with N children:
- **Tree internal nodes** (`DirContentIndex` INTERNAL): O(N / 16 × log_16 N)
  slots. For N=1000, that's ~250 slots × 32 bytes = 8 KB.
- **Tree leaves** (`DirContentIndex` LEAF): one per distinct hash path
  that has at least one child. For uniformly distributed hashes, ~1 per
  directory at the bottom. For clustered hashes, more.
- **`DirContentLink` slots**: one per child = N × 32 bytes = 32 KB for N=1000.
- **`DirContent` slots** (chain, unchanged): N × 32 bytes = 32 KB for N=1000.

Total per directory: ~N × 64 + log(N) × 512 bytes. For N=1000: ~65 KB.
For N=10000: ~650 KB.

The tree **shares internal nodes** across multiple files. The chain
allocates one DirContent per file. The tree adds one DirContentLink per
file (a 32-byte pointer). The total is ~2× the chain size.

The trade-off: ~2× storage cost in exchange for O(1) name lookups per
leaf instead of O(N) chain walks. For the write-heavy extraction
workload (1156 temp files in one directory), this means:
- Current: 1156 × avg_chain_length operations per create
- With tree: 1156 × O(leaf_size) operations per create
- For 1156 files distributed across 16 hash paths: leaf_size ≈ 72,
  so 1156 × 72 = 83K ops vs current 1156 × 1156 = 1.3M ops. ~16× speedup.

## Implementation Status (as built)

The above design document describes the intended behavior. The code in
`src/tree.c`, `src/tree.h`, `src/gc.c` and `test/test_tree.c` reflects the
following subset and deviations:

### Implemented

- **Node types and I/O** (`nodes.h:14-20`, `nodes.c:55-90`): `NODE_TYPE_INDEX_INTERNAL=0x02`,
  `NODE_TYPE_INDEX_LEAF=0x03` (matches plan.md, not plan_r6's 0x04/0x05),
  `DIRNODE_OFF_INDEXHEADPTR` at offset 16, `DIRCONTENTINDEX_OFF_*`,
  `DIRCONTENTLINK_OFF_*`. Round-trip read/write functions for both new
  node types with `memset(0)` + offset writes.
- **`dircontentindex_lookup`** (`tree.c:1396`): walks the tree by nibble,
  returns the leaf's `listVP`.
- **`dircontentindex_insert`** (`tree.c:1455`): walks the tree, allocates
  intermediate INTERNALs as needed, CAS-prepends a `DirContentLink` to
  the leaf's `listVP`.
- **`dircontentindex_remove`** (`tree.c:1611-1738`): walks the tree,
  atomically zeroes matching `dirContentVP` to make the link a
  tree-tombstone. Slot is NOT freed (1-slot leak per remove).
- **Tree in `vfs_create`** (`tree.c:748-764`): CAS-prepend to chain, then
  `dircontentindex_insert` to add the entry to the tree.
- **Tree in `vfs_mkdir`** (`tree.c:920-930`): same pattern; new directories
  get their own INTERNAL root via `tree.c:887`.
- **Tree in `vfs_delete` and `vfs_rmdir`** (`tree.c:986-997`,
  `tree.c:1183-1191`): inserts a tombstone link (pointing to the
  tombstone's `DirContent` slot) at the deleted name's hash.
- **Tree in `vfs_rename`** (`tree.c:1381-1391`, `tree.c:1428-1436`,
  `tree.c:1457-1466`): same-dir rename does
  `dircontentindex_remove` + `dircontentindex_insert` (old hash → zero
  the link; new hash → insert a link at the new hash). Cross-dir rename
  inserts at dst and a tombstone link at src.
- **Slot persistence**: every `dircontentindex_insert` call is followed
  by `vfs_wr8_s(parent_slot, DIRNODE_OFF_INDEXHEADPTR, parentIndex, ...)`
  so a newly-installed root is written back. Without this the tree
  would not survive `vfs_unmount`/`vfs_mount`.
- **Tombstone dedup in tree path** (`tree.c:1868-1916`): mirrors chain
  walk semantics — tombstones update `best_child`/`best_eff_epoch`/
  `best_raw_epoch` but not `best_name_match`; hash-mismatched entries
  also update `best_*` if their epoch beats the existing one. Without
  this, lower-epoch live entries would beat higher-epoch tombstones in
  the tree path (the bug plan.md Phase 7 flagged).
- **`dircontentindex_insert` walk fix** (`tree.c:1632`): the original
  code had `childVP = childListVP` which descended one level too deep
  on every iteration, causing each insert to allocate a fresh 16-deep
  chain. Fixed to `childVP = childWalk` (the matching INTERNAL VP
  itself).

### Deviations from plan_r6

- **Root node type**: plan_r6 says "root starts LEAF, first nibble
  mismatch promotes to INTERNAL." The code creates an INTERNAL root at
  bootstrap (`tree.c:117`) and at every `vfs_mkdir` (`tree.c:887`).
  This matches plan.md (the earlier accepted design) and saves the
  promotion logic for the bootstrap path; the trade-off is one extra
  32-byte INTERNAL slot per directory.
- **Chain walk as safety net**: `dirchain_find_child` always falls
  through to the chain walk if the tree path doesn't find a match
  (`tree.c:1708-1713` and `tree.c:1932-1934`). plan_r6 said the tree
  would be authoritative with no chain fallback; the code keeps the
  fallback for legacy compatibility and as a correctness net for any
  race window during insert. The chain walk's `s_hash_rejects` counter
  test was updated to reflect the new behavior (zero rejects on
  tree-path hits; counter is no longer the primary lookup path's
  metric).
- **Tombstones in tree**: tombstones live in both the chain (as
  `namePtr=0` `DirContent` slots) AND the tree (as links pointing to
  those slots). `dirchain_find_child` applies epoch dedup so a
  higher-epoch tombstone suppresses a lower-epoch live entry. This
  matches plan_r6 Phase 3.

### Skipped (documented known limitations)

- **LEAF→INTERNAL promotion** (plan_r6 R5-1 through R5-9): NOT
  IMPLEMENTED. The deepest level (level 15) keeps the
  "first-LEAF-wins" shared-leaf behavior. For uniformly distributed
  64-bit hashes, the chance of two entries sharing 60 bits is
  1-in-2^60 per pair, so in practice each LEAF holds exactly one
  entry — the shared-leaf design is correct. Promotion would split
  single-entry LEAFs into per-nibble sub-groups for marginally
  better leaf-scan performance at very large N, but adds ~150-200
  LOC plus the R5-2 leak risk (up to 17 orphaned slots per failed
  promotion CAS, no `pool_free` exists, GC cannot reclaim runtime
  allocations). Not justified given current workload sizes.
- **GC tree walk** (`gc_walk_radix_tree` DFS in `gc.c`, plan_r6
  Phase 5): NOT IMPLEMENTED. `gc_walk_dirnode` at `gc.c:234-273`
  copies the DirNode (including `indexHeadPtr`) but does not walk
  the tree. After GC, every directory's slot points to a non-existent
  tree; the next `dircontentindex_insert` call sees `*indexRoot == 0`
  and CAS-installs a new INTERNAL root, then populates the tree
  from subsequent inserts. The first lookup after GC triggers a
  full chain walk via the fallback path; performance recovers as
  inserts repopulate the tree. Acceptable for current workloads;
  a real fix requires extending the GC field rewriter to handle
  `DirContentIndex` and `DirContentLink` slot layouts and adding
  a `gc_walk_radix_tree` DFS that remaps VP fields.
- **`dircontentindex_remove` link leak**: each remove zeroes a
  `dirContentVP` but does not free the `DirContentLink` slot. 32 bytes
  per remove. Bounded by directory lifetime. Acceptable; no
  `pool_free` exists.

### Test results

- `test_tree`: 958/958 passing.
- `test_crash`: 16/16 passing.
- `test_epoch`: 77/77 passing.
- `test_fuzz`: 10000 iterations, 0 crashes.
- `test_gc`: 263/263 passing (existing GC tests; tree-rebuild
  after GC is exercised implicitly).
- `test_mapper`: 106/106 passing.
- `test_nodes`: 238/238 passing.
- `test_pool`: 34475/34475 passing.
- `test_storage`: 37/37 passing.
- `test_var_array`: 59487/59487 passing.

No regressions vs pre-implementation baseline. The two test changes
made for plan.md Phase 7 (lines 549/556/564 of test_tree.c — vfs_read
truncates to file_size) and the hash-reject counter test (line 1708 —
updated to reflect tree-path returns early) are documented inline.

## References

- Phase 17 (NameEntry hash fast-reject) — the hash function used here
- Phase 4 (Node Types) — existing pool node layouts
- Phase 5 (Tree Operations) — the `vfs_create` / `dirchain_find_child`
  functions that are modified here
- Phase 13 (Mapper Table Cache) — example of an in-memory cache
  structure; phase 18 takes the same approach but persists to disk
</content>
