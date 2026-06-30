# Phase 6: Epoch System

## Goal
Implement snapshots, commit with conflict detection, soft-delete, and the
epoch mapper. Epochs are the core mechanism for snapshot isolation — taking
a snapshot costs nothing but an atomic increment, and the mapper controls
visibility of committed and deleted snapshots during reads.

## Non-Negotiable Constraints

- **Snapshot must be O(1).** No disk writes, no pool allocations, no tree
  walks. Just atomic increment of `currentEpoch`.
- **Commit checks all intervening live-head epochs.** With sibling snapshots,
  the live head advances multiple times. Commit must scan ALL even epochs
  between the snapshot and the current live head.
- **Mapper is single-hop.** No `fromEpoch → toEpoch → toEpoch2` chains.
  Enforced at insert time.
- **TouchedFile entries must be crash-safe.** They are pool-allocated and
  flushed to disk like all other metadata. A crash between write and commit
  must leave them intact so commit can proceed after remount.
- **Soft-delete is O(1).** Just add a MapperEntry. Defer reclamation to GC.

## File Organization

| File | Purpose |
|------|---------|
| `src/epoch.c` | Snapshot, Valid Epochs check, commit, soft-delete |
| `src/mapper.c` | Epoch mapper chain: insert, resolve, single-hop enforcement |
| `src/touched.c` | TouchedFile chain: add, dedup, collect for commit |
| `test/test_epoch.c` | Snapshot lifecycle, commit conflict, soft-delete visibility |

## Dependencies
- Phase 3 (Pool) for TouchedFile and MapperEntry slot allocation
- Phase 4 (Node Types) for layout helpers
- Phase 5 (Tree Operations) for reading superblock and version chain access

---

## Workload 6.1 — Valid Epochs

### What
A function that determines whether a given epoch is writable. Called at the
start of every mutation operation (vfs_write, vfs_create, vfs_delete,
vfs_mkdir, vfs_rmdir, vfs_rename).

### `vfs_epoch_is_writable(vfs, epoch) → bool`

```
1. If epoch == -1: use currentEpoch from superblock (live head).
2. If epoch == superblock->currentEpoch: return true (live head is always writable).
3. If epoch is odd:
     Walk mapper chain from epochMapperPtr.
     If a MapperEntry with fromEpoch == epoch exists:
         return false  (committed or soft-deleted → frozen)
     return true       (active snapshot)
4. If epoch is even and != currentEpoch:
     return false      (frozen history)
```

### Error Code
When this function returns false, the caller must set `VFS_ERR_IO` with a
descriptive message like "epoch is frozen" or "snapshot already committed."
Optionally add `VFS_ERR_EPOCH` as a dedicated error code in `vfs_error_t`.

### Acceptance
- [ ] Write to current live head (epoch 0): passes
- [ ] Take snapshot (live head=2, snapshot=1). Write to epoch 1: passes
- [ ] Commit epoch 1. Write to epoch 1 again: rejected
- [ ] Soft-delete epoch 3. Write to epoch 3: rejected
- [ ] Write to epoch 2 (older even, not live head): rejected
- [ ] Read from any epoch: always allowed (no check needed for reads)

---

## Workload 6.2 — Snapshot

### What
Take a snapshot. Returns a new odd epoch. Zero I/O.

### `vfs_snapshot(vfs) → new_snapshot_epoch`

```
1. Atomically increment superblock->currentEpoch by 2:
   old_epoch = atomic_add_i64(&superblock->currentEpoch, 2)
2. new_live_head = old_epoch + 2
3. snapshot_epoch = old_epoch + 1    // always odd
4. Return snapshot_epoch
```

### Key Semantics
- The live head is now `old_epoch + 2` (even).
- The returned epoch is `old_epoch + 1` (odd, the snapshot).
- The caller writes at either the live head or the snapshot epoch.
- Multiple snapshots can be taken before any are committed. All are siblings
  of successive live heads — never nested.
- The increment is purely in-memory. If the process crashes before any data
  is written at the new epoch, the epoch is lost — harmless zombie reclaimed
  by GC.
- The superblock's `currentEpoch` is NOT flushed to disk here. It is persisted
  the next time the superblock is flushed (during commit or GC or explicit
  vfs_flush).

### Acceptance
- [ ] `vfs_snapshot` returns an odd epoch (1, 3, 5, ...)
- [ ] Live head advances by 2 after each call
- [ ] Three snapshots taken in sequence: epochs 1, 3, 5 returned
- [ ] `kill -9` after snapshot, before any write → remount → `currentEpoch`
  rolls back to pre-snapshot value. No corruption.
- [ ] Zero disk I/O during snapshot (verify with strace/dtrace: no write syscalls)

---

## Workload 6.3 — Commit

### What
Commit a snapshot to the live head. The snapshot's data becomes visible to
all readers. If the same page was modified in both the snapshot and any
intervening live-head epoch, the commit is rejected with a conflict.

### `vfs_commit(vfs, snapshot_epoch) → VFS_OK or VFS_ERR_CONFLICT`

```
1. Validate that snapshot_epoch is an active snapshot:
   - Must be odd
   - Must NOT have a MapperEntry with fromEpoch == snapshot_epoch
2. Walk the TouchedFile chain rooted at superblock->touchedFilesPtr.
   Collect all entries where epoch == snapshot_epoch.
   Deduplicate by nodeId → get list of modified file nodeIds.
3. For EACH file in the modified list:
   a. Walk ALL version chains for that file (all PageNodes in all segments).
   b. For each logical page that has a VersionPage at snapshot_epoch:
        Check if same logical page has a VersionPage at ANY even epoch in
        the range [snapshot_epoch + 1, currentEpoch].
        If yes → CONFLICT → return VFS_ERR_CONFLICT immediately.
4. No conflicts. Add MapperEntry:
   mapper_insert(mapper, snapshot_epoch, snapshot_epoch + 1, true)
5. Drop the TouchedFile chain for snapshot_epoch (the entries are no longer
   needed — mark them for GC by removing them from the chain head, or just
   leave them; GC will reclaim).
6. Return VFS_OK.
```

### How the Conflict Scan Works

The range `[snapshot_epoch + 1, currentEpoch]` covers ALL live-head epochs
that existed while this snapshot was active. Example:
- Start: currentEpoch = 2 (live head)
- Snapshot 1 → currentEpoch = 4, snapshot = 3
- Snapshot 2 → currentEpoch = 6, snapshot = 5
- Commit epoch 3: scan live-head epochs [4, 6]. If page 0 was modified at
  epoch 4 AND at epoch 3 → conflict. If modified only at epoch 3 → clean.

### Acceptance
- [ ] Clean snapshot (no conflicting writes): commit returns VFS_OK
- [ ] Modify page 0 in snapshot 3 and live head 4: commit 3 → VFS_ERR_CONFLICT
- [ ] Modify page 0 in snapshot 3, take snapshot 5 (sibling), modify page 0 in
  live head 6: commit 3 → conflict (epoch 6 in range [4, 6])
- [ ] After commit: `mapper_resolve(snapshot_epoch)` returns `snapshot_epoch + 1`
- [ ] After commit: read at snapshot_epoch returns committed data
- [ ] Commit already-committed epoch → error

---

## Workload 6.4 — Soft-Delete Snapshot

### What
Mark a snapshot as deleted. O(1) — just adds a MapperEntry. GC reclaims
space later.

### `vfs_delete_snapshot(vfs, snapshot_epoch) → VFS_OK`

```
1. Validate that snapshot_epoch is an active snapshot (same check as commit step 1).
2. Add MapperEntry:
   mapper_insert(mapper, snapshot_epoch, snapshot_epoch - 1, false)
   (traversalApply = false)
3. Drop TouchedFile chain for this epoch.
4. Return VFS_OK.
```

### Effect on Reads
- `mapper_resolve(snapshot_epoch)` returns `snapshot_epoch - 1` (the pre-snapshot base).
- During chain walking: `traversalApply = false` means entries with
  `epoch == snapshot_epoch` are NOT remapped. They keep their original (odd)
  epoch. The standard read rule skips odd epochs — so the snapshot's data
  becomes invisible.
- The version nodes and data pages remain on disk until GC.

### Acceptance
- [ ] Write data in snapshot 3. Soft-delete 3. Read at epoch 3 → returns
  pre-snapshot base (epoch 2 data)
- [ ] Soft-delete does not affect sibling snapshots: snapshot 3 deleted,
  snapshot 5 still visible
- [ ] After soft-delete: mapper_resolve(3) returns 2
- [ ] GC after soft-delete reclaims space

---

## Workload 6.5 — Epoch Mapper

### What
A pool-allocated chain of MapperEntry nodes. Two functions: `mapper_resolve`
(query-time) and `mapper_traversal_apply` (chain-walk-time).

### Data Structure

```c
typedef struct {
    int64_t epochMapperPtr;  // VirtualPtr to first MapperEntry, from superblock
} Mapper;
```

### `mapper_insert(mapper, fromEpoch, toEpoch, traversalApply)`

```
1. Walk the chain. If any entry has fromEpoch == this_from or toEpoch == this_from
   or fromEpoch == this_to or toEpoch == this_to:
       return VFS_ERR_EXISTS  // would create a chain — single-hop enforced
2. Allocate pool slot.
3. nodes_write_mapperentry(slot, fromEpoch, toEpoch, traversalApply ? 1 : 0, headPtr)
4. CAS-prepend to epochMapperPtr.
```

### `mapper_resolve(mapper, epoch) → resolved_epoch`

```
1. Walk the chain.
2. If entry->fromEpoch == epoch: return entry->toEpoch
3. Return epoch unchanged.
```

### `mapper_traversal_apply(mapper, entry_epoch) → bool`

```
1. Walk the chain.
2. If entry->fromEpoch == entry_epoch AND entry->flags & 1: return true (remap)
3. If entry->fromEpoch == entry_epoch AND !(entry->flags & 1): return false (don't remap)
4. No entry found: return false (no mapping exists, use original epoch)
```

### Acceptance
- [ ] `mapper_resolve(1)` with no entries → returns 1
- [ ] After commit(1→2): `mapper_resolve(1)` → returns 2
- [ ] After soft-delete(3→2): `mapper_resolve(3)` → returns 2
- [ ] Inserting mapping where fromEpoch already mapped → rejected
- [ ] Inserting mapping where toEpoch already mapped → rejected
- [ ] Mapper chain survives crash and remount

---

## Workload 6.6 — TouchedFile Tracking

### What
A per-epoch list of files modified in a snapshot. Used exclusively by commit
for conflict detection. Persisted to disk so crashes don't force full tree scans.

### `touchedfile_add(vfs, epoch, nodeId)`

Called from `vfs_write` when a VersionPage is first written for a file in an epoch.

```
1. Walk the TouchedFile chain from superblock->touchedFilesPtr.
   Check if an entry with (epoch == this_epoch AND nodeId == this_nodeId) exists.
   If yes: return (already tracked — dedup).
2. Allocate pool slot.
3. nodes_write_touchedfile(slot, epoch, nodeId, headPtr)
4. CAS-prepend to superblock->touchedFilesPtr.
```

### `touchedfile_collect(vfs, epoch, nodeId_list_out, max) → count`

Called by commit to get the list of modified files.

```
1. Walk the chain.
2. For each entry with epoch == target_epoch:
       add nodeId to output list (dedup by nodeId)
3. Return count.
```

### `touchedfile_drop(vfs, epoch)`

Called after commit or soft-delete. Removes the entries for this epoch.
Simple implementation: set the chain head to skip entries with this epoch
(they'll be reclaimed by GC). Or just leave them — GC will clean them up.

### Acceptance
- [ ] Modify file A at epoch 3: one TouchedFile entry {epoch=3, nodeId=A}
- [ ] Modify file A again at same epoch: no duplicate
- [ ] Modify file B at epoch 3: second entry {epoch=3, nodeId=B}
- [ ] Commit epoch 3: both A and B scanned for conflicts
- [ ] `kill -9` after write, before commit → remount → TouchedFile entries
  still present → commit can proceed without full tree scan

---

## Final Phase 6 Checklist

- [ ] Snapshot creates new odd epoch with zero I/O
- [ ] Valid Epochs check correctly identifies writable/frozen epochs
- [ ] Commit succeeds when clean, fails with VFS_ERR_CONFLICT when page modified
  in both snapshot and any intervening live-head epoch
- [ ] Sibling snapshots handled correctly (all intervening even epochs scanned)
- [ ] Soft-deleted snapshot data invisible to reads
- [ ] Mapper correctly resolves committed and soft-deleted epochs
- [ ] Mapper enforces single-hop invariant
- [ ] TouchedFile dedup prevents duplicate entries for same (file, epoch)
- [ ] Crash after write, before commit: TouchedFile entries survive, commit works
