# Phase 28: GC Refactor (Framework)

**Status:** Draft. The framework design is the foundation; the
actual bin jobs (specific work functions per garbage type) are
added in subsequent phases following the per-bin-job spec
template defined here.

**Goal:** Replace the existing stop-the-world GC (`src/gc.c`,
Phase 7) with a **persistent Bin + background thread + per-bin
job dispatch** framework. The framework is generic — it knows
how to store, retrieve, and dispatch garbage-collection work
entries. The actual work (what to do for a deleted file, a
soft-deleted snapshot, a committed epoch, etc.) is defined
**per bin job**, in its own spec section that plugs into the
framework's dispatch table.

**What's in this spec:**

1. The Bin (persistent queue in the VFS file)
2. The producer API (`bin_push` called from garbage-producing
   public operations)
3. The GC thread (background, starts at mount, stops at unmount)
4. The GC bin consumer (dispatch table that maps Bin entry types
   to per-job analysis / work functions)
5. The per-bin-job spec section structure (a template that
   future bin jobs fill in to plug into the framework)

**What's NOT in this spec:**

- The actual per-bin-job work functions (those are added in
  separate specs, one per bin job, each reviewed independently
  against the per-bin-job spec template in §5)
- The fix for the GC-coupled latent bugs (N2, M4/N3) — those
  become unblocked once the framework exists, but the fix is
  not part of this spec

The framework makes **no attempt** to do any actual GC work.
It only delivers Bin entries from producers to consumers.
This separation is what lets bin jobs be added one-by-one
without re-reviewing the framework.

## Background

### Current state (Phase 7)

The current GC is a single-threaded stop-the-world operation
exposed as `vfs_gc(vfs)` in the public API:

- `src/gc.c` — the implementation (~1700 lines, including the
  deferred-free queue, the tree lock, and shadow-compaction)
- `src/gc.h` — the public GC interface (declarations, structs,
  the `DeferredFreeQueue` type, the `LivePageSet` type)
- `vfs_gc(vfs_t* vfs)` — the public entry point (declared in
  `include/ixsphere/vfs.h:180`, currently a foreground op)

**The current GC is non-functional.** The ISSUES.md entries
**M4**, **N2**, **N3** describe the latent bugs that activate
when the GC runs against real data:

- **N2** (Phase 27 review): GC's `pool_acquire` calls use
  `pinPage=false`, so `pool_release` is a no-op. Shadow-copied
  entries are never written back, so the new tree is empty.
- **M4/N3**: `gc_copy_entry` does not dispatch on the slot's
  type byte (legacy code assumed all entries are FileSize).
  The chain rewriting in `gc_walk_versionpage_chain` (the
  S→E rewrite) silently corrupts any chain whose entries
  aren't FileSize.

These bugs are out of scope for this spec. They will be
testable and fixable once the framework exists, because the
framework lets us write proper unit tests for individual
chain-walks (each bin job is independently testable).

**The current GC's deferred-free queue is also out of scope.**
The `DeferredFreeQueue` struct in `src/gc.h` and its helpers
(`deferred_free_init`, `deferred_free_enqueue`,
`deferred_free_is_queued`,
`deferred_free_confirm_and_release`,
`deferred_free_destroy`) are mechanisms designed for the
old GC's long-running tree sweep. The new framework's
per-bin-job lock model (see §5) replaces the need for a
global "wait for all readers before any free" mechanism.

### Why a refactor

Three reasons:

1. **The 5 garbage types need different lock models.** The
   current GC's single global `treeLockState` (reader-writer
   in `src/gc.h:30-70`) serializes ALL GC work against ALL
   readers. This is correct but pessimistic: a tombstone
   removal in one dir doesn't need to exclude readers of
   other dirs. The new framework lets each bin job pick its
   own lock (per-file, per-mapper, per-chain, CAS, or none).

2. **The 5 garbage types need different crash-safety
   stories.** The current GC assumes no crash mid-walk (the
   new superblock is the commit point). The new framework
   requires each bin job to be idempotent — a crash mid-job
   must be safe to re-run.

3. **The 5 garbage types should be testable in isolation.**
   The current GC's monolithic `vfs_gc` function makes
   per-type testing hard. The new framework's dispatch
   table lets each bin job be unit-tested independently
   (push a specific Bin entry, call the dispatch function,
   verify the chain modification, no other chain affected).

### The 5 garbage types (recap, for context only)

This is the analysis from the brainstorming doc
(`impl/phase-28-gc-free-pages.md`), summarized here so the
framework spec is self-contained. **The actual per-bin-job
spec sections are NOT in this spec** — they are added one
by one afterwards, following the template in §5.

| # | Garbage type | Producers | What GC does |
|---|--------------|-----------|--------------|
| 1 | Free pages (orphaned data pages) | 6 paths: `mirror_write` failure, GC pool remap, GC data reclamation, file delete, truncate (shrink), snapshot delete | Identify dead pages via chain walks, call `storage_free` |
| 2 | Tombstones in dir chains | `vfs_delete`, `vfs_rename` | Drop tombstone entries during pool rebuild |
| 3 | Soft-deleted mapper entries | `vfs_delete_snapshot`, `vfs_rollback` | Drop the soft-delete mapper entry during mapper rebuild |
| 4 | Committed mapper entries + chain rewriting | `vfs_commit` | Rewrite chain S→E during chain walk, drop the committed mapper entry |
| 5 | Future pool compaction | (not a current path) | Move slots to new pages, free old pages |

**7 public operations** leave garbage: `vfs_delete`,
`vfs_truncate` (shrink), `vfs_delete_snapshot`,
`vfs_rollback`, `vfs_rename`, `vfs_commit`,
`mirror_write` (rare failure path).

The framework spec does not enumerate how each of these is
processed — that's per-bin-job work. The framework only
defines:

- How a producer signals "this thing happened" (bin_push)
- How a consumer receives the signal (bin_pop from the
  background thread)
- How the consumer dispatches to the right work function
  (the dispatch table)
- What the per-bin-job spec section must contain (the
  template in §5)

## Proposed state

### Bin overview

The **Bin** is a persistent FIFO queue stored in the VFS
backing file, following the same pattern as the free-page
queue (Phase 27). It has three pieces:

- **Header fields** in the header page: `bin_head`,
  `bin_tail`, `bin_count` (3 × 8 bytes, total 24 bytes)
- **Bin pages** in the indirection: regular storage pages
  holding the Bin entries, with a custom layout (see §4.2)
- **Bin operations**: `bin_push`, `bin_pop`, `bin_peek`
  (see §6)

Like the free-page queue:

- Bin pages are allocated via `storage_allocate` and freed
  via `storage_free` when they drain
- The hot-path check is one atomic load of `bin_count`
  (zero = empty, fall through)
- The push/pop lock pattern is per-Bin-page CAS on `count`
  (no global lock; same pattern as Phase 27 W6 per-page
  CAS for the free-list dequeue)

Unlike the free-page queue:

- The Bin holds small fixed-size entries (16 bytes each,
  one BinEntry struct), not (logical VP, physical offset)
  pairs
- Entries are **typed** — a BinEntry's `type` field
  determines which work function dispatches (see §7)
- The Bin survives across unmount/mount (the free-page
  queue already does this; the Bin inherits the property)

### Trigger/work split

The framework implements the **trigger/work split** as a
core design principle. There are two kinds of Bin entries:

- **TRIGGER entries** — generic, contextual, pushed by
  producers. A trigger says "this kind of event happened"
  (e.g., "a file was deleted", "an epoch was committed").
  The trigger's `context` field carries the event's
  parameters (file VP, snapshot epoch, etc.). The
  framework's trigger-handler does the **analytical step**:
  walks the chain, identifies specific garbage items, and
  pushes one or more **WORK entries** for each. The
  trigger itself is deleted at the end of the analysis.

- **WORK entries** — specific, direct, pushed by the
  trigger-handler's analysis. A work entry says "do this
  specific thing" (e.g., "free this page", "remove this
  tombstone"). The framework's work-handler does the
  actual work.

The split is the key to per-bin-job flexibility:

- **Producers stay simple.** They don't need to know the
  chain structure or what specific garbage the operation
  produces. They just say "this thing happened".
- **The trigger-handler is the only place that knows the
  chain structure.** Changes to the chain (new node types,
  new garbage types) only affect the trigger-handler's
  analysis, not the producers.
- **Work entries are independent and re-entrant.** Each
  work entry is a self-contained unit of work that can be
  processed in any order, retried, or batched with other
  work entries of the same type.

### BinEntry layout

A Bin entry is 16 bytes (2 × 8 bytes):

```c
typedef struct {
    int64_t context;   /* type-specific identifier */
    int32_t type;      /* bin_entry_type_t — trigger or work */
    int32_t context2;  /* second type-specific field (used by some triggers/works) */
} BinEntry;
```

- `type` is a small enum (see below). 32 bits is more than
  enough headroom (the framework anticipates at most ~32
  entry types in total).
- `context` and `context2` are type-specific. For most
  entries, only `context` is used. `context2` is reserved
  for entries that need two identifiers (e.g., a chain
  rewrite that needs both the entry's VP and the to-epoch).

**Entry type enum** (initial sketch — the actual enum is
defined in the framework implementation, but the framework
spec documents the high-level structure):

```c
/* Entries with type < BIN_TYPE_WORK_THRESHOLD are TRIGGER
 * entries (processed by the trigger handler, which pushes
 * WORK entries and deletes the trigger). Entries with type
 * >= BIN_TYPE_WORK_THRESHOLD are WORK entries (processed
 * directly by the work handler). */
#define BIN_TYPE_WORK_THRESHOLD  0x10000

/* TRIGGER types (initial) */
typedef enum {
    BIN_TRIGGER_FILE_DELETED,        /* context = file VP */
    BIN_TRIGGER_FILE_TRUNCATED,      /* context = file VP, context2 = new size */
    BIN_TRIGGER_EPOCH_COMMITTED,     /* context = snapshot epoch */
    BIN_TRIGGER_EPOCH_SOFT_DELETED,  /* context = snapshot epoch */
    BIN_TRIGGER_TOMBSTONE_ADDED,     /* context = tombstone slot VP */
    /* Future trigger types are added here. */
} bin_trigger_type_t;

/* WORK types (initial) */
typedef enum {
    BIN_WORK_REMOVE_TOMBSTONE,        /* context = tombstone slot VP */
    BIN_WORK_DROP_SOFT_DELETE,        /* context = mapper slot VP */
    BIN_WORK_DROP_COMMITTED_MAPPER,   /* context = mapper slot VP */
    BIN_WORK_REWRITE_CHAIN_ENTRY,     /* context = chain slot VP, context2 = to-epoch */
    BIN_WORK_FREE_PAGES,              /* context = head of pages list (sentinel for batch) */
    /* Future work types are added here. */
} bin_work_type_t;
```

**Placeholder type for the framework's initial implementation:**

The framework spec defines a `BIN_TRIGGER_NOOP` placeholder
trigger type that the GC processes as "delete the entry, do
nothing else". This lets the framework be implemented and
tested end-to-end (producers push entries, GC thread
processes them, entries are deleted) without any real
bin-job logic. Once the framework is verified, the
placeholder is replaced by real bin-job trigger types in
subsequent phases.

The NOOP trigger is the **only** entry type the framework
spec defines. The trigger types and work types listed
above (`BIN_TRIGGER_FILE_DELETED`, etc.) are illustrative
sketches that future bin-job specs will refine and
finalize. They are not part of this spec's implementation
scope.

### Per-bin-job lock model

The framework **does not prescribe a global lock model**.
Each bin job's spec section describes:

- What lock (or no lock, or CAS) the job takes
- For how long
- What state the lock guarantees
- What the GC's flexibility is in implementation strategy
  (in-place rewrite vs copy+replace vs lazy)

The framework's `gc_process_entry` function is a **thin
dispatch layer** — it has no global lock, no per-bin-job
lock, no shared state. Each per-job work function takes
its own lock as needed.

**The page free specifically does not need a lock.** The
synchronization that excludes readers is already in place:

- **File delete** (`vfs_delete`): takes the file lock.
  After `vfs_delete` returns, the file is "deleted" from
  the system's perspective. Any operation that would read
  the file's data goes through the same lock (or sees
  the deleted state). So no concurrent reader of the data
  pages exists after the delete.

- **Truncation** (`vfs_truncate`): the pages past the
  new size are orphan IF no active snapshot still
  references them. The GC's analysis walks the FileSize
  chain to determine this. The analysis itself may need a
  lock (per-file); the free does not.

- **Commit** (`vfs_commit`): inserts a mapper entry and
  rewrites the chain. The chain rewriting is a chain
  modification, not a page free. The lock (if any) is
  for the chain modification.

- **Snapshot delete** (`vfs_delete_snapshot`): after the
  mapper's soft-delete, no reader sees the snapshot's
  data. The pages are truly orphan. The lock is for
  dropping the soft-delete mapper entry; the free itself
  needs no lock.

- **Tombstone** (`vfs_rename`): the tombstone is a chain
  modification. No page free. The lock (if any) is for
  the chain modification.

For all 5 garbage types, the lock (if any) is for **chain
modification**, not for the page free. The page free
specifically does not need a lock because the
synchronization that excludes readers is already in place.

**One exception: chain modification mid-walk.** For
commit's chain rewriting, the GC walks all chains to
rewrite S→E. A reader at S also walks the chain (to apply
the same rewrite on the read path). If the GC and the
reader walk the same chain concurrently, the reader
might see half-rewritten entries. The fix is either:

- A lock on the chain (held by GC during the walk;
  reader blocks briefly), OR
- A CAS-based rewrite (each entry is atomically
  rewritten via CAS; reader sees either old or new state)

The choice is per-bin-job (commit's chain rewriting
might use CAS; another bin job's chain rewrite might use
a lock). The framework provides the CAS primitive
(`vfs_cas_i32` for in-place entry rewrites) and the lock
primitives (the existing `vfs_lock` per-file, the
`tree_lock` per-tree); per-bin-job specs choose.

### Producer / GC separation of concerns

The framework's clean separation:

- **Producers** know: "I just did X (deleted a file,
  committed a snapshot, etc.)". They call `bin_push` to
  tell the GC about it.
- **GC** knows: the chain structure, what constitutes
  live vs dead data, how to identify the specific garbage
  items, how to do the work safely. The GC's analysis
  code is the single place that needs to know the chain
  structure.

This means:

- Producers don't need to be updated when the chain
  structure changes
- The GC's analysis code is the single place that needs
  to know the chain structure
- Each bin job's spec section describes only the GC's
  work (not the producer's call — the producer's call is
  a one-liner `bin_push(type, context)`)

## Data layout

### Header fields (VFS file)

The Bin adds 3 new 8-byte fields to the header page,
following the free-page queue's pattern (Phase 27 W1).
The fields are placed at the start of the inline
indirection area. The inline indirection table shifts
back, and `inline_count` is reduced by 3.

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0      | 8    | `total_pages`         | unchanged |
| 8      | 8    | `page_size`           | unchanged |
| 16     | 4    | `segment_size`        | unchanged |
| 20     | 4    | (padding)             | unchanged |
| 24     | 8    | `physical_tail`       | unchanged |
| 32     | 8    | `indirection_head`    | unchanged |
| 40     | 8    | `free_list_head`      | (Phase 27) |
| 48     | 8    | `free_list_tail`      | (Phase 27) |
| 56     | 8    | `free_list_count`     | (Phase 27) |
| 64     | 8    | `bin_head`            | **NEW** — VP of first Bin page (0 = empty) |
| 72     | 8    | `bin_tail`            | **NEW** — VP of last Bin page (0 = empty) |
| 80     | 8    | `bin_count`           | **NEW** — total entries across all Bin pages |
| 88     | 8    | (padding / future)    | reserved for one more 8-byte field (e.g., GC thread state) |
| 96     | 8*N  | inline indirection entries | shifted from offset 64; N = inline_count - 3 |

For an 8 KB page, `inline_count` goes from 1016 (post-Phase-27)
to 1013. For 4 KB: 504 → 501. The overflow chain takes over
beyond `inline_count`, so this only delays overflow allocation
by 3 page writes.

**Why an extra 8 bytes of padding at offset 88?** To leave
room for a future 8-byte field without re-shifting the
inline indirection table again. The padding is reserved
for one more field; the spec commits to not using it for
anything else. (If a future spec needs 2+ more fields, the
inline indirection shifts further; this spec doesn't
pre-solve that problem.)

The new offsets are added to `src/storage.h`:

```c
/* Phase 28 Bin (spec: impl/phase-28-gc.md).
   Three 8-byte fields live at offsets 64, 72, 80.
   An 8-byte padding at offset 88 is reserved for one
   future 8-byte field. The inline indirection table
   shifts from offset 64 to offset 96; inline_count is
   reduced by 3. */
#define HDR_OFF_BIN_HEAD     64
#define HDR_OFF_BIN_TAIL     72
#define HDR_OFF_BIN_COUNT    80
#define HDR_OFF_RESERVED     88
#define HDR_OFF_ENTRIES      96
```

### Bin page layout

A Bin page is a regular storage page (in the indirection)
with a custom layout. The Bin grows by allocating Bin
pages via `storage_allocate`; the Bin shrinks (when a
Bin page drains) by freeing the page via `storage_free`.

```
Offset  Size  Field
0       8     next_bin_page    (VP of next Bin page in chain, 0 = end)
8       4     count            (entries currently in this page)
12      4     capacity         (max entries in this Bin page)
16      16*K  entries[K]       (each entry: 16-byte BinEntry)
```

For an 8 KB page: `K = (8192 - 16) / 16 = 510` entries per
Bin page. For 4 KB: `K = (4096 - 16) / 16 = 255`.

So the Bin holds up to 510 entries per page of metadata.
Beyond that, the chain extends via `next_bin_page`. 100k
Bin entries need ~197 Bin pages = ~1.6 MB of metadata.
Well under any realistic worst case.

**BinEntry struct (recap):**

```c
typedef struct {
    int64_t context;   /* type-specific identifier */
    int32_t type;      /* bin_entry_type_t */
    int32_t context2;  /* second type-specific field */
} BinEntry;
```

16 bytes, matching the entry slot size in the Bin page
layout. No padding inside the struct.

### Bin page allocation

When the Bin is full (the tail Bin page's `count` equals
its `capacity`), `bin_push` allocates a new Bin page via
`storage_allocate`. To avoid the producer-consumer cycle
(the free-list's pattern, see Phase 27 spec §"storage_free
internal allocation (R6)"), `bin_push` uses an internal
helper `storage_allocate_bin_page` that does ONLY the
tail-advance (no free-list check). The new Bin page is
linked to the tail via:

1. Write `next_bin_page` field of the old tail to the new
   page's VP (this is a regular 8-byte atomic store on the
   old tail Bin page)
2. CAS-update the global `bin_tail` field from the old
   tail's VP to the new page's VP (this is the
   commit point — the old tail's `next_bin_page` is
   visible only after the CAS succeeds)

If step 2 fails (another thread won the race), the loser
sees the new tail and appends to it.

### Bin page deallocation

When the last entry is popped from a Bin page (the head
Bin page's `count` reaches 0), the GC frees the page via
`storage_free` and CAS-advances `bin_head` to the head's
`next_bin_page`.

The deallocation is the same pattern as Phase 27 W3's
drained-free-list-head handling: the freed Bin page goes
through the free-list (per Phase 27), so the physical slot
is reusable.

**`storage_free` of a Bin page is an immediate free.** It
does NOT need to be deferred to a queue. The Bin page is
not referenced by anything except the Bin itself (which
has been updated to point past it), so no in-flight
reader can hold a reference.

### Mount / unmount

**`mount_existing`**:

- Reads the Bin header fields `bin_head`, `bin_tail`,
  `bin_count`. The count must equal the sum of `count`
  fields across all Bin pages (validated via a walk; on
  mismatch, log a warning and rebuild the count from the
  walk — the same pattern as Phase 27 W1's
  `validate_free_list_on_mount`).
- Does NOT spawn the GC thread (that's `vfs_mount` in
  `vfs.c`, not `storage_open` in `storage.c` — the thread
  is a higher-level concern).

**`bootstrap_new`**:

- Sets the Bin header fields to 0 (no Bin entries).

**`vfs_unmount`**:

- Sets the GC thread's shutdown flag
- Joins the thread (after it finishes its current
  iteration; see §7.4 for the join semantics)
- Flushes the header page and any dirty Bin pages
- The Bin pages stay on disk (they're regular storage
  pages). No special cleanup needed.

**No backward compat.** Per the no-backward-compat
convention, the new header layout is mandatory. Older VFS
files (pre-Phase-28) are not readable. W1's mount path
documents this in the test (a pre-Phase-28 VFS file is
not expected to mount; the test creates a fresh VFS).

## Bin operations

### `bin_push` (producer side)

**Input**: `StorageBackend* sb`, `TreeContext* ctx`
(used for indirection access), `int32_t type`,
`int64_t context`, `int64_t context2` (defaults to 0).

**Algorithm** (sketch — the actual implementation lives
in W1):

```c
int bin_push(StorageBackend* sb, int32_t type,
             int64_t context, int64_t context2) {
    BinEntry entry = { .type = type,
                       .context = context,
                       .context2 = context2 };

    // 1. Read the current tail and tail_count atomically.
    int64_t tail = vfs_atomic_load_i64(HDR_OFF_BIN_TAIL);
    int     tail_count = (tail != 0)
        ? read_bin_page_count(sb, tail)  // CAS-loop read
        : 0;

    // 2. If tail is null OR tail_count == capacity, allocate
    //    a new Bin page via storage_allocate_bin_page (the
    //    internal tail-advance-only helper, like the
    //    free-list's storage_allocate_tail_advance).
    int64_t new_tail;
    if (tail == 0 || tail_count >= BIN_PAGE_CAPACITY) {
        new_tail = storage_allocate_bin_page(sb, 1);
        if (new_tail < 0) return VFS_ERR_FULL;

        // Initialize the new Bin page (next=0, count=0,
        // capacity=BIN_PAGE_CAPACITY).
        init_bin_page(sb, new_tail);

        // Link the new page as the new tail.
        if (tail != 0) {
            // Old tail exists — update its next_bin_page.
            write_bin_page_next(sb, tail, new_tail);
        } else {
            // Queue was empty — set head to new page.
            vfs_cas_i64(HDR_OFF_BIN_HEAD, 0, new_tail);
        }
        // Update tail to new page (this is the commit point
        // for the new page — after the CAS, the new page is
        // the tail and enqueues target it).
        vfs_cas_i64(HDR_OFF_BIN_TAIL, tail, new_tail);
    } else {
        new_tail = tail;
    }

    // 3. Append the entry to the new tail's entries array at
    //    position [tail_count]. CAS the count from
    //    tail_count to tail_count+1, retry if another thread
    //    won the race.
    if (!append_bin_page_entry(sb, new_tail, &entry)) {
        // Race lost — retry the whole push. Bounded retry
        // loop (e.g., 1000 max) to avoid livelock.
        return bin_push_retry(sb, type, context, context2);
    }

    // 4. Increment the global count.
    vfs_atomic_add_i64(HDR_OFF_BIN_COUNT, 1);

    // 5. Mark the Bin page dirty (so the change is flushed).
    cache_mark_dirty(&sb->cache, new_tail, FLUSH_PRIO_POOL);

    return VFS_OK;
}
```

**Failure modes** (all → the Bin push is dropped, the
operation that called `bin_push` still succeeds — the
garbage is just delayed):

- `storage_allocate_bin_page` fails (disk full) → return
  `VFS_ERR_FULL`. The push is dropped. (The page that the
  operation already wrote is not freed; it will be
  reclaimed on the next GC cycle when the Bin eventually
  catches up via a different mechanism, e.g., a full
  VFS walk.)
- CAS contention exceeds retry limit → return
  `VFS_ERR_FULL`. Same as above.
- `read_bin_page_count` returns 0 (head page was just
  drained) → retry with the new head.

**Concurrency**: `bin_push` is thread-safe via per-Bin-page
CAS on `count` and global CAS on `bin_head` / `bin_tail`
/ `bin_count`. Multiple producers (FUSE worker threads)
can push concurrently.

**Crash safety**: the Bin entry must be durable BEFORE
the operation's other side effects are made durable
(per-bin-job spec sections spell out the exact ordering
for their operation — this is a per-job concern, not a
framework concern). The framework provides the primitive
(`bin_push`); the ordering is the caller's responsibility.

### `bin_pop` (consumer side)

**Input**: `StorageBackend* sb`, `BinEntry* out_entry`.

**Output**: `VFS_OK` on success (entry popped); `VFS_ERR_NOTFOUND`
if the Bin is empty; negative error code on I/O failure.

**Algorithm** (sketch):

```c
int bin_pop(StorageBackend* sb, BinEntry* out_entry) {
    // 1. Read the head.
    int64_t head = vfs_atomic_load_i64(HDR_OFF_BIN_HEAD);
    if (head == 0) return VFS_ERR_NOTFOUND;  // empty

    // 2. Read the head page's count, with CAS-retry.
    int head_count = read_bin_page_count(sb, head);
    if (head_count == 0) {
        // Head page is empty (just drained by another
        // thread). Advance head to head->next.
        int64_t next = read_bin_page_next(sb, head);
        if (vfs_cas_i64(HDR_OFF_BIN_HEAD, head, next) == head) {
            // We won the advance. Free the drained head page.
            storage_free(sb, head);
        }
        return VFS_ERR_NOTFOUND;  // caller retries
    }

    // 3. Pop the LAST entry from the head page (page-local
    //    LIFO for cache locality — the most recently
    //    appended entry is hot in the cache).
    int     pop_idx  = head_count - 1;
    BinEntry entry;
    if (!read_bin_page_entry(sb, head, pop_idx, &entry)) {
        return VFS_ERR_IO;  // I/O failure
    }

    // 4. CAS the count from head_count to head_count - 1.
    if (vfs_cas_i32(HEAD_COUNT_OFFSET, head_count, head_count - 1) != head_count) {
        return VFS_ERR_NOTFOUND;  // race lost; caller retries
    }

    // 5. Decrement the global count.
    vfs_atomic_add_i64(HDR_OFF_BIN_COUNT, -1);

    // 6. If head_count was 1, the page is now empty. Advance
    //    head to head->next (same as step 2). Free the
    //    drained head page.

    // 7. Invalidate the cache entry for the Bin page (so
    //    subsequent pops see fresh data).

    // 8. Copy the entry to the caller's buffer.
    *out_entry = entry;
    return VFS_OK;
}
```

**Each pop is a single 16-byte BinEntry read** (no logical
VP / physical offset preservation needed — Bin entries are
opaque to the framework; the per-bin-job work functions
interpret the entry's fields).

**CAS pattern**: matches Phase 27 W6's per-page CAS on
count + head CAS-advance. Bounded retry loop handles
contention. ABA is detected via the per-page CAS on
count (the count is monotonic for a given Bin page; the
CAS from N to N-1 fails if another thread changed the
count).

**Concurrency**: `bin_pop` is single-threaded (only the
GC thread pops), but the CAS pattern is correct for
multi-threaded pops in case the framework is extended
later (e.g., a second GC thread for parallelism).

### `bin_peek` (optional)

**Input**: `StorageBackend* sb`, `BinEntry* out_entry`.

**Output**: `VFS_OK` on success (entry peeked, NOT
removed); `VFS_ERR_NOTFOUND` if the Bin is empty.

**Algorithm**: same as `bin_pop` step 1-3, but skip the
CAS-decrement, the global count decrement, and the head
advance. The entry is read from the head's [count-1] slot
but not removed.

**Use case**: the GC thread's scheduler may want to look
at the next entry's type to decide between
trigger-handling and work-handling (or for future
priority-based scheduling). For the framework's initial
implementation, `bin_peek` is provided but not used
(the GC thread just calls `bin_pop` and dispatches based
on the entry's type).

### Mount-time validation

The `bin_count` field must equal the sum of `count`
fields across all Bin pages. Validation walk:

1. Read `bin_head`. If 0, expect `bin_count == 0` and
   `bin_tail == 0`. Done.
2. Walk from `bin_head` via `next_bin_page` until the
   end. Sum the `count` fields.
3. If the sum matches `bin_count`, the Bin is consistent.
4. If the sum doesn't match `bin_count`, log a warning
   and rebuild `bin_count` from the walk.
5. If the walk doesn't end at `bin_tail` (e.g., the
   `next_bin_page` chain is broken), log an error and
   truncate the Bin at the last valid page (any entries
   past the break are lost; this is a recovery path, not
   a normal path).

The validation uses **raw `pread`** for the Bin pages
(not the cache), to avoid putting Bin pages in the
cache. This avoids the cache-coherency interaction that
Phase 27 W5's `validate_free_list_on_mount` was designed
to avoid (see Phase 27 W5 spec for the full reasoning).

The validation walk is implemented in W1.

## GC thread

### Lifecycle

- **Start**: at `vfs_mount` (after the StorageBackend is
  open and the Bin header fields are validated). The
  thread is spawned with the VFS instance's `vfs_t*` as
  its argument.
- **Stop**: at `vfs_unmount` (after all pending VFS
  operations are quiesced). The shutdown is signaled via
  an atomic flag (`vfs->ctx->gc_shutdown = 1`). The
  thread joins within a bounded timeout (e.g., 1 second);
  if the timeout expires, the thread is detached
  (continues running until the Bin is drained, but the
  VFS is considered closed).

**Crash safety of the loop**: the thread can be killed at
any point. On next process start, a new thread is spawned
at `vfs_mount`. The Bin is persistent, so no work is lost.

### Loop

```c
static void* gc_thread_main(void* arg) {
    vfs_t* vfs = (vfs_t*)arg;
    TreeContext* ctx = vfs->ctx;

    while (!vfs_atomic_load_i32(&ctx->gc_shutdown)) {
        BinEntry entry;
        int rc = bin_pop(ctx->sb, &entry);
        if (rc == VFS_OK) {
            /* Got a job — process it immediately, no sleep. */
            gc_process_entry(vfs, &entry);
        } else if (rc == VFS_ERR_NOTFOUND) {
            /* Bin is empty — back off for a bit, then check
             * again. The backoff is a sleep; see §7.3. */
            usleep(GC_BACKOFF_US);
        } else {
            /* I/O error or other failure. Log and back off
             * to avoid busy-looping on persistent errors. */
            log_gc_error(rc);
            usleep(GC_BACKOFF_US);
        }
    }

    return NULL;
}
```

**If there's work, we don't sleep** — we process as fast
as the Bin delivers entries. If the Bin is empty, we
sleep for `GC_BACKOFF_US` (initial: 100 ms; see §7.3 for
the backoff strategy) and check again.

The "intelligent scheduling" is the simple **try-then-backoff**
pattern: the Bin is the queue, the thread is the consumer,
the backoff is a constant sleep. No priorities, no
fairness, no starvation handling — these are out of scope
for the framework and can be added later as
per-bin-job-spec extensions if needed.

### Backoff strategy

The framework's initial implementation uses **constant
backoff** (`GC_BACKOFF_US = 100ms`):

- Constant backoff is simple and correct
- For the expected workload (occasional frees; the Bin
  drains quickly), the constant backoff is fine
- Adaptive backoff (exponential on continued empty, reset
  on first non-empty) is documented as a known post-MVP
  improvement

A more sophisticated backoff (e.g., busy-wait for the
first few ms, then sleep) is left for a future
optimization. The framework spec commits to constant
backoff for the initial implementation.

### Shutdown

The shutdown sequence:

1. `vfs_unmount` sets `ctx->gc_shutdown = 1`
2. The GC thread observes the flag at the top of its
   loop and exits
3. `vfs_unmount` calls `pthread_join` with a 1-second
   timeout
4. If the join succeeds, the thread has exited; the
   unmount completes
5. If the join times out, the thread is detached (it
   continues running until the Bin drains, but the VFS
   is considered closed and the thread's work is
   effectively leak-free because the Bin is persistent)

**Shutdown drain vs stop-and-leave**: the framework's
initial implementation uses **stop-and-leave**. The GC
thread exits on the shutdown signal; any unprocessed Bin
entries are left for the next mount. This is correct
because the Bin is persistent — the next mount will pick
up where the previous mount left off.

A **shutdown drain** (wait for the Bin to drain before
exiting) is documented as a future optimization for
workloads that want "clean unmount" semantics. The
framework spec does not commit to it.

### `gc_process_entry` (dispatch)

The GC's per-entry handler is a thin dispatch layer. The
**only** entry type it knows about in the initial
implementation is `BIN_TRIGGER_NOOP` (the placeholder
that does nothing). Future bin-job specs add their
trigger and work types to the dispatch table.

```c
int gc_process_entry(vfs_t* vfs, const BinEntry* entry) {
    if (entry->type < BIN_TYPE_WORK_THRESHOLD) {
        /* TRIGGER entry — call the trigger handler. */
        return gc_handle_trigger(vfs, entry);
    } else {
        /* WORK entry — call the work handler. */
        return gc_handle_work(vfs, entry);
    }
}

int gc_handle_trigger(vfs_t* vfs, const BinEntry* entry) {
    /* Dispatch on entry->type. For the framework's initial
     * implementation, the only trigger type is NOOP. */
    switch (entry->type) {
    case BIN_TRIGGER_NOOP:
        /* No-op trigger — just delete the entry, do
         * nothing else. The framework does NOT push any
         * work entries; the trigger is gone after the
         * dispatch returns. */
        return VFS_OK;

    /* Future trigger types (FILE_DELETED, EPOCH_COMMITTED,
     * etc.) are added in subsequent specs. Each adds a
     * case here and a corresponding analysis function in
     * a per-bin-job implementation file. */
    default:
        log_gc_unknown_trigger(entry->type);
        return VFS_ERR_IO;  /* unknown type, skip */
    }
}

int gc_handle_work(vfs_t* vfs, const BinEntry* entry) {
    /* Dispatch on entry->type. For the framework's initial
     * implementation, no work types are defined; any
     * unknown type is logged and skipped. */
    switch (entry->type) {
    /* Future work types (REMOVE_TOMBSTONE, DROP_SOFT_DELETE,
     * etc.) are added in subsequent specs. Each adds a case
     * here and a corresponding work function in a
     * per-bin-job implementation file. */
    default:
        log_gc_unknown_work(entry->type);
        return VFS_ERR_IO;  /* unknown type, skip */
    }
}
```

The dispatch table is **explicit** (a switch statement,
not a function-pointer table) because:

- It's easy to review (each case is a one-liner
  pointing at a function)
- The compiler can warn on missing cases (with
  `-Wswitch-enum`)
- It compiles to the same machine code as a function
  table (jump table optimization)

A function-pointer table is left as a future optimization
if the dispatch becomes a hot path (currently it's not —
each entry is processed in microseconds to milliseconds).

### Idempotency

The framework requires every per-bin-job work function to
be **idempotent** — running it twice on the same Bin
entry's context is safe and equivalent to running it
once. This is essential for crash safety:

- A crash mid-`gc_process_entry` leaves the Bin entry in
  place (the entry is only removed at the end of a
  successful processing). On next mount, the GC pops
  the same entry and re-runs the work function.
- A crash during trigger analysis may leave partial
  work entries in the Bin. The trigger is also still
  there (it was deleted at the end of the analysis). On
  next mount, the GC re-runs the analysis, which
  re-pushes the missing work entries (the analysis is
  idempotent — re-running on the same trigger context
  produces the same set of work entries).

**The framework enforces idempotency via the per-bin-job
spec template (§5)**: each future bin job's spec section
includes a "Crash safety" subsection that documents the
job's idempotency story. The implementation review
verifies the idempotency claim.

### Per-bin-job dispatch extension mechanism

The framework's dispatch table is a switch statement
that lives in a single file (`src/gc.c`, or a new
`src/gc_dispatch.c` if the file gets too large). Future
per-bin-job specs add cases to the switch.

To keep the framework's main file small, the per-bin-job
work functions live in their own files:

- `src/gc_trigger_file_deleted.c` (future — bin job spec
  for `BIN_TRIGGER_FILE_DELETED`)
- `src/gc_work_remove_tombstone.c` (future — bin job spec
  for `BIN_WORK_REMOVE_TOMBSTONE`)
- etc.

The dispatch switch in `src/gc_dispatch.c` calls the
functions in the per-bin-job files. The per-bin-job
files are independent — they can be added, removed, or
modified without touching the framework's core files
(`src/bin.c`, `src/gc_thread.c`).

## Per-bin-job spec section structure

The framework defines a **template** for each future bin
job's spec section. Every bin job's spec section (added
in subsequent specs) must contain the following
subsections. The framework's implementation review
verifies that each subsection is filled in and is
consistent with the framework's design.

The template is also what makes per-bin-job reviews
tractable: a reviewer can read the bin job's spec
section against this template and check that all the
required pieces are present and consistent.

### Subsection 1: Name and motivation

A short name (e.g., "File Deleted", "Snapshot Soft
Delete") and a one-paragraph motivation: what garbage
this bin job handles, what operation produces it, and
why a background GC pass is needed (rather than
immediate cleanup in the producer).

### Subsection 2: Trigger and work types

The Bin entry types this bin job uses:

- **TRIGGER type** (if any): the type producers push.
  The trigger's `context` and `context2` fields are
  defined here (what each field carries, what the
  analysis does with them).
- **WORK types** (if any): the types the trigger's
  analysis pushes. Each work type's `context` and
  `context2` fields are defined here.
- **Producer push call site**: the exact line in the
  producer's public operation where `bin_push` is
  called, with the type and context values filled in.
  This is the ONE place producers are updated for this
  bin job (per the framework's separation of concerns).

### Subsection 3: Analysis (trigger handler)

The trigger handler's algorithm:

- What chain(s) it walks
- How it identifies the specific garbage items
- What work entries it pushes
- How it deletes the trigger at the end

The analysis is **idempotent**: re-running on the same
trigger context produces the same set of work entries
(per the framework's idempotency requirement).

### Subsection 4: Work handler

For each work type, the work handler's algorithm:

- What modification it makes to the tree (chain
  modification, page free, mapper entry drop, etc.)
- What state it assumes (what the trigger's analysis
  has already established)
- What state it leaves (what the next work entry can
  assume)

The work handler is **idempotent**: re-running on the
same work context is safe and equivalent (per the
framework's idempotency requirement).

### Subsection 5: Lock model

Per the framework's per-bin-job lock principle, the
lock model is described here, not in the framework
spec:

- **Lock taken**: per-file (`vfs_lock`), per-mapper
  (new or existing), per-chain (new), CAS-only, or
  none. The bin job specifies what lock (if any) it
  needs.
- **Lock duration**: how long the lock is held (e.g.,
  "for the duration of the chain walk", "for the
  duration of the page free", "not held — uses CAS").
- **Lock guarantees**: what the lock prevents
  (e.g., "excludes concurrent readers of the file
  during the chain walk", "prevents concurrent
  modification of the same mapper entry").
- **Implementation flexibility**: what the GC can
  choose between (in-place rewrite, copy+replace,
  lazy). The bin job spec describes the
  recommended strategy and the acceptable
  alternatives.

### Subsection 6: Page free (if applicable)

If the bin job frees pages, this subsection describes
the free mechanism:

- Which pages are freed (the specific pages
  identified by the analysis)
- The free mechanism (immediate via `storage_free`,
  deferred via the Bin's free-list, or other)
- **Why the free does not need a lock** (per the
  framework's principle). The bin job references the
  framework's §3.4 ("The page free specifically does
  not need a lock") and confirms that the
  synchronization excluding readers is already in
  place.

If the bin job does NOT free pages (e.g., tombstone
removal, chain rewriting), this subsection is omitted.

### Subsection 7: Crash safety

The bin job's idempotency story:

- What crash windows exist (mid-trigger-analysis,
  mid-work-handler, mid-page-free, etc.)
- What state is on disk after a crash in each window
- How the next mount's GC re-runs the work safely
- What invariants the bin job's spec assumes about
  the framework (e.g., "the Bin entry remains in the
  Bin until the work handler returns success")

This subsection is reviewed for completeness by the
external reviewer. The framework requires it; the
specific content is the bin job's responsibility.

### Subsection 8: Test plan

The bin job's test cases:

- Unit test: push a specific entry, call the dispatch,
  verify the chain modification
- Integration test: produce a real garbage event
  (e.g., `vfs_delete` for a File Deleted job), let the
  GC thread process it, verify the state
- Crash safety test: simulate a crash mid-processing
  (e.g., by aborting the process during the dispatch),
  verify the next mount re-runs safely
- Performance test: measure the bin job's overhead
  (if the work is on a hot path; otherwise note as
  N/A)

The test plan is reviewed for completeness; the
framework requires it; the specific tests are the
bin job's responsibility.

### Subsection 9: Open questions

Any open design questions specific to this bin job.
Examples from the brainstorming doc:

- How to handle a bin job that needs to coordinate
  with another bin job (e.g., file delete and
  snapshot delete both want to free the same page)
- How to handle a bin job whose work depends on
  another bin job's completion (e.g., file delete
  needs the tombstone removal to happen first)
- How to handle backpressure (the Bin grows faster
  than the GC can drain it; should the producers
  block?)

These are bin-job-specific questions; the framework
doesn't have answers. The bin job spec proposes answers
and the reviewer accepts/rejects.

### Subsection 10: Source

References to the brainstorming doc sections, the
existing code, the ISSUES.md entries that motivate the
bin job, and any prior art.

## Concurrency model

### Producers

User-facing public operations (FUSE worker threads).
The producer's `bin_push` call is thread-safe via the
Bin page's per-page CAS. No global lock on the producer
side.

### Consumer

A **single** background thread (the GC thread). The
thread is created at `vfs_mount` and joined at
`vfs_unmount`. The thread reads the Bin, processes
entries, and removes them. No global lock on the
consumer side.

### Cross-thread (producer + consumer)

The producer and consumer share the Bin pages. The CAS
on the per-page `count` is the synchronization primitive.
The GC thread's work functions don't need the Bin lock
(they work on the tree, not the Bin).

**The GC thread's per-bin-job work function may need a
lock on the tree** (per the per-bin-job spec template's
Subsection 5: Lock model). This is a per-bin-job concern,
not a framework concern. The framework provides the
lock primitives (`vfs_lock`, the future per-mapper
lock, etc.); per-bin-job specs choose.

### Multiple GC threads (future)

The framework's CAS pattern is correct for multiple GC
threads (each `bin_pop` is atomic; multiple GC threads
would each pop different entries). The framework's
initial implementation uses a single GC thread. A
future optimization could add parallelism by spawning N
GC threads, each popping from the same Bin.

**For the framework's initial implementation, the GC
thread is single-threaded.** This is documented as a
deliberate simplification: per-bin-job work functions
are easier to reason about when they don't need to
coordinate with each other.

## Hot path performance

### `bin_push` cost

- 1 atomic load of `bin_tail` (~1 ns, L1 cached)
- 1 atomic load of the tail's `count` (might require a
  `storage_read`; the tail Bin page is usually in the
  cache after the first push, so subsequent pushes are
  cache hits)
- 1 atomic store of the entry at position [count]
- 1 CAS on the count (N to N+1; uncontended in the
  common case)
- 1 atomic add on `bin_count` (+1)
- 1 cache_mark_dirty (lock-free, no atomic)
- Total: ~10-20 ns in the common case (cache hit, no
  contention)
- Rare path (new Bin page): +1 `storage_allocate` call
  (~30-40 ns) + 1 atomic store of `next_bin_page` + 1
  CAS on `bin_tail`

**Producer impact**: `bin_push` is called from public
operations (`vfs_delete`, `vfs_commit`, etc.). The
10-20 ns is in noise for the typical public operation
(200-300 ns per op). The rare new-page path is a one-time
event (every 510 pushes), so its cost is amortized.

### `bin_pop` cost

- 1 atomic load of `bin_head` (~1 ns, L1 cached)
- 1 atomic load of the head's `count` (cache hit after
  the first pop; the head Bin page stays in the cache
  until it drains)
- 1 atomic load of the entry at [count-1]
- 1 CAS on the count (N to N-1; uncontended in the
  common case)
- 1 atomic add on `bin_count` (-1)
- 1 cache_invalidate (lock-free, no atomic)
- Total: ~10-20 ns in the common case

**GC thread impact**: the GC thread is the only consumer
of `bin_pop`. The 10-20 ns is in noise for the GC's
per-entry processing (microseconds to milliseconds per
entry, dominated by the chain walks).

### `bin_count` cache-line contention

The `bin_count` field is in the header page. The header
page is read by the producer (to check the count for
fast-path decisions in future optimizations) and written
by both the producer (on push) and the consumer (on
pop). Under low write frequency (the expected case for
the framework's initial implementation), the cache line
stays in L1 across reads and the cost is one
uncontended atomic load (~1-2 ns).

Under high write frequency (the Bin is heavily exercised
— e.g., a `vfs_delete` storm with many files), the cache
line bounces between cores. This adds ~5-20 ns per load
under contention. The framework spec documents this as a
known post-MVP concern; if profiling shows contention
after the framework is in production, the fix is to pad
`bin_count` to a separate cache line (64 bytes on x86,
128 on Apple Silicon). The header page has unused space
at offset 88 (the reserved field).

### Comparison to the free-list

The Bin is structurally identical to the free-list
(Phase 27). The same performance analysis applies:
hot path is ~10-20 ns; cache-line contention is a known
post-MVP concern; padding is the mitigation.

## Memory and disk overhead

**Per Bin entry**: 16 bytes (the BinEntry struct).

**Per Bin page**: 8 KB. With 510 entries per page, the
overhead is `8192 / 510 = 16 bytes per Bin entry of
metadata` + `16 bytes per Bin entry` = **32 bytes per
Bin entry of overhead**.

For 10k Bin entries (a heavy workload): 320 KB. Negligible.

**Header page growth**: +32 bytes (3 × 8 bytes for the
Bin fields + 8 bytes reserved padding). Inline
indirection count drops by 3.

**GC thread**: 1 thread, ~8 MB stack (default pthread
stack size). The thread is idle when the Bin is empty
(99%+ of the time for most workloads).

## Crash safety

The Bin is a persistent queue in the VFS file. On crash:

- **Header page** is written last (the existing pattern,
  via the periodic `storage_flush(-1)` or at unmount). If
  a crash happens before the header is written, the
  previous `bin_*` values are intact.
- **Bin page writes** are `pwrite` + `cache_mark_dirty`.
  On crash, the page cache is lost (in-memory state), but
  the on-disk data is what was last flushed.

**Edge case 1**: crash after a `bin_push` writes the Bin
page but before the header is updated. On remount, the
header shows the old state, but the Bin page has the new
entry. The Bin page's `count` is whatever was last
written. This is fine — the count is consistent with the
page's data; the header's `bin_count` will be rebuilt by
the mount-time validation walk.

**Edge case 2**: crash mid-update of the Bin page (e.g.,
the count was CAS'd to N+1, the entry written, but the
dirty mark not yet set). On remount, the count says N+1,
but the disk has only N entries. The hot path (the GC
thread's `bin_pop`) would read a garbage entry from
position N. **This is a real (if rare) bug.**

**Mitigation**: order the writes carefully. The
framework's push sequence is:

1. Write the entry at position [count]
2. CAS the count from `count` to `count+1` (this is
   the commit point — if we crash after this, the entry
   is "visible")
3. Mark the page dirty (so the change is flushed on
   next `storage_flush`)

The CAS in step 2 is the commit. If we crash before
step 2, the count is still `count` and the entry at
[count] is garbage but won't be read. If we crash
between step 2 and step 3, the count is `count+1` and
the entry is correct in cache but the page isn't
dirty. On remount, the page cache is empty, so the
next `bin_pop` reads from disk — which has the OLD
data (no flush happened). The count says N+1 but the
disk has only N entries. On the next pop, we'd read
the Nth entry, which is garbage.

**Mitigation for this**: the mount-time validation
walk validates each Bin page's count against the
number of valid entries on disk. If a count is ahead
of the disk, the validation rebuilds it. This is the
same approach as the free-list's
`validate_free_list_on_mount` (Phase 27 W5).

The validation is best-effort: it can detect the
"count ahead of disk" case but cannot recover the
missing entries. The missing Bin entries are lost.
This is acceptable because:

- The Bin is for garbage collection, not data
  integrity. Lost Bin entries mean delayed GC
  processing, not data corruption.
- The next GC cycle (or the next operation that
  pushes a related entry) will trigger the analysis
  again. The analysis is idempotent.
- The probability of crashing in the microsecond
  window between CAS and flush is very low.

The spec documents this as a known race with very low
probability.

**Edge case 3**: crash during trigger analysis
(mid-walk, mid-work-push, etc.). The trigger entry is
still in the Bin (it was not yet deleted). On remount,
the GC pops the same trigger and re-runs the analysis.
The analysis is idempotent — it pushes the same set of
work entries (the missing ones from the prior run, plus
the already-pushed ones which are deduped by the
per-entry deduplication check, e.g., a set of in-flight
work contexts). The trigger is deleted at the end of
the re-run.

**Edge case 4**: crash during work handler (mid-chain
modification, mid-page-free, etc.). The work entry is
still in the Bin. The state on disk is whatever the
work handler had committed at the time of the crash.
On remount, the GC pops the same work entry and re-
runs the work handler. The work handler is idempotent
— re-doing the work is safe and equivalent.

**All four edge cases are recoverable** because every
framework operation is idempotent. The framework
imposes this requirement on every per-bin-job work
function (per §7.6 and §5.7).

## Migration plan

The framework is implemented in **4 workloads**, in
dependency order. Each is self-contained, testable,
and benched.

### Workload 1: Bin infrastructure (storage + push/pop/peek + mount validation)

**Scope**:

- Add the 3 new header fields (`bin_head`, `bin_tail`,
  `bin_count`) at offsets 64, 72, 80, plus the 8-byte
  reserved padding at offset 88
- Shift the inline indirection table from offset 64 to
  offset 96 (inline count drops by 3)
- Define the `BinEntry` struct (16 bytes)
- Define the Bin page layout (`next_bin_page`,
  `count`, `capacity`, `entries[]`)
- Implement `bin_push` (with the per-page CAS pattern)
- Implement `bin_pop` (with the per-page CAS pattern)
- Implement `bin_peek` (read-only variant)
- Implement `storage_allocate_bin_page` (internal
  tail-advance-only helper, bypasses the free-list
  check)
- Implement the mount-time validation walk
  (`validate_bin_on_mount`)
- Update `bootstrap_new` (sets new fields to 0) and
  `mount_existing` (reads new fields, validates)
- Update `src/storage.h` with the new offsets
- Update `src/indirection.c` with the new inline count
  math
- Update `src/page_cache.c` if needed (the Bin pages
  are cacheable like any other; the existing
  `cache_invalidate` from Phase 27 W3 is reused)

**Test gate**:

- All existing ctest suites pass (no behavioral change
  for non-Bin operations; the Bin is unused by any
  public operation in W1)
- New test `test_bin_basic`:
  - Push 1000 entries, pop 1000, verify FIFO order
  - Push, pop, push, pop interleaved, verify counts
  - Push until Bin page is full, verify the new Bin
    page is allocated and linked
  - Pop until Bin page is empty, verify the Bin page
    is freed
- New test `test_bin_concurrent`:
  - 4 threads × 250 pushes = 1000 unique pushes
  - Single thread pops all 1000, verify no duplicates,
    no losses
- New test `test_bin_persistence`:
  - Push 5 entries, unmount, remount, verify the 5
    entries are still there
- New test `test_bin_mount_validation`:
  - Push 5 entries, unmount
  - Manually corrupt the header's `bin_count` to a
    wrong value, remount
  - Verify the count is rebuilt correctly via the
    validation walk
- New test `test_bin_idempotency`:
  - Push an entry, simulate a crash by killing the
    VFS, remount
  - Verify the entry is still in the Bin (the push
    was either fully committed or not at all)
- Bench delta: 0 (no public operation uses the Bin
  in W1)

**Files touched**:

- `src/storage.h` — add 3 new offsets + reserved
  padding offset; update `HDR_OFF_ENTRIES` to 96
- `src/storage.c` — `bootstrap_new`, `mount_existing`,
  `storage_allocate_bin_page` (new)
- `src/indirection.c` — update inline count math
  (offset 40 → offset 96)
- `src/bin.c` (new) — `bin_push`, `bin_pop`,
  `bin_peek`, mount-time validation walk
- `src/bin.h` (new) — public Bin API
- `test/test_storage.c` — new tests
- `test/test_bin.c` (new) — new test file for Bin-
  specific tests

**Commit**: `phase-28-w1: Bin infrastructure (storage + push/pop/peek + mount validation)`

### Workload 2: GC thread infrastructure (start/stop/loop/scheduling)

**Scope**:

- Add a `pthread_t gc_thread` field to `TreeContext`
  (in `include/ixsphere/vfs_internal.h`)
- Add an atomic `gc_shutdown` flag to `TreeContext`
- Add a `bin_count` atomic field to `TreeContext`
  (mirroring the header field for hot-path access —
  populated at mount, used by future optimizations)
- Implement `gc_thread_main` (the loop)
- Implement `gc_thread_start` (called from
  `vfs_mount`)
- Implement `gc_thread_stop` (called from
  `vfs_unmount`, with the bounded join)
- Implement `gc_process_entry` (the dispatch — only
  `BIN_TRIGGER_NOOP` is handled in W2; the work handler
  is a stub)
- Update `vfs_mount` to spawn the GC thread
- Update `vfs_unmount` to stop the GC thread
- Update `vfs.c` with the lifecycle changes
- Update `src/gc.c` to remove the existing
  `vfs_gc` implementation (the old GC is being
  dropped per the brainstorming doc — the public
  `vfs_gc` API will return `VFS_ERR_NOTIMPL` until
  the per-bin-job work functions are added in
  subsequent phases)
- Update `include/ixsphere/vfs.h` to document the
  new `vfs_gc` behavior

**Test gate**:

- All existing ctest suites pass
- New test `test_gc_thread_lifecycle`:
  - Mount, verify thread is running (`pthread_kill`
    with signal 0 returns 0)
  - Push a NOOP entry, wait briefly, verify the
    entry is processed (the Bin is empty after the
    wait)
  - Unmount, verify thread has joined
- New test `test_gc_thread_shutdown`:
  - Mount, push 100 NOOP entries, immediately
    unmount
  - Verify the thread joins within the timeout (the
    unprocessed entries are left in the Bin)
  - Remount, verify the 100 entries are processed
    (the framework correctly handles the
    shutdown-mid-processing case)
- New test `test_gc_thread_empty`:
  - Mount, wait for 1 second (10 backoff cycles),
    verify the thread is still running but idle
  - Push 1 entry, verify the thread processes it
    within 1 backoff cycle
- Bench delta: 0 (the GC thread is idle in the
  bench's workload; the thread overhead is in noise
  for the bench's per-op measurements)

**Files touched**:

- `include/ixsphere/vfs_internal.h` — add
  `pthread_t gc_thread`, `volatile int gc_shutdown`
- `src/gc.c` — remove old implementation, add thread
  infrastructure
- `src/gc.h` — update public API
- `src/vfs.c` — `vfs_mount`, `vfs_unmount` updated
  to spawn/stop the thread
- `test/test_gc.c` (new) — new test file for GC
  thread tests
- `include/ixsphere/vfs.h` — update `vfs_gc` doc

**Commit**: `phase-28-w2: GC thread infrastructure (start/stop/loop/scheduling)`

### Workload 3: Producer integration with placeholder NOOP trigger

**Scope**:

- Add `bin_push` calls to the 7 garbage-producing
  public operations, using `BIN_TRIGGER_NOOP` as the
  placeholder type:
  - `vfs_delete` (src/tree.c) — push after the
    tombstone is added
  - `vfs_truncate` (src/tree.c) — push after the
    shrink FileSize entry
  - `vfs_delete_snapshot` (src/epoch.c) — push after
    the soft-delete mapper entry
  - `vfs_rollback` (src/epoch.c) — push after the
    soft-delete mapper entry
  - `vfs_rename` (src/tree.c) — push after the
    tombstone is added
  - `vfs_commit` (src/epoch.c) — push after the
    committed mapper entry
  - `mirror_write` failure path (src/lazy_mirror.c) —
    push after `storage_free(sibling)`
- Each `bin_push` call carries the operation's context
  (file VP, snapshot epoch, etc.) in the `context`
  field. The `context2` field is set to 0 for the
  NOOP placeholder.
- The `BIN_TRIGGER_NOOP` type is the only entry type
  the GC thread processes (per the framework's
  initial scope). The GC dispatches the NOOP trigger
  to a no-op handler (just delete the entry, do
  nothing else).
- Update ISSUES.md to document the framework
  (no entries are closed in W3 — the framework is
  infrastructure, not a fix)

**Test gate**:

- All existing ctest suites pass
- New test `test_bin_producer_integration`:
  - Mount, perform a `vfs_delete`, verify a NOOP
    trigger is pushed to the Bin
  - Wait for the GC thread to process it, verify the
    Bin is empty
- New test `test_bin_producer_persistence`:
  - Mount, perform 5 `vfs_delete`s
  - Immediately unmount (before the GC thread
    processes them)
  - Remount, verify the 5 NOOP triggers are still in
    the Bin
  - Wait for the GC thread to process them, verify
    the Bin is empty
- New test `test_bin_producer_concurrent`:
  - 4 threads each perform 100 `vfs_delete`s on
    different files
  - Verify 400 NOOP triggers are pushed to the Bin
  - Wait for the GC thread to drain, verify the Bin
    is empty
- Bench delta: +10-20 ns per `vfs_delete` (and other
  operations that push to the Bin). For the bench's
  workload (which doesn't do many deletes), this is
  in noise. For a delete-heavy workload, it would be
  measurable (~5% of the per-op cost).

**Files touched**:

- `src/tree.c` — add `bin_push` calls in
  `vfs_delete`, `vfs_truncate` (shrink), `vfs_rename`
- `src/epoch.c` — add `bin_push` calls in
  `vfs_delete_snapshot`, `vfs_rollback`, `vfs_commit`
- `src/lazy_mirror.c` — add `bin_push` call in
  `mirror_write` failure path
- `src/gc_dispatch.c` (new) — the dispatch switch
  with the `BIN_TRIGGER_NOOP` case
- `test/test_producer_integration.c` (new) — new
  tests
- `ISSUES.md` — document the framework; no entries
  closed

**Commit**: `phase-28-w3: producer integration with placeholder NOOP trigger`

### Workload 4: Framework end-to-end test (full system validation)

**Scope**:

- Add a comprehensive end-to-end test that exercises
  the full framework: producers push, GC thread pops,
  entries are processed, Bin is drained
- Add a test that verifies the framework's behavior
  under mount/unmount cycles (Bin persistence)
- Add a test that verifies the framework's behavior
  under crash (simulated by killing the VFS mid-
  processing)
- Add a test that verifies the framework's behavior
  under concurrent producers (multiple FUSE worker
  threads pushing simultaneously)
- Update ISSUES.md: framework is in place; the
  per-bin-job work functions will close the
  remaining GC-coupled issues (M4, N2, N3) in
  subsequent phases
- Add a section to the user-facing docs (if any)
  explaining the framework

**Test gate**:

- All existing ctest suites pass
- New test `test_bin_end_to_end`:
  - Mount
  - 4 producer threads, each performing a mix of
    `vfs_create`, `vfs_write`, `vfs_delete`,
    `vfs_commit`
  - After all producers finish, wait for the GC
    thread to drain the Bin
  - Verify the Bin is empty
  - Verify all operations succeeded (no data loss)
- New test `test_bin_crash_safety`:
  - Mount, push 100 NOOP entries
  - Kill the VFS process (simulated via
    `abort()`-equivalent or by closing the fd
    without unmount)
  - Remount, verify the 100 NOOP entries are still
    in the Bin
  - Wait for the GC thread to process them, verify
    the Bin is empty
- New test `test_bin_performance`:
  - Measure the cost of `bin_push` (push 1000
    entries in a tight loop, measure throughput)
  - Measure the cost of `bin_pop` (push 1000
    entries, then pop 1000 in a tight loop, measure
    throughput)
  - Verify the costs are within the framework's
    predicted ranges (~10-20 ns per push, ~10-20 ns
    per pop)
- Bench delta: 0 (the framework is the same; this
  workload is testing, not implementation)

**Files touched**:

- `test/test_bin_e2e.c` (new) — new test file
- `ISSUES.md` — document the framework's state; list
  the per-bin-job work functions that will be added
  in subsequent phases
- `README.md` (if exists) — add a section on the
  framework

**Commit**: `phase-28-w4: framework end-to-end test (full system validation)`

## What's NOT in this spec (the actual bin jobs)

The framework is complete after W4. The 5 garbage types
and 7 public operations that produce them are handled by
**per-bin-job specs** that are added in subsequent
phases. Each per-bin-job spec:

- Follows the per-bin-job spec section structure (§5)
- Plugs into the framework via the dispatch table
  (§7.5)
- Adds its trigger and work types to the framework's
  `bin_trigger_type_t` and `bin_work_type_t` enums
- Is reviewed independently by the external reviewer
  (one review per bin job)

The first per-bin-job spec is **Type 1: Free pages from
file deletion** (the most complex, per the brainstorming
doc). It's recommended to be the first because:

- It exercises the full trigger/work split (the trigger
  pushes multiple work entries, including a tombstone
  removal and a batched free-pages)
- It exercises the per-bin-job lock model (the
  trigger's analysis takes a per-file lock; the work
  entries' page frees are unlocked)
- It exercises the per-bin-job crash safety (the work
  is idempotent at the entry level, but the
  batching/free mechanism has multiple crash windows)

The second per-bin-job spec is recommended to be
**Type 4: Committed mapper entry + chain rewriting**
(the most subtle, per the brainstorming doc). It's
recommended to be the second because:

- It exercises the chain-walk race (the GC walks
  chains concurrently with readers; the lock model
  is the CAS-based rewrite)
- It exercises the chain rewriting's idempotency
  (re-writing an already-rewritten entry is a no-op)
- It exercises the mapper's soft-delete / committed
  semantics (two entry types in the same chain)

The remaining per-bin-job specs (Type 2 tombstone,
Type 3 soft-delete, Type 1 free pages from snapshot
delete / truncate / mirror_write failure) follow.

## Performance analysis

**Baseline (post-Phase-27, pre-this-phase)**:

- create 1t: 13,150 ops/sec (76 µs/op)
- create 4t: 13,048 ops/sec
- small_create 1t: 12,400 ops/sec
- write 1t: 12,481 ops/sec

**Expected delta from this phase**:

- Producer push (W3, `vfs_delete` etc.): +10-20 ns
  per operation. For the bench's workload (which
  doesn't do many deletes), this is in noise. For a
  delete-heavy workload, it would be measurable
  (~5% of the per-op cost).
- GC thread overhead (W2): the thread is idle in the
  bench's workload (no Bin entries). The thread
  overhead is in noise for the bench's per-op
  measurements.
- Mount validation (W1): the validation walk is
  O(Bin pages). For an empty Bin, it's 0 ns. For a
  Bin with 1000 entries, it's ~10-20 µs (a few page
  reads). This is in noise for the bench's mount time
  (~10-50 ms).
- Bin pop (W2, GC thread only): the GC thread is the
  only consumer. The 10-20 ns is in noise for the
  GC's per-entry processing (microseconds to
  milliseconds per entry).

**Bench workload** (created as part of the migration,
in W4):

- A new bench mode `bench_bin` that pushes 1000
  Bin entries in a tight loop, then pops 1000 in a
  tight loop. Measures push and pop throughput
  independently.

**Overall verdict**: this phase is "in noise" for the
current benchmarks. The wins are for the per-bin-job
work functions (added in subsequent phases) and for
the eventual SQLite VFS shim (Phase 29) which will
exercise the Bin heavily.

## Open questions for reviewer

The framework spec has **5 open design questions**.
Per-bin-job specs may have additional open questions
(per the §5.9 template).

1. **Backoff strategy**. The spec uses constant
   backoff (`GC_BACKOFF_US = 100ms`). Is constant
   sufficient for the expected workload, or should
   the spec commit to adaptive backoff
   (exponential on continued empty, reset on
   first non-empty)?

2. **Shutdown drain vs stop-and-leave**. The spec
   uses stop-and-leave (the GC thread exits on the
   shutdown signal; unprocessed entries are left
   for the next mount). Is stop-and-leave
   sufficient, or should the spec commit to
   shutdown drain (wait for the Bin to drain
   before exiting)?

3. **Bin page capacity**. The spec uses a fixed
   `BIN_PAGE_CAPACITY = 510` (for 8 KB pages) or
   `255` (for 4 KB pages), computed from the page
   size. Is this acceptable, or should the spec
   support a configurable capacity (e.g., for
   testing)?

4. **`bin_count` cache-line contention**. The spec
   documents this as a known post-MVP concern.
   Should the spec commit to padding
   `bin_count` to a separate cache line as part
   of W1, or defer to a future optimization?

5. **Per-bin-job work function file layout**. The
   spec proposes `src/gc_trigger_*.c` and
   `src/gc_work_*.c` files. Is this layout
   acceptable, or should the spec commit to a
   different layout (e.g., one file per bin job
   with both trigger and work handlers)?

## Risk assessment

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Mount validation walk has a bug (rebuilds count incorrectly) | Low | Mount succeeds but Bin is corrupted; GC processes garbage entries | Comprehensive test coverage in W1; validation walk uses raw `pread` (no cache interaction) |
| GC thread fails to join on shutdown | Low (1-second timeout) | Thread is detached; VFS closes; thread continues in background | Detached thread is leak-free (Bin is persistent; thread eventually exits when the Bin drains or the process exits) |
| Per-bin-job work function is NOT idempotent | Low (caught in review) | Crash mid-processing corrupts the tree | Per-bin-job spec template requires an explicit "Crash safety" subsection; the review checks it |
| Producer's `bin_push` ordering is wrong (data committed before Bin entry) | Low (per-bin-job review) | Crash leaves data on disk without a Bin entry to clean it up | Per-bin-job spec template requires an explicit "Producer push call site" (Subsection 2); the review checks the ordering |
| `bin_count` cache-line contention under load | Low (currently low write frequency) | Hot-path slow under high GC load | Padding `bin_count` to a separate cache line; deferred to post-MVP optimization |
| Producer-consumer cycle in `bin_push` (push → allocate Bin page → allocate Bin page uses Bin) | None (mitigated by `storage_allocate_bin_page`) | N/A | Internal helper bypasses the free-list check (same as Phase 27 W3's `storage_allocate_tail_advance`) |
| ABA on `bin_head` (H1 → H2 → H1) | Very low (forward-only chain) | Duplicate entry processed | Per-page CAS on `count` is the ABA detector; the count's monotonic nature makes ABA rare |
| Crash between Bin CAS and flush | Very low (microsecond window) | Mount-time validation rebuilds the count; the lost entries are garbage-delayed, not data-corrupting | Mount-time validation walk in W1 |
| Multiple GC threads in the future | Not in this spec | Future work | Framework's CAS pattern is correct for multiple consumers; future optimization |

## Commit plan

4 commits, one per workload:

```
phase-28-w1: Bin infrastructure (storage + push/pop/peek + mount validation)
phase-28-w2: GC thread infrastructure (start/stop/loop/scheduling)
phase-28-w3: producer integration with placeholder NOOP trigger
phase-28-w4: framework end-to-end test (full system validation)
```

After W4, the framework is complete. The per-bin-job
specs follow as separate commits (one or more per bin
job, each reviewed independently).

## Source

- `impl/phase-28-gc-free-pages.md` — the brainstorming
  doc. Contains the free-page path analysis (6 paths
  across 3 garbage producers), the broader scope
  analysis (5 garbage types, 7 garbage producers), the
  Bin design sketch, the trigger/work split rationale,
  the per-bin-job lock model rationale, and the
  page-free-doesn't-need-lock analysis. This spec
  distills the framework parts of the brainstorming
  doc into a reviewable spec; the per-bin-job work
  function details remain in the brainstorming doc and
  are pulled into per-bin-job specs as those specs are
  written.

- `impl/phase-27-free-page-queue.md` — the spec for the
  free-page queue. The Bin follows the same pattern
  (header fields + per-page CAS + mount-time validation
  walk). Phase 27 W6's per-page CAS for the dequeue is
  the template for the Bin's per-page CAS for push/pop.

- `src/gc.c` — the existing GC implementation. Being
  replaced by the framework. The existing
  `DeferredFreeQueue`, `tree_lock`, and
  shadow-compaction are dropped; the per-bin-job specs
  define their own per-job concurrency model.

- `src/gc.h` — the existing GC interface. Updated in W2
  to remove the old API and add the framework's
  thread-related declarations.

- `src/storage.h` — updated in W1 with the new Bin
  header offsets (64, 72, 80) and the reserved padding
  offset (88).

- `src/indirection.c` — updated in W1 to use the new
  inline count math (offset 40 → offset 96).

- `include/ixsphere/vfs_internal.h` — updated in W2
  with the `gc_thread` and `gc_shutdown` fields.

- `include/ixsphere/vfs.h` — updated in W2 to document
  the new `vfs_gc` behavior (returns
  `VFS_ERR_NOTIMPL` until per-bin-job work functions
  are added in subsequent phases).

- `ISSUES.md` — M4, N2, N3 are documented as
  GC-coupled issues that will be unblocked by the
  framework's per-bin-job isolation. The framework
  itself closes no ISSUES.md entries (it's
  infrastructure, not a fix); per-bin-job specs close
  entries as their work is implemented.
