# Phase 8A: Locking Model Fix

## Goal
Replace the two-phase per-epoch locking implementation with a correct
global-vs-per-epoch exclusion model. The current implementation has two bugs:
per-epoch locks serialize on a shared global mutex, and global locks don't
wait for per-epoch locks to drain.

## Source

Replace `vfs_lock` and `vfs_unlock` in `src/vfs.c`.
No header changes needed — signatures remain identical.

---

## Workload 8A.1 — Per-File Lock State

### What
A per-file struct (not per-epoch) that tracks all lock state for one file
node. Per-epoch locks increment a counter; the global lock waits for the
counter to reach zero.

### Data Structure

```c
// One per file nodeId, stored in the same 256-bucket hash table
typedef struct FileLockState {
    pthread_mutex_t mtx;           // protects this struct
    pthread_cond_t  cv;            // condition variable for drain wait
    int64_t         nodeId;        // file nodeId this state belongs to
    int             epoch_count;   // number of active per-epoch locks
    bool            global_held;   // global (epoch=0) lock is held
    bool            global_pending; // global lock is waiting to acquire
    int             refcount;      // for hash table lifecycle
    struct FileLockState* next;    // hash chain
} FileLockState;
```

### Hash Table Integration

Replace the current per-epoch `LockEntry` with `FileLockState`. The table
is keyed by `nodeId` only (not `nodeId + epoch`). Each `FileLockState`
manages all locks for one file across all epochs.

`LockEntry` for per-epoch locks is replaced by a simple counter in
`FileLockState`. The per-epoch `pthread_mutex_t` per entry is removed —
per-epoch locks only need the counter in `FileLockState`.

---

## Workload 8A.2 — Lock Acquisition

### `vfs_lock(vfs, file, epoch)` — Global Lock (epoch == 0)

```
1. Find or create FileLockState for 'file'.
2. Lock FileLockState->mtx.
3. Set global_pending = true.
4. While epoch_count > 0:
       pthread_cond_wait(&cv, &mtx)   // wait for per-epoch locks to drain
5. Set global_held = true.
6. Set global_pending = false.
7. Unlock FileLockState->mtx.
8. Return VFS_OK.
```

### `vfs_lock(vfs, file, epoch)` — Per-Epoch Lock (epoch != 0)

```
1. Find or create FileLockState for 'file'.
2. Lock FileLockState->mtx.
3. If global_held || global_pending:
       // Block until global lock is released (or pending is cancelled)
       while (global_held || global_pending):
           pthread_cond_wait(&cv, &mtx)
4. epoch_count++.
5. Unlock FileLockState->mtx.
6. Return VFS_OK.
```

### Recursive Locking

Per-epoch recursive locks (same thread, same epoch) are not tracked in
`FileLockState`. The VFS write operations are single-call — they acquire
before the write and release after. No nested write calls within the same
epoch on the same file. Skip recursive lock support.

Global recursive locks: if `global_held` and same thread, increment a
`global_depth` counter.

---

## Workload 8A.3 — Lock Release

### `vfs_unlock(vfs, file, epoch)` — Global Unlock

```
1. Find FileLockState for 'file'.
2. Lock FileLockState->mtx.
3. If global_depth > 1:
       global_depth--  // recursive unlock
   Else:
       global_held = false
       pthread_cond_broadcast(&cv)  // wake all per-epoch waiters
4. Unlock FileLockState->mtx.
5. Decrement refcount; free if zero.
```

### `vfs_unlock(vfs, file, epoch)` — Per-Epoch Unlock

```
1. Find FileLockState for 'file'.
2. Lock FileLockState->mtx.
3. epoch_count--.
4. If epoch_count == 0 && global_pending:
       pthread_cond_signal(&cv)  // wake global lock waiter
5. Unlock FileLockState->mtx.
6. Decrement refcount; free if zero.
```

---

## Concurrency Model

### Two Per-Epoch Locks, Same File, Different Epochs

```
Thread A: vfs_lock(file, 1)        Thread B: vfs_lock(file, 3)
  lock(&mtx)                          lock(&mtx)
  global_held? No                    global_held? No
  epoch_count = 1                    epoch_count = 2
  unlock(&mtx)                       unlock(&mtx)
  // both proceed concurrently ✅
```

### Global Lock While Per-Epoch Lock Held

```
Thread A: vfs_lock(file, 1)        Thread B: vfs_lock(file, 0)
  lock(&mtx)                          lock(&mtx)
  epoch_count = 1                     global_pending = true
  unlock(&mtx)                        while (epoch_count > 0):
                                        cond_wait(&cv, &mtx)  // blocks ✅
  ... write ...
  vfs_unlock(file, 1)                 
    lock(&mtx)                        
    epoch_count = 0                   
    signal(cv)  → wakes B
                                        epoch_count == 0 → proceed
                                        global_held = true
                                        unlock(&mtx)
```

### Per-Epoch Lock While Global Lock Held/Pending

```
Thread B: vfs_lock(file, 0)        Thread A: vfs_lock(file, 1)
  lock(&mtx)                          lock(&mtx)
  global_pending = true               while (global_pending):
  no wait (epoch_count=0)               cond_wait(&cv, &mtx)  // blocks ✅
  global_held = true                  
  unlock(&mtx)
  ... write ...
  vfs_unlock(file, 0)
    lock(&mtx)
    global_held = false
    broadcast(cv)  → wakes A
                                        global_pending now false
                                        epoch_count = 1
                                        unlock(&mtx)
```

---

## Acceptance

- [ ] Two per-epoch locks on same file, different epochs: both acquire without blocking
- [ ] Global lock (epoch=0) blocks until all per-epoch locks on that file are released
- [ ] Per-epoch lock blocks while global lock is held or pending on that file
- [ ] Global lock broadcast wakes all per-epoch waiters on release
- [ ] Per-epoch unlock signals global waiter when count reaches zero
- [ ] No shared mutex between per-epoch lock acquisitions on different epochs
- [ ] Multi-threaded stress: 4 threads doing write/lock/unlock on different files
  for 10 seconds, no deadlocks, no corruption

---

## Workload 8A.4 — Mapper Integration in DirContent/FileSize Walk

### What
`vfs_open_file`, `vfs_readdir`, `vfs_file_size`, and `vfs_file_mtime` use
a read rule on DirContent and FileSize chains that does not apply the epoch
mapper. This makes committed and soft-deleted snapshots invisible/misvisible
during directory lookups and stat calls.

### Current Code (Broken)

In `vfs_open_file` (tree.c:867), `vfs_readdir`, and `vfs_file_size`:
```c
int applies = (ce_epoch == query_epoch) ||
              (ce_epoch < query_epoch && ce_epoch % 2 == 0);
```

No `mapper_resolve(epoch)` on the query. No `mapper_traversal_apply` on entries.

### Consequence

- **After commit (1→2):** Reading at epoch 1 — not remapped to 2. Entries at
  epoch 1 are odd → even check fails → committed files are invisible.
  `vfs_open_file` returns VFS_ERR_NOTFOUND for committed files.
- **After soft-delete (3→2):** Reading at epoch 3, entries at epoch 3 match
  `ce_epoch == 3` → applies. Soft-deleted files remain visible. Should be hidden.
- **FileSize:** Same gap — committed sizes return baseline, soft-deleted sizes
  still visible.

### Fix

Apply the same mapper logic already present in `vfs_read` (tree.c:1211, 1266):

```c
// Query remap (before the chain walk):
int64_t read_epoch = mapper_resolve(&ctx->mapper, epoch);

// Entry remap (inside the chain walk, per entry):
int64_t effective_epoch = (int64_t)ce_epoch;
if (mapper_traversal_apply(&ctx->mapper, (int64_t)ce_epoch)) {
    effective_epoch = mapper_resolve(&ctx->mapper, (int64_t)ce_epoch);
}
int applies = (effective_epoch == read_epoch) ||
              (effective_epoch < read_epoch && effective_epoch % 2 == 0);
```

### Files to Update

| File | Function | Chain Type |
|------|----------|------------|
| `src/tree.c` | `vfs_open_file` | DirContent |
| `src/tree.c` | `vfs_readdir` | DirContent |
| `src/tree.c` | `vfs_file_size` | FileSize |
| `src/tree.c` | `vfs_file_mtime` | FileSize |
| `src/tree.c` | `vfs_rename` (lookup) | DirContent |
| `src/tree.c` | `vfs_delete` (lookup) | DirContent |

### Acceptance
- [ ] After commit: `vfs_open_file` at snapshot epoch finds committed files
- [ ] After soft-delete: `vfs_open_file` at deleted epoch returns VFS_ERR_NOTFOUND
- [ ] After commit: `vfs_file_size` at snapshot epoch returns committed size
- [ ] After soft-delete: `vfs_file_size` at deleted epoch returns pre-snapshot size
- [ ] `vfs_readdir` correctly shows/hides entries after commit/soft-delete
- [ ] All existing tests pass (no regression)

---

## Workload 8A.5 — GC Data Page & Indirection Cleanup

### What
GC currently only frees old pool pages. Data pages from dropped VersionPages
and their indirection table entries are never cleaned. This leaks logical
pages and physical storage.

### Bugs Found

**1. Data pages are never freed.**
`gc_shadow_compact` only walks the old pool list (line 946–957). Unreachable
data pages (from VersionPages dropped during GC) keep their indirection
entries — the logical page appears allocated forever. `Allocate` eventually
exhausts free logical pages.

**2. Indirection table is never cleaned.**
The spec requires "All unreachable logical pages: entries set to 0." GC
doesn't touch the indirection table at all. Without this, dead logical
pages remain permanently allocated.

**3. Old pool list may be NULL.**
Line 895: `ctx->pool.list_head ? *ctx->pool.list_head : 0`. If the pool
was never allocated (empty tree), `list_head` is NULL and the dereference
is skipped — but `old_pool_list_head` is 0 and no cleanup happens. The
old tree's pool pages (if any exist) are not freed.

### Fix

After the tree walk but before writing the new superblock:

1. **Collect live data pages.** During the tree walk, every surviving
   VersionPage has a `dataPage` field. Record all of these in a hash set
   or sorted array. These are the live data pages.

2. **Walk the old indirection table.** For every logical page from 2 to
   `total_pages - 1`:
   - If it's a pool page AND was already enqueued in the deferred-free
     queue → skip (already being freed).
   - If it's a pool page AND was NOT enqueued → add to deferred-free queue
     (it's an old pool page outside the list chain — shouldn't happen, but
     safe to catch).
   - If it's a data page AND NOT in the live set → `indir_set(page, 0)`.
   - If it's a data page AND in the live set → keep.

3. **Walk old pool pages properly.** Use `indir_lookup` to find the old
   page's physical offset, read its `next` pointer from the payload.
   Don't use `storage_read` on already-freed pages.

### Before/After

```
Before GC:                    After GC (current):           After GC (fixed):
  pool page A (live)            new pool page A'             new pool page A'
  pool page B (dead)            new pool page B'             new pool page B'
  data page X (live)            data page X (still alloc)    data page X (alloc)
  data page Y (dead)            data page Y (still alloc!)   data page Y (freed → 0)
  indirection entries all used  indirection entries all used indirection cleared for dead
```

### Acceptance
- [ ] After GC with soft-deleted snapshot: dead data pages freed, indirection entries 0
- [ ] After GC: `Allocate` can reuse freed logical pages
- [ ] After GC with no soft-deleted data: all data pages survive
- [ ] Empty tree GC: no crash, no leaked pages
