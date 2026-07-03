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
