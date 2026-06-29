# Phase 6: Epoch System

## Goal
Implement snapshots, commit with conflict detection, soft-delete, and the
epoch mapper. Epochs are the core mechanism for snapshot isolation — taking
a snapshot costs nothing but an atomic increment, and the mapper controls
visibility of committed and deleted snapshots during reads.

---

## Workload 6.2 — Snapshot
## Workload 6.1 — Valid Epochs

**What:** Enforce write restrictions on epochs. Only the current live head
(highest even epoch) and active (uncommitted, undeleted) snapshots are
writable. All other epochs are read-only.

**Why:** Historical epochs are immutable — they represent committed or frozen
state. Allowing writes to them would corrupt snapshot data and break the
version chain model.

**How:**
- Track which epochs are writable. The current live head (the highest even
  epoch from the superblock) is always writable. Active snapshots are odd
  epochs that have neither a commit mapping (traversalApply=true) nor a
  soft-delete mapping (traversalApply=false) in the epoch mapper.
- Before any `vfs_write` or directory mutation at a given epoch, validate:
  - If `epoch` is the current live head: allowed.
  - If `epoch` is odd AND not in the mapper: allowed (active snapshot).
  - All other epochs: rejected. Return `VFS_ERR_IO` or a dedicated error.
- Reads have no epoch restriction — any epoch is valid for reading.
- This check runs once per API call, early in the function, before any
  pool allocations or chain walks.

**Acceptance:**
  - Write to current live head (epoch 0): succeeds.
  - Take snapshot (now live head=2, snapshot=1). Write to epoch 1: succeeds.
  - Commit epoch 1. Write to epoch 1 again: rejected (committed).
  - Soft-delete epoch 3. Write to epoch 3: rejected.
  - Write to epoch 2 (an older even epoch, not the live head): rejected.
  - Read from any epoch: always succeeds.


**What:** Take a snapshot by incrementing the epoch counter. The new epoch
is odd (snapshot), the previous epoch becomes frozen live-head history.
Multiple active sibling snapshots are supported.

**Why:** Snapshot isolation is the defining feature of the VFS. A snapshot
must be instantaneous and cost no disk I/O — just an atomic increment of
an in-memory counter. This enables frequent, lightweight snapshots for
transactional safety, backups, and point-in-time queries.

**How:**
- `vfs_snapshot(vfs)`: atomically increment `currentEpoch` by 2 (e.g., from
  2 to 4). Return the new odd epoch (e.g., 3). The live head is now the new
  even epoch (e.g., 4). The returned odd epoch (3) is the snapshot.
- The caller can now write at either the live head (4) or the snapshot (3).
  Writes at the snapshot epoch are isolated — they do not affect live-head
  readers. Writes at the live head are visible to all new readers.
- The `currentEpoch` counter is purely in-memory during this operation.
  No disk write, no fsync — the epoch counter is persisted at the next Flush
  or when the superblock is updated during commit or GC.
- If the process crashes before any data is written at the new snapshot epoch,
  the epoch is lost. The on-disk superblock still has the old `currentEpoch`.
  On remount, the old epoch is reused. Any pool slot allocations for the lost
  epoch are zombies — unreachable, reclaimed by GC.
- Multiple snapshots can be taken before any of them are committed. Each
  snapshot advances the live head by 2. All active snapshots are siblings
  (direct children of successive live heads) — they never form a nested chain.
  You cannot snapshot a snapshot.

**Acceptance:**
  - `vfs_snapshot` returns an odd epoch (e.g., 3, 5, 7).
  - The live head advances by 2 after each snapshot call.
  - Two snapshots taken in sequence return different odd epochs.
  - After crash and remount before any flush, the epoch counter rolls back
    to the last persisted value. Lost epochs don't cause corruption.
  - Attempting to write to an even epoch other than the current live head
    is rejected (frozen history).

---

## Workload 6.3 — Commit

**What:** Commit a snapshot epoch to the live head. The snapshot's changes
become visible to all readers. If the same page was modified in both the
snapshot and any intervening live-head epoch, the commit is rejected with
a conflict.

**Why:** Commit is the mechanism for merging snapshot work back into the main
timeline. Conflict detection prevents lost updates when two concurrent writers
touch the same logical page.

**How:**
- `vfs_commit(vfs, snapshot_epoch)`:
  1. Validate that `snapshot_epoch` is an active snapshot (odd, has not been
     committed or soft-deleted).
  2. Walk the TouchedFile chain for `snapshot_epoch` from the superblock's
     `touchedFilesPtr`. Collect all unique file nodeIds modified in this
     snapshot.
  3. For each file, scan its version chains. For each logical page that has
     a VersionPage at `snapshot_epoch`, check whether the same logical page
     also has a VersionPage at ANY even epoch in the range `[snapshot_epoch+1,
     currentEpoch]`. If yes: conflict → return `VFS_ERR_CONFLICT`.
     (The range covers all live-head epochs that existed while the snapshot
     was active, including those created by sibling snapshots advancing the
     live head.)
  4. If no conflicts: add a MapperEntry `{fromEpoch = snapshot_epoch, toEpoch
     = snapshot_epoch + 1, traversalApply = true}` to the epoch mapper chain
     via CAS-prepend to `epochMapperPtr`.
  5. Drop the TouchedFile chain for this epoch (the entries are no longer
     needed — GC will reclaim their pool slots).
  6. Return `VFS_OK`. The snapshot data is now visible via the mapper: any
     read at the snapshot epoch is remapped to the live head epoch that
     existed at commit time.
- The commit does NOT mutate existing version nodes. Only the mapper is
  updated. This is O(modified files), not O(total data).

**Acceptance:**
  - Commit a clean snapshot (no conflicts): returns `VFS_OK`. Reading at the
    snapshot epoch now returns the committed data.
  - Modify page 0 in snapshot 3 and also in live head 4: commit 3 fails with
    `VFS_ERR_CONFLICT`.
  - Modify page 0 in snapshot 3, take snapshot 5 (sibling), modify page 0 in
    live head 6: commit 3 fails because page 0 was modified at epoch 6
    (which is in the live-head range [4, 6]).
  - After commit, the TouchedFile chain for the committed epoch is empty.
  - Attempting to commit an already-committed or soft-deleted epoch returns
    an error.

---

## Workload 6.4 — Soft-Delete Snapshot

**What:** Mark a snapshot as deleted without physically removing its data.
The snapshot's changes become invisible to all readers. GC later reclaims
the space.

**Why:** Snapshot data must be discardable without blocking writers. Soft-delete
is an O(1) operation — just add a mapper entry with `traversalApply = false`.
The heavy work (page reclamation, version node removal) is deferred to GC.

**How:**
- `vfs_delete_snapshot(vfs, snapshot_epoch)`:
  1. Validate that `snapshot_epoch` is an active snapshot.
  2. Add a MapperEntry `{fromEpoch = snapshot_epoch, toEpoch = snapshot_epoch
     - 1, traversalApply = false}` via CAS-prepend to `epochMapperPtr`.
  3. Drop the TouchedFile chain for this epoch.
  4. Return `VFS_OK`.
- Effect on reads: `mapper.resolve(snapshot_epoch)` returns `snapshot_epoch - 1`
  (the pre-snapshot base). Traversal encounters `traversalApply = false`, so
  entries with `epoch == snapshot_epoch` are NOT remapped — they keep their
  original epoch, which is odd, so the standard read rule skips them. The
  result is that the snapshot's changes are invisible.
- The version nodes and data pages from the soft-deleted epoch remain on disk
  until GC runs. This is intentional — the soft-delete is instant.

**Acceptance:**
  - Write data in snapshot 3. Soft-delete snapshot 3. Read at epoch 3 returns
    the pre-snapshot base data (epoch 2).
  - Write data in snapshot 3. Take snapshot 5. Write more data at epoch 5.
    Soft-delete snapshot 3. Read at epoch 5 still returns the epoch 5 data
    (soft-deleting sibling 3 does not affect sibling 5).
  - After soft-delete, the TouchedFile chain for the deleted epoch is empty.
  - GC run after soft-delete reclaims the version nodes and data pages from
    the deleted epoch.

---

## Workload 6.5 — Epoch Mapper

**What:** A pool-allocated chain of MapperEntry nodes that translates epoch
numbers during reads. Two operations: query resolution (always applies) and
traversal remapping (controlled by `traversalApply`).

**Why:** The mapper is the glue between snapshots and the read path. Without
it, committed snapshots would be invisible and soft-deleted snapshots would
still be visible. The single-hop constraint keeps resolution O(1).

**How:**
- Mapper chain rooted at `superblock.epochMapperPtr` (a VirtualPtr).
- Each MapperEntry (Phase 4, Workload 4.8) stores `fromEpoch`, `toEpoch`,
  `flags` (bit 0 = `traversalApply`), and a `nextPtr` chain link.
- `mapper_resolve(R)`: walk the chain. If an entry with `fromEpoch == R`
  exists, return `toEpoch`. Otherwise return `R`. This is single-hop — no
  following chains of mappings.
- Traversal remapping (applied during chain walking in the read rule):
  - `traversalApply = true` (commit): entries with `epoch == fromEpoch` are
    treated as if their epoch were `toEpoch` before applying the standard
    read rules. This makes committed snapshot data visible.
  - `traversalApply = false` (soft-delete): entries are NOT remapped. They
    keep their original epoch and the standard read rule handles them (odd
    epochs are skipped). This hides soft-deleted snapshot data.
- Single-hop invariant: when inserting a mapping, verify that neither
  `fromEpoch` nor `toEpoch` already appears in another mapping. If a chain
  would form, reject the insert. In practice, commit only creates entries for
  active snapshots that are not yet mapped, and soft-delete does the same.
  GC consolidates overlapping mappings.
- GC compacts the mapper chain: committed mappings are dropped (their version
  nodes have been relabeled), soft-deleted mappings remain until GC physically
  removes the deleted data.

**Acceptance:**
  - `mapper_resolve(1)` with no entries returns 1.
  - After commit(1): `mapper_resolve(1)` returns 2.
  - After soft-delete(3): `mapper_resolve(3)` returns 2.
  - Attempt to insert a mapping where `fromEpoch` already exists: rejected.
  - After GC of a committed epoch: the mapping is removed; `mapper_resolve`
    returns the epoch itself (the data has been relabeled, so this is correct).
  - Mapper chain survives crash and remount: entries written before the crash
    are still present and functional.

---

## Workload 6.6 — TouchedFile Tracking

**What:** Maintain a per-epoch list of files that were modified in that
snapshot. Used exclusively by commit for conflict detection.

**Why:** Without a persisted list of modified files, a crash between a
snapshot write and its commit would require a full tree scan at commit time.
The TouchedFile chain makes commit O(files modified in snapshot), not
O(all files in tree).

**How:**
- TouchedFile chain per epoch, rooted at the superblock's `touchedFilesPtr`
  for that epoch. Currently the spec defines a single `touchedFilesPtr` field
  in the superblock — for multiple active snapshots, the chain includes an
  `epoch` field in each TouchedFile entry, so a single chain can hold entries
  for multiple epochs. Entries are filtered by epoch during commit.
- On first VersionPage write for a file in a given epoch: check whether a
  TouchedFile entry already exists for this `(epoch, nodeId)`. If not,
  CAS-prepend one.
- Deduplication: before prepending, walk the chain and check for an existing
  entry with the same `epoch` and `nodeId`. This is a short scan — the number
  of files modified per epoch is typically small.
- During commit: walk the chain, filter entries by the target snapshot epoch,
  collect unique nodeIds, and scan each file's version chains.
- After commit or soft-delete: drop the TouchedFile entries for that epoch
  (they are no longer needed). GC reclaims the pool slots.
- If the process crashes, the TouchedFile entries survive because they are
  pool-allocated and persist in dirty pages. On remount, the chain is
  still present and commit can proceed without a full tree scan.

**Acceptance:**
  - Modify file A at epoch 3: one TouchedFile entry {epoch=3, nodeId=A}.
  - Modify file A again at epoch 3: no duplicate entry.
  - Modify file B at epoch 3: second entry {epoch=3, nodeId=B}.
  - Commit epoch 3: both A and B are scanned for conflicts.
  - After commit: the TouchedFile entries for epoch 3 are dropped.
  - Crash after write, before commit: on remount, the TouchedFile entries
    for epoch 3 are still present and commit can proceed.

---

## Deliverables

| File | Purpose |
|------|---------|
| `src/epoch.c` | Snapshot, commit, soft-delete, epoch counter management |
| `src/mapper.c` | Epoch mapper chain: insert, resolve, GC compact |
| `src/touched.c` | TouchedFile chain: add, dedup, collect for commit |
| `test/test_epoch.c` | Snapshot lifecycle, commit with/without conflict, soft-delete visibility |

## Success Criteria
- Snapshot creates a new odd epoch instantly with no disk I/O.
- Commit succeeds when no conflicts exist and fails with `VFS_ERR_CONFLICT`
  when the same page was modified in both snapshot and live head.
- Commit with sibling snapshots correctly scans all intervening live-head epochs.
- Soft-deleted snapshot data is invisible to reads; pre-snapshot base data
  is returned instead.
- Mapper correctly resolves committed and soft-deleted epochs.
- TouchedFile deduplication prevents duplicate entries for the same file
  in the same epoch.
- Epoch state survives a crash when a Flush has occurred between snapshot
  and crash; lost epochs (no Flush) are harmless zombies reclaimed by GC.
