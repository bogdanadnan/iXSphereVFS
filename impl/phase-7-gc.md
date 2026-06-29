# Phase 7: Garbage Collection

## Goal
Shadow compaction GC with deferred-free queue.

## Workloads

### 7.1 Tree Lock
- `treeLockState` CAS-based reader/writer lock
- Reader: check bit 63, if clear → CAS-inc reader count
- Writer: set bit 63 → spin until reader count reaches 0
- Crash recovery: if bit 63 set, discard that superblock generation

### 7.2 Shadow Compaction
- Walk tree from root, collect all reachable VirtualPtrs
- Build new pool pages with surviving entries only:
  - Drop VersionPage entries from soft-deleted epochs
  - Collapse committed epochs (relabel epoch S→E)
  - Drop DirContent entries from deleted epochs where no surviving entry for childNodeId
  - Drop FileContent segments beyond surviving FileSize
  - Rebuild mapper chain, drop dead entries
- Write new pool pages, new superblock (with treeLockState=0)
- fsync, then atomically swap superblock pointer

### 7.3 Deferred-Free Queue
- Old tree pages placed in deferred-free queue
- Allocator skips pages in queue during scans
- Once GC confirms no active traversals, actually free via `Free()`
- Background phase, non-blocking

### 7.4 Tests
- Create file, snapshot, write more, soft-delete snapshot, GC, verify space reclaimed
- Crash during GC → old superblock still valid
- Deferred-free: allocate after GC, verify old pages not reused prematurely
