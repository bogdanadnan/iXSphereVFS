# Phase 28 (Step 1): Free-Page Inventory

**Status:** Planning document. Step 1 of the GC refactor. To be
followed by a full Phase 28 spec with the design + workloads,
following the Phase 27 spec workflow (3 review iterations before
implementation).

**Goal:** Enumerate every code path that can result in a logical
page becoming free (i.e., a page that was allocated is no longer
reachable from the live tree). For each path, document:
- The state of the page at the moment it's about to be freed
  (indirection entry, PageHeader status, chain membership).
- The cleanup responsibilities *before* `storage_free` is called
  (which chain links to break, which siblings to invalidate, etc.).
- Whether the free is **immediate** (called from the path that
  produces the orphan) or **deferred to GC** (the path produces
  the orphan, but a separate pass walks the tree to find all
  unreferenced pages and frees them in a batch).

This is the foundation document for the GC refactor: every free
path must have its state machine understood, because GC must
re-enact the same state transitions correctly when it reclaims
pages out-of-band.

---

## Broader scope: GC handles ALL garbage, not just free pages

The original framing of "GC = free-page collector" is too narrow.
GC's actual job is to handle **all deviations from the clean slate**
that any operation leaves behind. The clean slate is:

- **Indirection table**: every entry that should be set is set,
  every entry that should be 0 is 0
- **Dir chains** (DirNode → DirSegment → SlotNode → DirContent):
  no tombstones, every entry is live
- **File chains** (FileNode → FileContent → PageNode →
  VersionPage): every entry at a unique epoch, every data page
  is in `indir` and is reachable from some chain
- **Mapper**: no soft-deletes, no `traversalApply` flags
  (every entry resolves to itself)
- **FileSize / FileMTime chains**: every entry at a unique epoch

Anything that deviates from this is **garbage** that GC must
process. There are **5 distinct garbage types**, produced by
**7 public operations** (the previous count of 6 free-page
paths was too narrow — it didn't include `vfs_commit`'s
Type-4 garbage and the Type-2 tombstones from `vfs_rename`).

### Garbage type 1: Free pages (orphaned data pages)

The previously-documented free-page paths (1-6 below). Pages
that are in `indir` but no longer reachable from any live tree
walk. GC reclaims via `storage_free`.

### Garbage type 2: Tombstones in dir chains

`vfs_delete` and `vfs_rename` add **tombstone DirContent
entries** to mark a file as deleted. The tombstone is a pool
slot in the chain — the slot is allocated, the entry is
"live" in the chain, but the file is gone. The slot is
wasted space in the pool.

**What GC must do:** during the pool rebuild, drop tombstone
entries (entries with `namePtr == 0` or `deleted != 0`). The
tombstone's pool slot is then free (the slot was allocated
by `pool_alloc`; after the GC's copy step drops it, the slot
goes to the new pool list's free slot list, or the old pool
page is freed and the slot just disappears with the page).

**Producers:**
- **vfs_delete**: 1 tombstone per file deletion
- **vfs_rename**: 1 tombstone per rename (for the old name)

### Garbage type 3: Soft-deleted mapper entries

`vfs_delete_snapshot` / `vfs_rollback` insert a **soft-delete
mapper entry** with `traversalApply=false`. The entry is in
the mapper chain, but the snapshot is no longer valid — reads
at that epoch fall through to the base epoch.

**What GC must do:** during the mapper rebuild, drop the
soft-delete entry. The chain then resumes from the base
epoch directly. Any data pages that were ONLY referenced by
the soft-deleted snapshot are reclaimed as Type-1 garbage.

**Producers:**
- **vfs_delete_snapshot** / **vfs_rollback**: 1 soft-delete
  entry per snapshot deletion

### Garbage type 4: Committed mapper entries + chain rewriting

`vfs_commit` inserts a **committed mapper entry** with
`traversalApply=true` and `toEpoch = currentEpoch`. The
entry is in the mapper chain. The VersionPage chain at the
snapshot epoch has entries that the read-rule rewrites to
`currentEpoch` on every walk.

**What GC must do:** during the mapper rebuild, the committed
entry can be DROPPED (the read-rule no longer needs the
redirect — after GC rewrites the chain, the chain entries
at `currentEpoch` directly serve all future reads). During
the version chain walk, GC must **rewrite the S-epoch entries
to E-epoch entries** (matching the read-rule's behavior) so
that future reads can find them under the new epoch label
without a mapper lookup.

This is the most subtle garbage type: the chain is
self-consistent (reads work), but it's in a "redirected"
state that requires the GC's chain-rewrite pass to make
permanent.

**Producers:**
- **vfs_commit**: 1 committed entry per commit; the chain
  rewrite applies to every chain walked by the GC

### Garbage type 5: Future — pool compaction

Not a current path. The pool's slot list can become
fragmented over time. A future GC pass could compact the
pool (move slots to new pages, free old pages). When
implemented, this would produce both Type-1 (freed old
pool pages) and slot-level garbage (the freed slots).

---

## Existing free-page infrastructure (Phase 27 W2/W3/W6)

`storage_free(StorageBackend* sb, int64_t logical_page)` is the
single entry point for returning a page to the free list. It:

1. Reads the page's current physical offset via `indir_lookup`.
2. Checks `logical_page >= 2` (don't free header/superblock).
3. No-op if `indir` is already 0 (idempotent).
4. Sets `indir[logical_page] = 0` via `indir_set`.
5. Invalidates the cache entry for the page.
6. Enqueues `(logical_page, physical)` into the free-page queue
   (per-page LIFO queue, page-local LIFO).

**Required pre-state for `storage_free` to be safe:**
- `indir[logical_page]` is set (points to a valid physical offset)
- The page is not referenced by any live tree walk (no
  VersionPage, FileContent, FileSize, DirContent, DirSegment,
  SlotNode, or PoolSlot chain holds the page)
- The page is not in the deferred-free queue (if a free-list
  consumer allocated the page, but GC hasn't confirmed yet)
- The page's PageHeader may be in any state (the flush will
  write a new header if needed; the existing header is irrelevant
  for free because the page is about to be reallocated)

**Post-state after `storage_free`:**
- `indir[logical_page] = 0`
- The free-page queue has a new entry (head or tail)
- The page is eligible for the next `dequeue_from_free_list`
  call (in `storage_allocate(1)`)
- The page's cache entry is invalid (next read goes to disk,
  but disk content is irrelevant since the slot is now "free")

**What `storage_free` does NOT do (and why):**
- It does **not** clear the mirror sibling. The mirror exists
  on disk (Phase 2 lazy mirror); the next allocation of the
  page will overwrite both copies via `mirror_write`.
- It does **not** write anything to disk. The free-list queue
  itself is a metadata page; the page being freed is left as-is
  on disk (its content is stale, but irrelevant).

---

## Free-page paths

There are exactly **5 paths** that produce free pages in the
current codebase. Three are **immediate** (call `storage_free`
directly); two are **deferred to GC** (the path leaves garbage
that GC will later clean up).

### Path 1: `mirror_write` second-write failure (IMMEDIATE)

**Where it lives:** `src/lazy_mirror.c:159-205` (the W4 H2 fix,
unchanged in W5/W6/W8).

**What it does:** When `mirror_write` writes a page for the
second time (the page already has a single copy on disk), it
allocates a mirror sibling via `storage_allocate(1)`. If any
of the subsequent steps fail (sibling allocation, page lookup,
write, or link), the sibling page is now an orphan. The H2 fix
calls `storage_free(sibling)` to return it to the free list.

**State at free time:**
- The sibling's `indir` entry is set (just allocated)
- The sibling has a valid physical offset on disk
- The sibling has a PageHeader (written by the partial
  `mirror_write` that failed) but the page content may be
  partial or missing (depending on which failure path)
- The mirror relationship: the original page's PageHeader
  has `mirror_page = -1` (single copy, no link) — the link
  was never written

**Why safe to free immediately:**
- The original page is still intact on disk (mirror_write
  never overwrites the original)
- The sibling is brand-new (just allocated), no other code
  path has a reference
- All 4 failure paths (allocate/lookup/write/link) are caught
  before the mirror link is established, so no other chain
  references the sibling

**Number of pages freed per event:** 1 (the sibling).

---

### Path 2: GC pool page remap (DEFERRED, then confirmed)

**Where it lives:** `src/gc.c:1158-1207` (the GC pool rebuild
section), with the actual free happening in
`deferred_free_confirm_and_release` at `src/gc.c:193-202`.

**What it does:** During GC, the old pool list is walked
page-by-page. Each old pool page is enqueued into the
deferred-free queue (`deferred_free_enqueue`). After the new
pool list + new mapper are built and the new superblock is
written, the deferred-free queue is confirmed (all in-flight
readers have finished) and the pages are released via
`storage_free`.

**State at free time (after confirmation):**
- The old pool page's `indir` entry is set (allocated)
- The old pool page has a valid physical offset on disk
- The old pool page's content has been **shadow-copied** to a
  new pool page (via `gc_copy_entry`). The old page is now
  a stale copy.
- The old pool page is NOT referenced by:
  - The new pool list (it's been replaced)
  - The new mapper (it's been rebuilt)
  - The new superblock (pointing at new values)
- The old pool page's PageHeader is still valid (the original
  generation, mirror_page=-1 since the page was single-copy)

**Why deferred (not immediate):**
- In-flight readers may hold `PoolSlot` copies of the old
  pool page. The deferred-free queue ensures the page is
  not reallocated until all readers have finished their
  traversals (the GC acquires the tree's exclusive lock, but
  this is a soft lock — readers in flight at the time of
  the lock acquire complete naturally).

**State at enqueue time (before confirmation):**
- The page is in the deferred-free queue
- The page is **not** eligible for `dequeue_from_free_list`
  (the dequeue checks `deferred_free_is_queued` and skips)
- The page is still in `indir` (the dequeue's `try_claim_entry`
  would succeed if the page were dequeued, but it's not)

**Number of pages freed per event:** All old pool pages
(typically a small handful, e.g., 1-3).

**Open issue (N2):** All `pool_acquire` calls in `gc.c` use
`pinPage=false`, which means the GC's `pool_release` is a
no-op. The shadow-copied entries are never written back to
the destination. This means GC's current implementation
silently loses all its work. The free pages are real, but
the new tree they were meant to populate is empty.

---

### Path 3: GC data page reclamation (DEFERRED, mixed)

**Where it lives:** `src/gc.c:1199-1207` (the loop
`for (page = 2; page < old_total_pages; page++)`).

**What it does:** After the GC walk identifies the live set
(via `live_page_set`), every page from 2 to the pre-GC
`total_pages` that is **not** in the live set is freed via
`storage_free`. The loop skips pool pages (already in
deferred-free).

**State at free time:**
- The page's `indir` entry is set (allocated)
- The page has a valid physical offset on disk
- The page's content is intact (no corruption introduced by
  GC; GC only reads, never writes to data pages in this loop)
- The page's PageHeader is valid (original generation)
- The page is **not** in the live page set (the live set is
  built by walking the new tree's FileNode/PageNode/
  VersionPage/FileSize/DirNode/DirContent chains)

**What "live" means in this context:**
- A page is live if it is the data page of a VersionPage
  in any chain that survives GC (i.e., a chain that has
  entries reachable from the live head epoch or a
  non-soft-deleted snapshot)
- A page is dead if it is only referenced by:
  - Soft-deleted snapshots (mapper entry with
    `traversalApply=false`)
  - Tombstone DirContent entries (from `vfs_delete`/
    `vfs_rmdir`)
  - FileSize entries with size smaller than the highest
    surviving bound (from `vfs_truncate`)

**Why deferred (not immediate):**
- The live set can only be determined after the GC walk
  completes (the walk builds the live set incrementally).
  Pages produced by file deletion, snapshot deletion, or
  truncation are not "freed at the moment of orphaning" —
  they accumulate until GC runs.

**Number of pages freed per event:** All dead data pages
(typically a large number — every data page that was
written into a soft-deleted snapshot).

---

### Path 4: File deletion (DEFERRED, garbage accumulates)

**Where it lives:** `src/tree.c:1769` (`vfs_delete`).

**What it does:** `vfs_delete` adds a **tombstone** DirContent
entry to the parent directory's SlotNode chain. The file's
FileNode, FileContent, PageNode, VersionPage, FileSize, and
FileMTime chains are NOT modified. They become orphans —
unreachable from any live directory, but still on disk.

**State at the moment of orphaning (the "free" state):**
- The file's FileNode `indir` entry is still set
- The file's FileContent chain head is in the FileNode's
  SIZEPTR field (still valid)
- The file's PageNode chains (one per file page) are
  reachable from the FileContent chain
- The file's VersionPage chains (one per PageNode) are
  reachable from the PageNodes
- The file's FileSize chain is reachable from the FileNode
  (or from the PageNode's HEADPTR, depending on schema)
- The file's FileMTime is reachable from the FileNode
- All pages are on disk, with valid PageHeaders, valid CRC
- The tombstone DirContent (in the parent's SlotNode chain)
  marks the file as deleted

**What needs to happen for free:**
- All chain links above must be broken (the chains must
  not be reachable from any live tree walk)
- The `indir` entries for the orphaned pages must remain
  set (so GC can identify them as "allocated but unreachable")
- GC walks the live tree, builds the live set, and reclaims
  the orphans in Path 3

**Why deferred:**
- The file might be open (FUSE holds an inode reference)
- The file might be referenced by a non-soft-deleted snapshot
  (its data is part of that snapshot's view)
- Immediate free would require walking the chain and
  checking each page's reachability — this is exactly what
  GC does in a batch

**Number of pages freed per event:** All pages of the deleted
file (FileNode + FileContent + PageNode per page +
VersionPage per (page, snapshot) + FileSize + FileMTime). For
a file with 1000 pages and 5 snapshots, this is ~7000 pages.

---

### Path 5: Truncation (DEFERRED, garbage accumulates)

**Where it lives:** `src/tree.c:3628` (`vfs_truncate`,
shrink path only).

**What it does:** `vfs_truncate` to a smaller size appends a
new FileSize entry at the current epoch with the new (smaller)
size. The data pages past the new size are NOT modified.
They become unreachable from any future `vfs_file_size` read
at this epoch (the read-rule will stop at the new size), but
they may still be referenced by:
- Earlier snapshots (their FileSize chain stops at the
  snapshot's epoch, which was before the truncate)
- The live head epoch if a subsequent write re-extends the
  file (the FileSize chain rewinds to the truncate, then
  re-extends — the data pages past the truncate were never
  freed, so they re-appear)

**State at the moment of orphaning:**
- The data pages past the new size are still in `indir`
- The pages are still on disk (intact, valid CRC)
- The pages are reachable from VersionPage chains whose
  read-rule resolves to an epoch that didn't see the
  truncate
- The pages are NOT reachable from `vfs_file_size` at the
  current epoch (the new FileSize entry stops the read)

**What needs to happen for free:**
- The pages must not be reachable from any live VersionPage
  chain at any epoch
- This is determined by GC's `gc_walk_filesize_chain` —
  it walks the FileSize chain, identifies the highest
  surviving bound, and reclaims pages beyond that bound
- The pages are then freed in Path 3 (data page reclamation)

**Why deferred:**
- The truncate might be at a snapshot epoch; earlier
  epochs' FileContent chains may still reference the
  data
- The truncate might be reverted (by writing past the
  new size) — immediate free would prevent this
- Batching with GC's other reclamation is more efficient

**Number of pages freed per event:** (old_size - new_size) /
page_size pages, possibly less if other snapshots still
reference them.

---

### Path 6: Snapshot deletion (DEFERRED, garbage accumulates)

**Where it lives:** `src/epoch.c:365` (`vfs_delete_snapshot`).

**What it does:** `vfs_delete_snapshot` inserts a soft-delete
mapping into the mapper (`mapper_insert(snapshot_epoch,
toEpoch=base_epoch, traversalApply=0)`) and updates the
in-memory mapper table. The snapshot's data pages are NOT
modified.

**State at the moment of orphaning:**
- The data pages written at the snapshot epoch are still
  in `indir`
- The VersionPages at the snapshot epoch are still in
  the chain (prepended, newest first)
- The mapper now maps the snapshot epoch to the base epoch
  (the read-rule falls through to the base)
- Future reads at the snapshot epoch will see the BASE
  epoch's data, not the snapshot's data

**What needs to happen for free:**
- The VersionPages at the snapshot epoch must be
  reclaimed from each chain
- The data pages that were ONLY referenced by the
  snapshot's VersionPages can then be freed
- This is determined by GC's `gc_walk_versionpage_chain` —
  it walks each chain, drops entries with
  `traversalApply=false` (soft-deleted), and reclaims
  their data pages

**Why deferred:**
- The soft-delete is just a mapper change; the actual
  reclamation needs the full GC walk
- Batching with GC is more efficient

**Number of pages freed per event:** All data pages that
were written at the soft-deleted snapshot epoch AND are
not referenced by any other live epoch. For a snapshot
with 1000 page-writes, this could be up to 1000 pages.

---

## Summary table

### Free-page paths (Type 1 garbage — 6 paths)

| # | Path | When | Where (state) | Free mode |
|---|------|------|---------------|-----------|
| 1 | `mirror_write` sibling failure | Lazy mirror, second write | `indir` set, mirror link not written | Immediate |
| 2 | GC pool page remap | After GC walk | `indir` set, new pool already built | Deferred (queue) |
| 3 | GC data page reclamation | After GC walk + live set | `indir` set, not in live set | Deferred (queue) |
| 4 | File deletion | `vfs_delete` | Tombstone in dir, file chains orphaned | Deferred to GC |
| 5 | Truncation | `vfs_truncate` (shrink) | New FileSize entry, past-size pages orphaned | Deferred to GC |
| 6 | Snapshot deletion | `vfs_delete_snapshot` | Mapper soft-delete, snap-epoch chains orphaned | Deferred to GC |

### Other garbage paths (Types 2/3/4 — 3 operations)

| # | Operation | Garbage type | What GC does |
|---|-----------|--------------|--------------|
| 7 | `vfs_delete` | Type 2 (tombstone) | Drop the tombstone during pool rebuild |
| 8 | `vfs_rename` | Type 2 (tombstone) | Drop the tombstone during pool rebuild |
| 9 | `vfs_delete_snapshot` / `vfs_rollback` | Type 3 (soft-delete) | Drop the soft-delete entry during mapper rebuild |
| 10 | `vfs_commit` | Type 4 (committed entry + chain rewrite) | Rewrite chain S→E, drop the committed mapper entry |

**Total garbage producers: 7 public operations** (4 of which
also produce free pages). All 7 require GC processing — none
is a "free the page" call; they all leave garbage for GC to
reclaim, rewrite, or drop.

The GC's job is **comprehensive garbage collection**, not
just free-page reclamation:
1. Reclaim Type 1 (free pages) via `storage_free`
2. Drop Type 2 (tombstones) during pool rebuild
3. Drop Type 3 (soft-deletes) during mapper rebuild
4. Rewrite Type 4 (committed) chains + drop the committed entry

---

## Operations that are NOT garbage producers

For completeness, the following public operations were considered
and ruled out as garbage producers. They are pure read-only
operations or produce only "clean" allocations (slots that are
immediately reachable from the tree). Listed here so the Phase 28
reviewer doesn't ask "but what about X?" and the spec author
doesn't have to re-derive the analysis.

**IMPORTANT:** the operations that DO produce garbage are
listed in the "Summary table" above (paths 1-10). The list
below is operations that do NOT produce any of the 5 garbage
types.

### vfs_snapshot

`vfs_snapshot(vfs)` is a single atomic add (`currentEpoch += 2`).
The returned odd epoch is in-memory only (M6 design). No
allocation, no free, no I/O. The snapshot is cheap by design —
the intended use case is the SQLite VFS where each transaction
gets a snapshot (per the M6 rationale).

### vfs_create / vfs_mkdir

Allocate a new FileNode/DirNode + FileContent. The new slots
are reachable from the parent dir's DirContent chain
immediately after creation. **No garbage produced.** The
garbage comes later, when the file is deleted (which
produces Type-2 tombstone + Type-1 orphan data pages).

### vfs_rename (the new-name side)

The new-name side adds a fresh DirContent with the new name.
The new slot is reachable. **The new-name side produces no
garbage.** The OLD-name side (tombstone) is a Type-2
garbage producer (path 8 in the summary table).

### vfs_open / vfs_close / vfs_read

Read-only operations. No allocation, no garbage.

### vfs_write

COW write path. Allocates a new data page. The new data page
is reachable from the new VersionPage. **The write itself
produces no garbage.** The old data page is still
referenced by the old VersionPage (and only becomes garbage
when the old epoch is soft-deleted — see Type 3).

### vfs_flush

Writes dirty pages to disk via the cache. May trigger
`mirror_write` (Path 1's sibling allocation) but only on
I/O failure. In the success case, no garbage.

### vfs_truncate (GROW path only)

The grow path delegates to `vfs_write` (which allocates new
data pages, no garbage). The new pages are reachable. The
**shrink** path is a Type-1 garbage producer (path 5).

### vfs_lock / vfs_unlock / vfs_resolve_path / vfs_file_size

Read-only or lock-state operations. No garbage.

### vfs_utimens

Appends a new FileMTime entry to the chain. The old FileMTime
entry is still in the chain (reachable). **No garbage produced.**
The old entry becomes garbage only when the epoch that wrote
it is soft-deleted (which then triggers Type-1 reclamation
of the entry's data, but the entry itself is just dropped
during the GC's chain walk, not via `storage_free`).

---
1. Validates `snapshot_epoch` is odd and the mapper resolves to itself
2. Conflict-detection walk (commit_scan_dir) — read-only, flags
   a conflict and returns an error if both S and E have writes
3. `mapper_insert(snapshot_epoch, currentEpoch, traversalApply=true)`
   — metadata only
4. `storage_flush_cache_only` — persists the dirty pool page that
   holds the new mapper entry. No allocation, no free.
5. `mapper_table_append` — in-memory update, no I/O
6. `tree_superblock_write` — persists the new superblock. No
   allocation, no free.

**Why no free pages:** the commit changes the *visibility* of
the snapshot's data (via the mapper's `traversalApply` flag and
the read-rule's chain rewriting), not the *location* of the
data. The data pages written at `snapshot_epoch` are still
referenced by the VersionPage chain (rewritten to
`currentEpoch` on read). The pre-commit data pages at
`currentEpoch` (if any) are still referenced by the chain.
No duplicate allocation, no orphan.

**What commit does change:** the VersionPage's epoch field is
rewritten on read (via `vfs_chain_walk`'s mapper traversal-apply
logic). This is a single 4-byte update per chain walk, not a
new page allocation.

### vfs_snapshot

`vfs_snapshot(vfs)` is a single atomic add (`currentEpoch += 2`).
The returned odd epoch is in-memory only (M6 design). No allocation,
no free, no I/O. The snapshot is cheap by design — the intended
use case is the SQLite VFS where each transaction gets a snapshot
(per the M6 rationale).

### vfs_rollback

`vfs_rollback(vfs, snapshot_epoch)` is the same as
`vfs_delete_snapshot` (soft-delete via the mapper). Already
covered as **Path 6**. No new path.

### vfs_create / vfs_mkdir

Allocate new file/dir nodes. The allocation goes through
`pool_alloc` (creates a new pool page if needed) and writes
the new node. No free pages produced. The new file/dir becomes
*live* immediately (reachable from the parent dir's
DirContent chain). To free, use `vfs_delete` (Path 4).

### vfs_rename

Adds a new DirContent with the new name and a tombstone for
the old name. The file itself is unchanged. No free pages.

### vfs_open / vfs_close / vfs_read

Read-only operations. No allocation, no free.

### vfs_write

COW write path. Allocates a new data page for the write (if
not in-place). The OLD data page (referenced by the parent
VersionPage) is still reachable. **No free pages produced by
the write itself.** The old data page becomes free only when
the parent VersionPage is reclaimed by GC (Path 3).

**Subtle point:** if you write to a snapshot, the chain gets
a new VersionPage at the snapshot epoch. The data page is
allocated (COW from base). No free pages from the snapshot
write either.

### vfs_flush

Writes dirty pages to disk via the cache. May trigger
`mirror_write` (Path 1's sibling allocation) but only on
I/O failure. In the success case, no allocation, no free.

### vfs_truncate (GROW path only)

The grow path delegates to `vfs_write` (which allocates new
data pages). The new pages are reachable. No free pages.

The **shrink** path is **Path 5** (pages past the new size
become orphans, reclaimed by GC).

### vfs_lock / vfs_unlock / vfs_resolve_path / vfs_file_size

Read-only or lock-state operations. No allocation, no free.

### vfs_utimens

Appends a new FileMTime entry to the chain. The old FileMTime
entry is still in the chain (reachable). No free pages. The
old entry is reclaimed by GC when the snapshot that wrote it
is soft-deleted.

---

## Proposed GC Design: the "Bin"

The user proposed the following design (lightly structured here;
this is a sketch, not a final spec).

### Bin concept

A **Bin** is a persistent queue stored in the VFS backing file,
similar in spirit to the free-page queue (Phase 27 W2/W3). Each
Bin entry is a small fixed-size tuple:

```c
typedef enum {
    BIN_GC_TYPE_FREE_PAGES,        /* Garbage type 1 */
    BIN_GC_TYPE_TOMBSTONE,         /* Garbage type 2 */
    BIN_GC_TYPE_SOFT_DELETE,       /* Garbage type 3 */
    BIN_GC_TYPE_COMMITTED_REWRITE, /* Garbage type 4 */
    BIN_GC_TYPE_FUTURE_POOL_COMPACT, /* Garbage type 5 (future) */
} bin_gc_type_t;

typedef struct {
    bin_gc_type_t type;
    int64_t       context;  /* type-specific identifier */
} BinEntry;
```

**Context is type-specific:**
- Type 1 (free pages): context = a single identifier that tells the
  GC where to start the cleanup. For `vfs_delete`, context =
  deleted file's VirtualPtr. For `vfs_truncate` (shrink), context =
  file VP + new size (or some packed representation). For
  `vfs_delete_snapshot`, context = the deleted snapshot epoch.
  The GC walks from this context, identifies dead pages, and
  enqueues them into the deferred-free queue.
- Type 2 (tombstone): context = tombstone's pool-slot VirtualPtr.
  The GC removes the entry from the chain.
- Type 3 (soft-delete): context = deleted snapshot epoch.
  The GC removes the soft-delete entry from the mapper chain.
- Type 4 (committed): context = committed snapshot epoch.
  The GC rewrites the chain (S → E) and drops the committed
  mapper entry.

### Bin storage in the VFS file

Like the free-page queue, the Bin lives in the VFS file. Options:

- **(A)** Reserve 3 new header fields: `bin_head`, `bin_tail`,
  `bin_count`. Bin pages are regular storage pages (the Bin
  grows in pages of `MAX_BIN_ENTRIES_PER_PAGE` entries). The
  header points at the head/tail Bin pages. The Bin pages are
  themselves in the indirection (so GC can walk them).
- **(B)** Reserve a fixed-size region in the VFS file
  (e.g., 64 KB) for the Bin. Simpler but caps the in-flight
  garbage at a fixed number of entries.
- **(C)** Store Bin entries in the pool (the same way
  DirContent, FileSize, etc. are stored). The Bin is a
  chain of pool pages. Most flexible, most complex.

The spec should pick one. My recommendation: **(A)** — reuses
the existing page infrastructure (storage_allocate for Bin
pages, storage_free when the Bin page is empty), and the
header is already the natural place for queue metadata.

### Bin operations (API)

```c
/* Producer side: called by every garbage-producing operation. */
int bin_push(StorageBackend* sb, TreeContext* ctx,
             bin_gc_type_t type, int64_t context);

/* Consumer side: called by the GC thread. */
int bin_pop(StorageBackend* sb, TreeContext* ctx,
            BinEntry* out_entry);

/* Peek (optional): for the GC to look at the next entry's
 * type without removing it (for scheduling decisions). */
int bin_peek(StorageBackend* sb, TreeContext* ctx,
             BinEntry* out_entry);
```

`bin_push` and `bin_pop` must be crash-safe (see below).

### GC thread loop

A **single** background thread. The "intelligent scheduling"
the user described is a simple "try then back off" pattern:

```c
static void* gc_thread_main(void* arg) {
    TreeContext* ctx = (TreeContext*)arg;
    while (!ctx->gc_shutdown) {
        BinEntry entry;
        if (bin_pop(ctx->sb, ctx, &entry) == VFS_OK) {
            /* Got a job — process it immediately, no sleep. */
            gc_process_entry(ctx, &entry);
        } else {
            /* Bin is empty — back off for a bit, then check again. */
            usleep(GC_BACKOFF_US);  /* e.g., 100ms initially, adaptive later */
        }
    }
    return NULL;
}
```

The "intelligent" part is that **if there's work, we don't
sleep** — we process it as fast as the Bin delivers entries.
If the Bin is empty, we sleep for `GC_BACKOFF_US` and check
again. The backoff could be adaptive (exponential on continued
empty, reset on first non-empty) but the spec can defer that.

**Crash safety of the loop:** the thread can be killed at any
point. On next process start, a new thread is spawned. The
Bin is persistent, so no work is lost.

### Refinement: trigger/work split

The user refined the design: **garbage producers should push
ONE GENERIC ENTRY per operation**, not detailed per-type
entries. The GC then does an **analytical step** that walks
the chain, identifies the specific smaller garbage items, and
pushes them as work entries. The original generic entry is
deleted after the analysis. The smaller work entries are then
processed in subsequent iterations.

This has two benefits:
1. **Producers stay simple.** They don't need to know the
   chain structure or what specific garbage the operation
   produces. They just say "this thing happened" (e.g.,
   "file F was deleted", "epoch E was committed").
2. **The GC is the only one that knows the chain structure.**
   Changes to the chain (new node types, new garbage types)
   only affect the GC's analytical step, not the producers.

**Two-tier BinEntry type system:**

```c
/* TRIGGER types — what producers push.  Generic, contextual,
 * requires analysis before work. */
typedef enum {
    BIN_TRIGGER_FILE_DELETED,        /* context = file VP */
    BIN_TRIGGER_EPOCH_COMMITTED,     /* context = snapshot epoch */
    BIN_TRIGGER_EPOCH_SOFT_DELETED,  /* context = snapshot epoch */
    BIN_TRIGGER_FILE_TRUNCATED,      /* context = file VP + new size */
    BIN_TRIGGER_TOMBSTONE_ADDED,     /* context = tombstone slot VP */
} bin_trigger_type_t;

/* WORK types — what the GC's analysis pushes.  Specific, direct,
 * no further analysis required. */
typedef enum {
    BIN_WORK_REMOVE_TOMBSTONE,       /* context = tombstone slot VP */
    BIN_WORK_DROP_SOFT_DELETE,       /* context = mapper slot VP */
    BIN_WORK_REWRITE_CHAIN_ENTRY,    /* context = chain slot VP, from-epoch, to-epoch */
    BIN_WORK_FREE_PAGES,             /* context = head of pages list (or sentinel) */
} bin_work_type_t;

typedef struct {
    uint8_t  type;       /* bin_trigger_type_t OR bin_work_type_t (tagged) */
    uint8_t  flags;      /* reserved for future use */
    int64_t  context;    /* type-specific */
    int64_t  context2;   /* second context field (e.g., to-epoch) */
} BinEntry;
```

**Revised GC thread loop with trigger/work split:**

```c
static void* gc_thread_main(void* arg) {
    TreeContext* ctx = (TreeContext*)arg;
    while (!ctx->gc_shutdown) {
        BinEntry entry;
        if (bin_pop(ctx->sb, ctx, &entry) != VFS_OK) {
            usleep(GC_BACKOFF_US);
            continue;
        }
        if (is_trigger_type(entry.type)) {
            /* TRIGGER: do analytical step, push WORK entries, delete
             * the trigger.  The trigger is deleted in the SAME pass
             * that pushes the work entries (atomic from the consumer's
             * view).  If we crash mid-analysis, the trigger is still
             * there and we re-analyze next time — the analysis is
             * idempotent. */
            gc_analyze(ctx, &entry);
            /* gc_analyze: walks the chain, pushes WORK entries for
             * each garbage item found, then deletes the trigger entry. */
        } else {
            /* WORK: do the work directly. */
            gc_do_work(ctx, &entry);
        }
    }
    return NULL;
}
```

**Idempotency of the analysis step:**

The analysis is idempotent in the strong sense: re-running it
on the same trigger context produces the same set of WORK
entries (with the same contexts). The push is idempotent
(pushing a duplicate WORK entry is a no-op via a per-entry
deduplication check, e.g., a set of in-flight WORK contexts).

The trigger deletion is idempotent: deleting an already-deleted
trigger is a no-op.

So the worst-case crash behavior is: a partial analysis
left some WORK entries in the Bin. The next run re-analyzes
the trigger (which re-pushes the missing WORK entries and
re-asserts the existing ones), then deletes the trigger.
The WORK entries are then processed in subsequent iterations.

**Per-trigger analysis:**

| Trigger | Analysis |
|---------|----------|
| `BIN_TRIGGER_FILE_DELETED` | Walk FileNode → FileContent → PageNode → VersionPage chains. For each VersionPage at an epoch not in any live snapshot, the data page is dead. For each dead data page, push `BIN_WORK_FREE_PAGES` (or a batched version). For each chain page (FileContent, PageNode, VersionPage, FileSize, FileMTime) that is fully dead, also push `BIN_WORK_FREE_PAGES`. The tombstone itself is a `BIN_WORK_REMOVE_TOMBSTONE`. |
| `BIN_TRIGGER_EPOCH_COMMITTED` | Walk every VersionPage chain. For each entry at the committed epoch, push `BIN_WORK_REWRITE_CHAIN_ENTRY` (from-epoch = committed, to-epoch = currentEpoch). After all chains walked, push `BIN_WORK_DROP_SOFT_DELETE`-equivalent for the committed mapper entry (or a new type `BIN_WORK_DROP_COMMITTED_MAPPER`). |
| `BIN_TRIGGER_EPOCH_SOFT_DELETED` | Walk every VersionPage chain. For each entry at the soft-deleted epoch, mark its data page as dead (if no other live epoch references it). Push `BIN_WORK_FREE_PAGES` for the dead data pages. After all chains walked, push `BIN_WORK_DROP_SOFT_DELETE` for the mapper entry. |
| `BIN_TRIGGER_FILE_TRUNCATED` | Walk the FileSize chain to find the truncation point. For data pages past the new size that aren't referenced by any live FileContent segment, push `BIN_WORK_FREE_PAGES`. |
| `BIN_TRIGGER_TOMBSTONE_ADDED` | Walk the parent dir's SlotNode chain to find the SlotNode containing the tombstone. Push `BIN_WORK_REMOVE_TOMBSTONE` for the tombstone. |

### `gc_process_entry` — the per-type work functions

For each garbage type, the GC implements a "do work" function:

```c
static int gc_process_entry(TreeContext* ctx, const BinEntry* entry) {
    switch (entry->type) {
    case BIN_GC_TYPE_FREE_PAGES:
        return gc_process_type1_free_pages(ctx, entry->context);
    case BIN_GC_TYPE_TOMBSTONE:
        return gc_process_type2_tombstone(ctx, entry->context);
    case BIN_GC_TYPE_SOFT_DELETE:
        return gc_process_type3_soft_delete(ctx, entry->context);
    case BIN_GC_TYPE_COMMITTED_REWRITE:
        return gc_process_type4_committed_rewrite(ctx, entry->context);
    default:
        return VFS_ERR_IO;  /* unknown type, log + skip */
    }
}
```

Each `gc_process_typeN` function is **idempotent** — running
it twice on the same context is safe (and equivalent to
running it once). This is critical for crash safety (see
below).

### Per-path `bin_push` calls

With the **trigger/work split** (the refined design above),
each garbage-producing operation pushes **exactly ONE generic
TRIGGER entry**. The GC's analytical step then pushes the
specific WORK entries. This means:

- **Producers don't need to know the chain structure or what
  specific garbage the operation produces.** They just say
  "this thing happened" (e.g., "file F was deleted", "epoch
  E was committed"). The GC is the only one that knows.

| Operation | Bin push | Trigger type |
|-----------|----------|--------------|
| `vfs_delete` | 1× `BIN_TRIGGER_FILE_DELETED` | context = deleted file VP |
| `vfs_truncate` (shrink) | 1× `BIN_TRIGGER_FILE_TRUNCATED` | context = file VP, context2 = new size |
| `vfs_delete_snapshot` / `vfs_rollback` | 1× `BIN_TRIGGER_EPOCH_SOFT_DELETED` | context = snapshot epoch |
| `vfs_rename` | 1× `BIN_TRIGGER_TOMBSTONE_ADDED` | context = tombstone slot VP |
| `vfs_commit` | 1× `BIN_TRIGGER_EPOCH_COMMITTED` | context = snapshot epoch |
| `mirror_write` failure (Path 1) | (optional) — could push `BIN_TRIGGER_*` or do synchronously | The failure path is rare; can be done synchronously in `mirror_write` itself |

**Push semantics:** `bin_push` is called from the user-facing
operation. The push is **durable** before the operation
returns (the Bin is part of the crash-safe VFS file). The
operation's other side effects (data writes, mapper updates)
are also durable. On crash recovery, the trigger entry is
still there, the GC re-analyzes it (idempotent), and the
resulting WORK entries are processed in subsequent iterations.

**Why one entry per operation (not multiple):**
- **Simplicity:** producers don't need to enumerate the
  specific garbage they produce
- **Centralization:** the GC's analysis is the single place
  that knows the chain structure. If the chain structure
  changes (new node types, new garbage types), only the
  GC's analysis code needs to change
- **Batch efficiency:** the GC can process a big chunk
  (e.g., all the dead pages of a deleted file) in one
  pass, rather than dribbling them out as separate Bin
  entries

**What the GC's analysis produces for each trigger:**

| Trigger | WORK entries pushed |
|---------|---------------------|
| `BIN_TRIGGER_FILE_DELETED` | One `BIN_WORK_FREE_PAGES` per dead data page, one `BIN_WORK_FREE_PAGES` per dead chain page, one `BIN_WORK_REMOVE_TOMBSTONE` for the file's tombstone |
| `BIN_TRIGGER_FILE_TRUNCATED` | One `BIN_WORK_FREE_PAGES` per data page past the new size (batch if many) |
| `BIN_TRIGGER_EPOCH_SOFT_DELETED` | One `BIN_WORK_FREE_PAGES` per snap-only data page, one `BIN_WORK_DROP_SOFT_DELETE` for the mapper entry |
| `BIN_TRIGGER_TOMBSTONE_ADDED` | One `BIN_WORK_REMOVE_TOMBSTONE` for the tombstone |
| `BIN_TRIGGER_EPOCH_COMMITTED` | One `BIN_WORK_REWRITE_CHAIN_ENTRY` per S-epoch VersionPage, one `BIN_WORK_DROP_COMMITTED_MAPPER` for the committed mapper entry |

### Producer / GC separation of concerns

The trigger/work split gives a clean separation:

- **Producers** know: "I just did X (deleted a file, committed
  a snapshot, etc.)". They push one trigger entry to tell
  the GC about it.
- **GC** knows: the chain structure, what constitutes live
  vs dead data, how to identify the specific garbage items,
  how to do the work safely. It converts triggers into
  work entries and processes them.

This means producers don't need to be updated when the
chain structure changes. And the GC's analysis code is
the single place that needs to know the chain structure.

### GC work semantics (per type)

#### Type 1 (free pages, context = file VP / file VP+size / snapshot epoch)

The GC walks from the context to identify dead pages:
- **File VP**: walk FileNode → FileContent → PageNode →
  VersionPage chains. For each VersionPage at an epoch not in
  any live snapshot, the data page is dead. For each dead
  data page, enqueue to the deferred-free queue (Type 1
  reclamation = Path 3).
- **File VP + new size**: walk FileSize chain to find the
  truncation point. Pages past the new size (not in any
  FileContent segment beyond the truncation) are dead. Same
  deferred-free enqueue.
- **Snapshot epoch**: walk all chains looking for entries at
  this epoch (which is now soft-deleted). Their data pages
  are dead if no other live epoch references them.

After enqueuing the dead pages, the GC also frees the chain
pages themselves (FileContent, PageNode, VersionPage,
FileSize, FileMTime) and the FileNode. These are
**structurally dead** — the file is no longer in any
directory — so they can be freed directly (not via the
deferred-free queue).

#### Type 2 (tombstone, context = tombstone slot VP)

The GC walks the parent dir's SlotNode chain to find the
SlotNode containing the tombstone. It removes the tombstone
from the chain (compacts the chain to remove the now-empty
slot). The slot itself is then freed (deferred-free or direct).

#### Type 3 (soft-delete, context = snapshot epoch)

The GC walks the mapper chain to find the soft-delete entry
for the epoch. It removes the entry from the chain
(redirects any chain links to skip it). The freed slot goes
to the deferred-free queue.

#### Type 4 (committed, context = snapshot epoch)

The GC walks all VersionPage chains. For each chain entry at
the committed snapshot epoch, it rewrites the entry to the
current epoch (the toEpoch from the mapper). After all
chains are walked, the GC walks the mapper chain to find
the committed entry and removes it (the chains are now
self-sufficient at the toEpoch).

### Crash safety

The Bin design must be crash-safe. Three failure modes to
consider:

1. **Crash before `bin_push` commits but after the operation's
   data write commits.** The data is on disk; the Bin entry
   is not. On recovery, the data is dead but the GC doesn't
   know. **Mitigation:** `bin_push` must be ordered BEFORE
   the operation's other side effects are made durable
   (similar to M5's pool-page flush before the superblock
   write). The Bin entry is the "marker" for the data's
   cleanup state.

2. **Crash mid-`bin_pop` (between reading the entry and
   removing it from the Bin).** The entry is still in the Bin.
   On recovery, the GC pops the same entry again. The
   `gc_process_typeN` function must be **idempotent** — re-doing
   the work is safe and equivalent.

3. **Crash mid-`gc_process_entry` (work in flight, Bin entry
   still present).** Same as case 2 — the entry is still in
   the Bin, the work is partial, the next run re-does the
   work. Idempotency is essential.

**Idempotency requirements per type:**

- **Type 1**: enqueuing a page to the deferred-free queue is
  idempotent (storage_free is a no-op for already-free pages).
  Re-walking the chain to find dead pages is idempotent (the
  dead pages are still dead). ✓ Trivially idempotent.
- **Type 2**: removing a tombstone from a chain is idempotent
  (a missing tombstone is a no-op). But the chain link
  rewriting must be careful — re-removing an already-removed
  entry is safe (chain length just goes down further). ✓
  Idempotent.
- **Type 3**: removing a soft-delete mapper entry is
  idempotent. ✓
- **Type 4**: rewriting a chain entry from S to E is
  idempotent (re-writing an already-E entry is a no-op).
  Removing the committed mapper entry is idempotent. ✓

All 4 types are idempotent. Crash safety is achievable
without complex recovery logic.

### Concurrency model

**Lock pattern: same as the free-page queue** (per-entry CAS,
no global lock). Each BinEntry has a state field, head/tail
are atomic pointers, the per-Bin-page count is updated via
`vfs_cas_i32`. This is the same pattern we already debugged
in Phase 27 W6 (per-page CAS, no lock during the consumer's
work). The user confirmed this is the desired pattern.

- **Producers (user-facing operations)**: single-threaded
  per VFS instance, but multiple operations can be in flight
  concurrently (FUSE worker threads). `bin_push` is
  thread-safe via per-entry CAS — no global lock.
- **Consumer (GC thread)**: single thread. Reads the Bin,
  processes entries, removes from Bin via per-entry CAS.
- **Cross-thread**: the producer and consumer share the Bin
  pages. The CAS is the synchronization primitive. The GC
  thread's work functions don't need the Bin lock (they
  work on the tree, not the Bin).

### Queue architecture: 2 persistent queues + per-section locks

**Updated design (after user feedback):** the new GC does
NOT use the existing deferred-free queue (Phase 7). The old
GC is being dropped completely. The new design uses **2
persistent queues** (Bin + free-page) and **per-section locks**
for the "safe to free" gate. The deferred-free queue was a
mechanism designed for the old GC's long-running tree sweep;
the new GC's per-entry processing (microseconds to milliseconds)
doesn't need a separate "wait for readers globally" mechanism.

```
┌────────────────┐
│  user-facing   │  vfs_delete, vfs_commit, etc.
│  operation     │
└───────┬────────┘
        │  bin_push(TRIGGER, context)
        ▼
┌────────────────┐
│      Bin       │  Persistent queue in the VFS file.
│   (Phase 28)   │  Generic TRIGGER entries + specific
│                │  WORK entries (post-analysis).
└───────┬────────┘
        │  GC thread: pop entry, take per-section lock,
        │  do work (analysis or work function), release lock
        ▼
┌────────────────┐
│  GC's work     │  Identify dead pages, walk chains, etc.
│  (in-memory)   │  Idempotent; runs in GC thread. Holds
│                │  per-section lock during processing.
└───────┬────────┘
        │  Dead pages: storage_free (per-page under lock)
        ▼
┌────────────────┐
│ Free-Page Queue│  Persistent queue in VFS header
│   (Phase 27)   │  (offsets 40/48/56). Pages available
│                │  for re-allocation.
└───────┬────────┘
        │  storage_allocate(1) calls dequeue_from_free_list
        ▼
┌────────────────┐
│  re-allocated  │  The page is now in use.
│   page         │
└────────────────┘
```

**Each queue / mechanism's scope:**

| Queue / mechanism | Lives in | Producer | Consumer | State |
|---|---|---|---|---|
| Bin | VFS file (header + pages) | User ops (push trigger), GC analysis (push work) | GC thread (pop) | Persistent |
| Per-section lock | In-memory spinlock | GC thread (during Bin entry processing) | GC thread (releases on completion) | In-memory |
| Free-page | VFS file (header offsets 40/48/56) | `storage_free` (called from inside per-section lock) | `dequeue_from_free_list` (called by `storage_allocate(1)`) | Persistent |

**Why per-section locks replace the deferred-free queue:**

The old GC's deferred-free queue existed because the old GC did
a **long-running tree sweep** (the "shadow compaction" — copy
all live entries to new pool pages). That long sweep needed a
"wait for all in-flight readers globally" mechanism before
freeing any pages. The deferred-free queue was that mechanism.

The new GC is **fundamentally different**:
- It processes one Bin entry at a time
- Each entry's processing is short (microseconds to milliseconds)
- It only needs to exclude readers for the **affected section**,
  not globally

### Locking is per-bin-job, not a global decision

The user clarified: the locking mechanism is **per-bin-job**,
not a global prescription. Each bin job decides what lock
(or no lock, or CAS) is needed based on what it does. The GC
has flexibility in how to do each job — e.g., it can copy the
entire file to a new place and free the original, or just
rewrite in place, depending on the job's strategy.

The spec for each bin job should describe:
- What lock (or no lock) the job takes
- For how long
- What state the lock guarantees

The lock is a per-job implementation detail, not a global
concurrency model. The Phase 28 spec's per-bin-job sections
will spell out each job's lock.

### The page free specifically does NOT need a lock

The user pointed out: "if a page is 'free' doesn't it mean
there are no more readers for it?" — this is correct for the
free specifically. The synchronization that excludes readers
is already in place:

- **File delete** (`vfs_delete`): takes the file lock. The
  lock serializes the delete with other operations on the
  file. After `vfs_delete` returns, the file is "deleted" from
  the system's perspective. Any operation that would read the
  file's data goes through the same lock (or sees the deleted
  state). So no concurrent reader of the data pages exists
  after the delete.

- **Truncation** (`vfs_truncate`): the pages past the new
  size are orphan. BUT — if any active snapshot still has a
  FileSize entry at the pre-truncate size, the past-size
  pages are still referenced by that snapshot's FileContent
  chain. **They are NOT free.** The GC's analysis must check
  this before freeing. The lock (if any) is for the analysis,
  not for the free.

- **Commit** (`vfs_commit`): inserts a mapper entry and
  rewrites the chain. The chain rewriting is a chain
  modification, not a page free. The lock (if any) is for
  the chain modification.

- **Snapshot delete** (`vfs_delete_snapshot`): after the
  mapper's soft-delete, no reader sees the snapshot's data
  anymore. The pages are truly orphan. The lock is for the
  chain modification (dropping the soft-delete mapper entry);
  the free itself needs no lock.

- **Tombstone** (`vfs_rename`): the tombstone is a chain
  modification (a new entry in the parent dir's chain). No
  page free. The lock (if any) is for the chain modification.

**Summary:** for all 5 garbage producers, the lock (if any)
is for **chain modification**, not for the page free. The
page free specifically does not need a lock because the
synchronization that excludes readers is already in place
(file lock for delete, soft-delete for snapshot, analysis
correctness for truncate).

**The exception — chain modification mid-walk:**

For commit's chain rewriting, the GC walks all chains to
rewrite S→E. A reader at S also walks the chain (to apply
the same rewrite on the read path). If the GC and the reader
walk the same chain concurrently, the reader might see
half-rewritten entries. This is a **chain walk race**, not
a "reader has a pointer to the page" race. The fix is
either:
- A lock on the chain (held by GC during the walk; reader
  blocks briefly)
- A CAS-based rewrite (each entry is atomically rewritten
  via CAS; reader sees either old or new state)

The lock (or CAS) is for the chain walk, not for the page
free.

### Lock model summary (per-bin-job decision)

The Phase 28 spec for each bin job specifies its own lock
(or no lock, or CAS). Common patterns:

- **File delete**: takes the existing `vfs_lock` for the
  file (same lock that `vfs_delete` already takes). The lock
  is held for the duration of the GC's analysis. The free
  itself is unlocked (no readers exist after the lock is
  released).

- **Snapshot delete**: takes a mapper lock (existing or
  to-be-added) for the duration of the analysis. The mapper
  write (soft-delete drop) is a brief critical section. The
  free of the orphan pages is unlocked (no readers after the
  soft-delete is dropped).

- **Truncation**: the analysis walks the FileSize chain
  to identify which pages are truly free. May take a file
  lock (if it needs to atomically read+decide). The free is
  unlocked for the truly-orphan pages.

- **Commit**: the chain walk is the lock-holding part. May
  use a per-chain lock or CAS. The free is unlocked (the
  pages being freed are the ones in the rewritten chain,
  which are reachable only via the committed epoch's reads
  — the reads walk the chain and rewrite, so the page is
  still reachable through the rewrite).

- **Tombstone**: a chain modification. May use a per-dir
  lock or CAS. No free involved.

### The GC has flexibility in implementation strategy

For each bin job, the GC can choose how to do the work:
- **In-place rewrite**: modify the existing chain entry
  (uses CAS for atomicity)
- **Copy + replace**: copy the chain to a new place, replace
  the old chain, free the old chain pages (uses a lock for
  the duration of the copy)
- **Lazy**: leave the chain as-is, just mark the old entries
  as garbage for later cleanup

The choice depends on the bin job's specifics. The spec
doesn't prescribe a global strategy — each bin job's spec
section describes the chosen strategy and its lock model.

**The lock is NOT the deferred-free queue.** The deferred-free
queue was a global mechanism that waited for all readers before
any free. The new lock is a per-section mechanism that excludes
readers for the brief duration of the GC's processing of one Bin
entry. The lock serves the same purpose (exclude readers
during free) but is localized to the affected section.

**The lock is similar to the existing `vfs_lock` (per-file,
per-page-node).** The codebase already has per-resource locks;
we extend the pattern to GC processing. The new lock types
might include per-mapper and per-dir-section locks (TBD in
the spec).

**What about "the existing deferred-free queue" in `src/gc.c`?**

The user noted: "the existing GC will basically be dropped
completely." This means:
- The old `gc.c` code (Phase 7) is **not carried forward** to
  Phase 28
- The `DeferredFreeQueue` struct, the deferred-free functions,
  the tree lock, and the shadow-compaction logic are all
  being replaced
- The new design implements its own concurrency model
  (per-section locks) and its own free mechanism
  (direct `storage_free` under the per-section lock)

The deferred-free queue **concept** (wait for readers before
freeing) is preserved in a different form: the per-section
lock serves the same purpose, just localized to the affected
section.

### Bin storage in the VFS file (concrete proposal)

Following the free-page queue's pattern, the Bin uses 3 new
header fields:

| Header offset | Field | Purpose |
|---|---|---|
| `HDR_OFF_BIN_HEAD` (TBD) | `int64_t bin_head` | VirtualPtr of the head Bin page (0 if empty) |
| `HDR_OFF_BIN_TAIL` (TBD) | `int64_t bin_tail` | VirtualPtr of the tail Bin page (0 if empty) |
| `HDR_OFF_BIN_COUNT` (TBD) | `int64_t bin_count` | Total number of entries across all Bin pages |

**Effect on inline indirection:** the inline count drops by
3 (same as W1 did for the free-page queue). For 8KB pages,
inline count goes from 1019 (post-W1) to 1016.

**Bin page layout:**

A Bin page is a regular storage page (in the indirection) with
a custom layout:

```
offset 0:    int64_t next_bin_page    (next Bin page in chain, 0 = end)
offset 8:    int32_t count            (entries currently in this page)
offset 12:   int32_t capacity         (max entries in this Bin page)
offset 16:   BinEntry[capacity]      (entry array)
```

**Bin page allocation:** when the Bin is full, `bin_push`
allocates a new Bin page via `storage_allocate`, links it to
the tail (CAS-update the old tail's `next_bin_page` field),
and updates the Bin's `bin_tail` pointer.

**Bin page deallocation:** when the last entry is popped from a
Bin page, the GC frees the page via `storage_free` and updates
the Bin's `bin_head` pointer (CAS-advance to the next page).

**CAS pattern (matches free-page queue):**

- `bin_push`:
  1. Read `bin_tail` (atomic load)
  2. Read the tail Bin page's `count` (atomic load)
  3. If `count < capacity`: write the entry at position
     `[count]`, CAS-increment `count` from N to N+1
  4. If `count == capacity` (page full): allocate a new Bin
     page, CAS-link it, retry from step 1

- `bin_pop`:
  1. Read `bin_head` (atomic load)
  2. If `head == 0`: return EMPTY
  3. Read the head Bin page's `count` (atomic load)
  4. If `count > 0`: read the entry at position `[count-1]`,
     CAS-decrement `count` from N to N-1
  5. If `count == 0` after CAS: CAS-advance `bin_head` to
     `next_bin_page`, free the old head page
  6. Return the entry

This is **the same pattern as the free-page queue's
`enqueue_free_page` / `dequeue_from_free_list`**, just
adapted for the BinEntry layout.

### Trigger and lifecycle

- **When does the GC thread start?** At `vfs_mount` (or at
  first garbage-producing operation). The thread runs for
  the lifetime of the VFS instance.
- **When does it stop?** At `vfs_unmount` (after all pending
  work is processed, or after a timeout). The Bin is
  persistent, so unprocessed work survives a graceful
  unmount.
- **Manual trigger?** `vfs_gc(vfs)` could be added for
  synchronous GC (the existing API). The background thread
  is the asynchronous version.

### Spec organization (proposed workloads)

The Phase 28 spec should describe this design and break it
into ~5 workloads:

1. **W1: Bin infrastructure** — Bin storage in the VFS file,
   `bin_push` / `bin_pop` / `bin_peek` API, thread-safe
   operations.
2. **W2: GC thread loop** — the background thread that
   pops entries and calls `gc_process_entry`. Basic
   scheduling (try-then-backoff).
3. **W3: Type 1 processing (free pages)** — the most
   complex per-type work. Walks chains, identifies dead
   pages, enqueues to deferred-free queue, frees chain
   pages.
4. **W4: Type 2/3/4 processing** — the simpler per-type
   work. Removes tombstones, drops soft-deletes, rewrites
   committed chains.
5. **W5: Integration** — wire the producers (`vfs_delete`,
   `vfs_truncate`, `vfs_delete_snapshot`, `vfs_rename`,
   `vfs_commit`) to call `bin_push`. End-to-end test of
   the background GC against the existing GC tests.

### Open questions remaining

After the Bin design, the original 7 open design questions
become:

1. **Bin storage option (A/B/C)** — my recommendation: (A)
2. **N2 (pinPage=true for GC)** — still blocks all type work
3. **M4/N3 (gc_copy_entry dispatch)** — still blocks all type work
4. **GC thread trigger model** — background at mount (per
   the design)
5. **GC interaction with snapshot model** — the Bin design
   abstracts this; the GC processes whatever the Bin
   contains
6. **Idempotency per type** — analyzed above, all 4 are
   idempotent
7. **deferred_free OOM fallback** — the Bin can be backed
   up to the deferred-free queue; if the queue is OOM, the
   GC can fall back to direct `storage_free` (since the
   tree exclusive lock is held during GC processing)

---

## Open design questions for the Phase 28 spec

**Note:** the original 7 open design questions from the
inventory stage are now superseded by the Bin design above
(see "Open questions remaining" in the Proposed GC Design
section). The key remaining questions after the Bin design:

1. **N2 (pinPage=true for GC)** — still blocks all type work.
   Must be fixed in W3 of the Phase 28 implementation.
2. **M4/N3 (gc_copy_entry dispatch)** — still blocks all
   type work. Must be fixed in W3.
3. **Bin storage option** — my recommendation is (A) (header
   fields + Bin pages in indirection), but the spec should
   compare with (B) and (C) and pick.
4. **GC thread trigger** — start at mount, stop at unmount.
   The Bin handles ordering of garbage vs. data writes.
5. **`bin_push` ordering** — the Bin push must be ordered
   BEFORE the operation's other side effects are made
   durable, so a crash doesn't leave data on disk without a
   Bin entry to clean it up. The exact ordering depends on
   the per-operation durability points (M5-style).

The original detailed 7 questions are captured in the
discussion above.
