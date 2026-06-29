# Phase 6: Epoch System

## Goal
Snapshot, commit with conflict detection, soft-delete, epoch mapper.

## Workloads

### 6.1 Snapshot
- `vfs_snapshot()`: `currentEpoch += 2`, return new odd epoch
- In-memory only; no disk write
- Multiple active sibling snapshots supported
- Tests: take 2 snapshots, verify epochs are odd and distinct

### 6.2 Commit
- `vfs_commit(snapshot_epoch)`:
  1. Walk TouchedFile chain for snapshot epoch S
  2. For each file, walk all epochs [S+1, current_live_head] looking for conflicts
  3. If any logical page has VersionPage at both S and a live-head epoch → abort
  4. Add mapper entry S→(S+1) with traversalApply=true
  5. Drop TouchedFile chain for S
- Tests: commit clean, commit with conflict, commit with sibling snapshot intervening

### 6.3 Soft-Delete
- `vfs_delete_snapshot(epoch)`:
  1. Add mapper entry S→(S-1) with traversalApply=false
  2. Drop TouchedFile chain for S
- Tests: delete snapshot, verify data invisible, GC later reclaims

### 6.4 Epoch Mapper
- Chain at superblock.epochMapperPtr
- Each entry: fromEpoch(4), toEpoch(4), flags(2), rsvd(6), nextPtr(8)
- `mapper_resolve(R)`: walk chain, return toEpoch if found, else R
- Single-hop invariant enforced at insert
- GC compacts mapper chain
- Tests: add mapping, resolve, verify single-hop enforcement

### 6.5 TouchedFile Tracking
- On first VersionPage written per file per snapshot epoch: CAS-prepend TouchedFile entry
- Chain at superblock.touchedFilesPtr per epoch
- Tests: write to 3 files in snapshot, verify 3 TouchedFile entries

### 6.6 Read Rule with Mapper
- Query resolution: `R' = mapper_resolve(R)`
- Traversal remapping: if traversalApply=true for entry's epoch, treat as toEpoch
- If traversalApply=false, don't remap (default rules apply)
- Tests: read committed snapshot data, read soft-deleted snapshot (returns base)
