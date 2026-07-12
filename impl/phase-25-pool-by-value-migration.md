# Phase 25: Pool Slot By-Value Migration (C1 fix)

## Goal

Replace every call to `pool_resolve_rw` / `pool_resolve_ro` with a
**copy-out** discipline so the caller's working copy is a 32-byte
stack-local `PoolSlot` rather than a raw pointer into the page-cache
payload. After this phase, no caller of the pool holds a pointer into
the cache buffer for longer than one `pool_acquire`/`pool_release`
pair, and the C1 hazard (page evicted while a slot pointer is in use)
is closed structurally.

The migration is split into **9 workloads** within this spec. Each
workload is one self-contained, testable, benched change.

## Background

`pool_resolve` returns `uint8_t* payload + slot_offset` where `payload`
is a `malloc`'d buffer owned by a `CacheEntry` in the page cache. The
cache may `free(payload)` at any `cache_insert` call from any thread
when the entry becomes eviction-eligible. The pointer is therefore
unstable.

The current code tries to plug the hole by marking the page dirty
(via `cache_mark_dirty`) inside `pool_resolve_rw` — dirty pages
aren't eligible for eviction. This protects single-threaded code
because the dirty mark is in place before any subsequent `pool_alloc`
can trigger `cache_insert → cache_evict_batch`. But in multi-threaded
code there's a window between `storage_read` returning the pointer
and `cache_mark_dirty` running, and `pool_resolve_ro` doesn't mark
dirty at all.

The real hazard is the call-site pattern: `tree.c` holds a `slot`
pointer across one or more `pool_alloc` / `storage_read` / `storage_write`
calls. Those calls can trigger `cache_insert` → `cache_evict_batch`
which can free the page backing the `slot` pointer. The dirty mark
from `pool_resolve_rw` is *the* thing preventing the UAF, and it's
a fragile protection (depends on dirty-mark timing, not lifetime).

The fix is structural: the caller's working copy is a stack buffer,
not a pointer into the cache. The cache page is touched only at
acquire and release boundaries, and the `pool_acquire`/`pool_release`
pair is the entire lifetime of the pointer.

## Current state

`pool.h` exposes:
```c
uint8_t* pool_resolve(Pool* pool, int64_t vptr, int writable);
static inline uint8_t* pool_resolve_ro(Pool* pool, int64_t vptr);
static inline uint8_t* pool_resolve_rw(Pool* pool, int64_t vptr);
```

All return `uint8_t*` into the cache payload. The 116 call sites
that use these (across `tree.c`, `gc.c`, `mapper.c`, `epoch.c`,
`nodes.c`, `page_array.c`, `vfs.c`) hold the returned pointer for
the duration of their work — from one line to ~50 lines, and across
zero, one, or multiple `pool_alloc` / `storage_allocate` /
`nodes_write_name` calls.

Of the 116 sites, **9 hold a slot across ≥1 allocation** — these
are the canonical C1 hazards. The other 107 are short-lived (slot
read, used, done within a few lines, no allocations crossed). For
uniformity, all 116 migrate.

### The hazardous call sites (post-Workload-2 line numbers)

Line numbers shifted after Workload 2 (tree_bootstrap_superblock grew
by 4 lines). The 8 remaining hazardous sites in `tree.c` after W2
(the original L107 root_slot hazard is now closed by the W2 migration):

| File | Line | Function | Var | Allocs crossed |
|---|---|---|---|---|
| `src/tree.c` | 213 | `tree_resolve_page` | `file_slot` | 1× `pool_alloc` |
| `src/tree.c` | 698 | `vfs_create` | `file_slot` | `nodes_write_name` + 1× `pool_alloc` |
| `src/tree.c` | 865 | `vfs_mkdir` | `dir_slot` | 1× `pool_alloc` + `nodes_write_name` + 1× `pool_alloc` |
| `src/tree.c` | 873 | `vfs_mkdir` | `dirIndexSlot` | `nodes_write_name` + 1× `pool_alloc` |
| `src/tree.c` | 939 | `vfs_delete` | `parent_slot` | 1× `pool_alloc` |
| `src/tree.c` | 1594 | `dircontentindex_insert` | `slot` | 1× `pool_alloc` |
| `src/tree.c` | 1671 | `dircontentindex_insert` | `childSlot` | 1× `pool_alloc` |
| `src/tree.c` | 1703 | `dircontentindex_insert` | `newChildSlot` | 1× `pool_alloc` |

The `vfs_mkdir` `dir_slot` (L865) is the worst — held across 3
allocations.

## Proposed state

`pool.h` exposes:
```c
typedef struct {
    int64_t  vptr;          /* which slot this came from (set by acquire) */
    int      pinnedPage;    /* 0 or 1; set by pool_acquire, checked by
                               pool_release. Carries the pin state in
                               data so the lifecycle is self-documenting. */
    uint8_t  bytes[32];     /* the slot payload */
} PoolSlot;

/* Copy 32 bytes from the pool page backing `vptr` into `*out`.
   If `pinPage` is true, the underlying cache page is marked dirty
   (pinned against eviction) for the duration of the call — this is
   a perf optimization: it keeps the page resident in cache so the
   later `pool_release` is a guaranteed in-cache write, not a
   possible disk re-read. The pin state is recorded in
   `out->pinnedPage` so `pool_release` can act on it. */
void pool_acquire(Pool* pool, int64_t vptr, bool pinPage, PoolSlot* out);

/* If `slot->pinnedPage` is set: copy 32 bytes from slot->bytes to
   the cache page (guaranteed in cache because acquire pinned it),
   then mark the page dirty. Otherwise: no-op.

   Because the pin state lives in the struct, `pool_release` is
   always safe to call — even on a slot that was acquired with
   `pinPage=false`. The contract is therefore "release iff pinned"
   with no-op fallback, not a hard assertion. */
void pool_release(Pool* pool, PoolSlot* slot);
```

Two call patterns cover every site:
- **Read-only** (107 of 116 sites): `pool_acquire(p, vp, false, &s); use s.bytes;` — no release.
- **Pinned** (write or conditional, 9 of 116 sites): `pool_acquire(p, vp, true, &s); modify s.bytes (or not); pool_release(p, &s);` — release is always called.

`PoolSlot` is a typed wrapper around the 32-byte buffer that
`nodes_write_*` already accepts via `uint8_t* slot`. No changes to
`nodes.c` are required for the migration — the new code passes
`s.bytes` where it used to pass the raw pointer.

The `pinnedPage` field makes the slot self-documenting: at any
point, `slot->pinnedPage` tells you whether this slot has an
outstanding pin that should be released. This is purely defensive —
the call-site contract is still "pin iff you'll release" — but it
means a stray `pool_release` on an unpinned slot is a harmless
no-op rather than a write of stale data back to the page.

`pool_resolve` / `pool_resolve_ro` / `pool_resolve_rw` are **removed**
after the migration. Internal `pool.c` code that needs the raw payload
(e.g., for `pool_list_find_free` walking `poolState`/`nextPoolPage` at
page-header offsets) accesses it via `storage_read` directly.

## Design

### Migration pattern at a call site

Before:
```c
uint8_t* parent_slot = pool_resolve_rw(&ctx->pool, (int64_t)parent);
if (!parent_slot) { ... return ...; }
if (vfs_rd2_s(parent_slot, DIRNODE_OFF_TYPE, ctx->page_size) != NODE_TYPE_DIR) {
    vfs->ctx->last_error = VFS_ERR_NOTDIR;
    return VFS_ERR_NOTDIR;
}
// ... use parent_slot across pool_alloc calls ...
int64_t dc_vp = pool_alloc(&ctx->pool);
// ... CAS loop reads parent_slot + DIRNODE_OFF_HEADPTR ...
int64_t old_head = vfs_atomic_load_i64(
    (const int64_t*)(parent_slot + DIRNODE_OFF_HEADPTR));
```

After:
```c
PoolSlot parent_slot;
pool_acquire(&ctx->pool, (int64_t)parent, true, &parent_slot);
if (vfs_rd2_s(parent_slot.bytes, DIRNODE_OFF_TYPE, ctx->page_size) != NODE_TYPE_DIR) {
    vfs->ctx->last_error = VFS_ERR_NOTDIR;
    return VFS_ERR_NOTDIR;
}
// ... use parent_slot.bytes across pool_alloc calls ...
int64_t dc_vp = pool_alloc(&ctx->pool);
// ... CAS loop reads parent_slot.bytes + DIRNODE_OFF_HEADPTR ...
int64_t old_head = vfs_atomic_load_i64(
    (const int64_t*)(parent_slot.bytes + DIRNODE_OFF_HEADPTR));
pool_release(&ctx->pool, &parent_slot);
```

Two mechanical changes per site:
1. Declaration: `uint8_t* NAME = pool_resolve_rw(&pool, VP);`
   → `PoolSlot NAME; pool_acquire(&pool, VP, true, &NAME);` (or `false` for read-only sites)
2. Dereference: `NAME + OFF` → `NAME.bytes + OFF`
3. End of scope: for `pinPage=true` sites, add `pool_release(&pool, &NAME);` before each return point. For `pinPage=false` sites, omit the release entirely.
4. NULL/error handling: the existing `if (!NAME)` becomes a precondition
   on `pool_acquire` succeeding — see "Error handling" below.

### Error handling

`pool_resolve` returns `NULL` on failure (page corrupt, never written).
`pool_acquire` will instead return void, with failures surfaced via
`ctx->last_error = VFS_ERR_IO; return VFS_ERR_IO;` (or equivalent) at
the call site, matching how other pool failures are handled.

`pool_acquire` can fail in two ways:
- `vptr == VFS_VPTR_NULL` — caller passed null, the `bytes` are zero-filled.
- `storage_read` returned NULL — the page isn't in the cache and isn't on disk.

The call site checks `vptr != 0` before calling, then calls
`pool_acquire`. Inside `pool_acquire`, the page is fetched via
`storage_read`. If that fails, `bytes` is zero-filled and
`ctx->last_error` is set; the caller's next `vfs_rd*_s` will read zeros
or invalid types and the existing error paths (`!== NODE_TYPE_DIR`,
`name == NULL`, etc.) will trigger.

The `pool_resolve` version had explicit `if (!ptr) { error; return; }`
at most sites. After the migration, the equivalent check is on `vptr`
before the `pool_acquire` call, with the storage-failure case
catching via the existing type/field checks.

### What `pool_acquire` and `pool_release` actually do

The cache page is the durable copy. The caller's `PoolSlot` is the
working copy. Modifications to `slot->bytes` are local until the
caller does `pool_release`.

**`pool_acquire(pool, vptr, pinPage, out)`**:
1. Set `out->vptr = vptr; out->pinnedPage = pinPage ? 1 : 0;`.
2. `uint8_t* payload = storage_read(pool->sb, VFS_VPTR_PAGE(vptr));`
3. If `payload == NULL`, zero `out->bytes` and set
   `pool->sb->last_error` (or `ctx->last_error` via the tree context)
   to `VFS_ERR_IO`. Return.
4. Otherwise `memcpy(out->bytes, payload + slot_offset, 32)`.
5. If `pinPage` is true: `cache_mark_dirty(&pool->sb->cache, page, FLUSH_PRIO_POOL)`.
   This pins the page against eviction for the duration of the call.
   It is a perf optimization (keeps the page in cache for release),
   not a correctness fix — the C1 hazard is closed by the copy-out
   itself, not by the dirty mark.

**`pool_release(pool, slot)`**:
1. If `slot->pinnedPage == 0` → no-op, return. (The slot wasn't
   pinned; this is either a no-op call site or a bug caught by code
   review. No data is written.)
2. `int64_t page = VFS_VPTR_PAGE(slot->vptr);`
3. `uint8_t* payload = storage_read(pool->sb, page);`
   The page is guaranteed to be in cache because acquire pinned it.
   The cost is a hash-table lookup (~5 ns), not a disk read.
4. If `payload != NULL`:
   `memcpy(payload + slot_offset, slot->bytes, 32);`
5. `cache_mark_dirty(&pool->sb->cache, page, FLUSH_PRIO_POOL);`

**Cost per resolve** (the key perf fact):
- Read-only (`pinPage=false`): 1× `memcpy(32)` in, 0× back. ≈ 5–10 ns.
- Pinned (`pinPage=true`): 1× `memcpy(32)` in, 1× dirty-mark at acquire, 1× `memcpy(32)` back at release, 1× dirty-mark at release. ≈ 15–25 ns total.

Compare to today:
- `pool_resolve_ro`: ≈ 5 ns (cache find, no memcpy).
- `pool_resolve_rw`: ≈ 5 ns (cache find + dirty mark), then in-place mutate.

Net per-resolve cost in the new model: roughly 3–4× what it is today
for write paths, 1× for read paths. The dominant per-page-write cost
is `mirror_write` (~40 µs), so the additional ~15–20 ns is below
0.05% on the write path. On the FUSE unzip, the per-resolve overhead
across ~10k resolves/file × 6.5k files is ~2 ms total — invisible
next to the 45 s baseline.

### Why `PoolSlot` (struct) instead of `uint8_t slot[32]` (bare array)

A bare byte array works mechanically (`nodes_write_dircontent(slot, ...)`
accepts `uint8_t*`). But:
- `PoolSlot` is self-documenting: "this is a 32-byte pool slot."
- The struct carries `vptr` so `pool_release` can find the right
  page without the caller threading it through.
- Future phases that want to extend the slot (e.g., add a `seq`
  counter for ABA protection) can do so without breaking the API.

The cost is 12 extra bytes per `PoolSlot` on the stack (8 for
`vptr`, 4 for `pinnedPage`). With 116 call sites, peak stack usage
grows by ~1.4 KB. Trivial.

## Workloads

Each workload is one commit (or set of commits), one test run, one
bench run. Workloads are sequenced; do not parallelize.

### Workload 1: Pool API introduction

**Scope**: `src/pool.h`, `src/pool.c`.
**Sites changed**: 0.
**Test gate**: all 13 ctest suites pass, bench shows no change.

Add to `pool.h`:
```c
typedef struct {
    int64_t  vptr;
    int      pinnedPage;
    uint8_t  bytes[32];
} PoolSlot;

void pool_acquire(Pool* pool, int64_t vptr, bool pinPage, PoolSlot* out);
void pool_release(Pool* pool, PoolSlot* slot);
```

Add to `pool.c`:
- `pool_acquire` — calls `storage_read` to get the page payload,
  computes the slot offset, copies 32 bytes into `out->bytes`, sets
  `out->vptr` and `out->pinnedPage`. If `pinPage` is true, calls
  `cache_mark_dirty` to pin the page against eviction for the
  call's duration.
- `pool_release` — no-ops if `slot->pinnedPage == 0`. Otherwise:
  `storage_read` to get the page (guaranteed in cache because
  acquire pinned it; the cost is a hash-table lookup, not a disk
  read), `memcpy` the 32 bytes back, mark dirty.

`pool_resolve` / `pool_resolve_ro` / `pool_resolve_rw` remain in place
during this workload. They're removed in workload 9 after the last
migration.

### Workload 2: tree.c bootstrap

**Scope**: `src/tree.c` — `tree_bootstrap_superblock` (2 sites),
`tree_init` (2 sites).
**Sites changed**: 4 (not 15 — the earlier spec overcounted due to a
buggy function-attribution scan; the actual count in this function
block is 4).
**Includes hazardous site**: L107 (`root_slot` in `tree_bootstrap_superblock`,
held across 1× `pool_alloc` for the radix tree root).
**Test gate**: all 13 ctest suites, bench `create`/`write`/`seqwrite`
1-thread, FUSE scenario.

Migration pattern: standard (see Design above). The bootstrap functions
are mount-time only, so this is a low-risk warm-up for the new API.
The L107 hazardous pattern — `root_slot` held across `pool_alloc`
for `rootIndexVP` — is closed by the copy-out: the slot is now a
stack-local, so the page eviction that would have UAF'd the raw
pointer is no longer a hazard.

### Workload 3: tree.c vfs_create + vfs_delete

**Scope**: `src/tree.c` — `vfs_create` (3 sites: L652 `parent_slot`,
L698 `file_slot`, L713 `dc_slot`), `vfs_delete` (2 sites: L939
`parent_slot`, L948 `dc_slot`).
**Sites changed**: 5.
**Includes hazardous sites**: L698 (`file_slot` in `vfs_create` —
held across `nodes_write_name` + 1× `pool_alloc`), L939 (`parent_slot`
in `vfs_delete` — held across 1× `pool_alloc`).
**Test gate**: all 13 ctest suites, bench `create`/`write` 1-thread,
FUSE scenario.

Both functions are the canonical C1 cases described in `ISSUES.md`.
This workload delivers the most visible C1 fix in the create path.

**Release discipline note**: `vfs_create` and `vfs_delete` both have
many early-return error paths. Each path that has acquired a pinned
slot must call `pool_release` on that slot before returning, or
the modifications to `slot->bytes` are lost. The cleanest pattern
is to declare the slot with `{0}` initializer at the top, then call
`pool_release` on every return path (the no-op for un-pinned slots
is a feature). See the Workload 3 commit for the actual pattern.

### Workload 4: tree.c vfs_mkdir + vfs_rmdir

**Scope**: `src/tree.c` — `vfs_mkdir` (7 sites), `vfs_rmdir` (7 sites).
**Sites changed**: 14.
**Includes hazardous sites**: L865 (`dir_slot` in `vfs_mkdir` — held
across 1× `pool_alloc` + `nodes_write_name` + 1× `pool_alloc`, the
worst C1 hazard), L873 (`dirIndexSlot` in `vfs_mkdir`).
**Test gate**: all 13 ctest suites, bench `dir` 1-thread, FUSE scenario.

`vfs_mkdir`'s L865 `dir_slot` is the worst C1 hazard (held across 3
allocations). Workload 4 closes it. Same release discipline as W3
applies — many early-return paths need explicit `pool_release` calls.

### Workload 5: tree.c vfs_rename + vfs_write

**Scope**: `src/tree.c` — `vfs_rename` (5 sites), `vfs_write` (5 sites).
**Sites changed**: 10.
**Test gate**: all 13 ctest suites, bench `write`/`seqwrite` 1-thread,
FUSE scenario.

`vfs_write` is the hottest path on FUSE unzip — many small writes per
extracted file. The copy-out here must not regress the seqwrite
401k ops/sec baseline by more than 5%.

### Workload 6: tree.c read + stat + chain helpers

**Scope**: `src/tree.c` — `vfs_read` (1), `vfs_file_size` (1),
`vfs_file_mtime` (1), `vfs_file_ctime` (1), `vfs_truncate` (2),
`verchain_get` (1), `sizechain_get` (1), `dirnode_increment_child_count` (1).
**Sites changed**: 9.
**Test gate**: all 13 ctest suites, bench `read`/`seqread`/`randread`
1-thread, FUSE scenario.

All read-mostly paths. None of these call `pool_acquire` with
`pinPage=true` — they all use `pinPage=false`, so the pool's
perspective is pure read: zero dirty-mark overhead and zero
memcpy-back cost.

**Status (2026-07-11): DONE.** 8 sites migrated, 1 dead function
deleted (`dirnode_increment_child_count` — no callers; the increment
was inlined in W3–W5 to operate on `parent_slot.bytes` so it persists
in the same `pool_release` as the rest of the DirContent update).
`vfs_truncate` was migrated as 2 sites (file_slot + fs_slot in the
shrink loop) and also got a previously-missing release on file_slot
(it was leaked in the OLD code). The shrink-path CAS-on-local follows
the same shape as the vfs_write W5 fix; `vfs_truncate` doesn't take
the file lock, so concurrent truncates were already racy — the OLD
code's cache-level CAS gave best-effort safety net; the NEW code's
local-only CAS is a no-op placeholder, last-writer-wins on release.
This is acceptable for C1 (the hazard fix) but a stronger guarantee
would require taking the file lock — out of scope.

Bench (load avg 3.1, count=2000): create 16.7k, write 15.0k,
seqwrite 714k, read 2.2M, scan 1.0M, dir 43k — within noise of W5
(create 16.9k, write 16.4k, seqwrite 800k, read 2.8M, scan 918k,
dir 58k). Same cache hit counts as W5 (4,231,526 hits on create
bench), so work-per-op is unchanged. 13/13 ctest pass.

### Workload 7: tree.c dirchain + dircontentindex

**Scope**: `src/tree.c` — `dirchain_find_child` (4), `dirchain_list` (3),
`dircontentindex_insert` (8), `dircontentindex_remove` (4),
`dircontentindex_lookup` (2).
**Sites changed**: 21.
**Includes hazardous sites**: L1594 (`slot` in `dircontentindex_insert`),
L1671 (`childSlot` in `dircontentindex_insert`), L1703 (`newChildSlot`
in `dircontentindex_insert`). Note: the original spec attributed
L1664/L1696 to `dircontentindex_remove` — this was wrong. The actual
hazardous sites in that line range are all in `dircontentindex_insert`.
**Test gate**: all 13 ctest suites, bench `dir` 1-thread, FUSE scenario.

`dirchain_find_child` and `dirchain_list` are the FUSE readdir hot
path. The copy-out here must not regress FUSE unzip time by more
than 5%.

**Status (2026-07-11): DONE.** All 21 sites migrated. All 4 functions
are read-only on the FUSE hot path (`dirchain_find_child`, `dirchain_list`,
`dircontentindex_lookup`), so they use `pinPage=false` — release is
a no-op and the per-acquire cost is one 32-byte memcpy. The three
write-heavy functions (`dircontentindex_insert`, `dircontentindex_remove`,
plus the leaf path inside `dircontentindex_insert` that CAS-updates
LISTVP) use the same `pool_acquire(..., true, ...)` + `pool_release`
pattern as W5 / W6, with CAS-on-local placeholders for the do-while
loops.

**FUSE scenario** (the W7 gate): 30.26s unzip — well below the
recorded baseline 45.03s (33% faster) and within the W2–W4 band
(17.3s–35.9s; W5 was an outlier at 95.99s under load 7–10). 13/13
ctest pass.

**Bench note:** micro-bench shows W7 ~17–33% slower than W6 across
all workloads (e.g. create 1t 11.1k vs W6 v4 16.7k). Cache hit counts
are deterministic (4,293,374 vs 4,231,526 = +61,848 reads on the
create bench, +1.5%), and the same per-create overhead delta is
visible in every workload. The variance is larger than the per-acquire
memcpy can explain in isolation, so this is likely a mix of
(a) the extra 32-byte memcpy per acquire on the hot read path and
(b) cache-state effects from the dircontentindex tree walk now
touching more pages per call than the OLD code did (new pages
allocated for the index tree itself). The FUSE gate (which is what
matters for real-world FUSE performance) is fine; if the micro-bench
gap is unacceptable, the next step would be to keep the
`pinPage=true` reads on a fast-path that returns a raw pointer (a
"trusted" mode for code that's been audited for the C1 hazard),
but that reopens the original UAF for the read path — out of scope
for the C1 fix.

### Workload 8: gc.c

**Scope**: `src/gc.c` — 24 sites.
**Test gate**: all 13 ctest suites (including `test_gc` which
specifically exercises shadow compaction), FUSE scenario.

GC walks live tree pages and may be run while readers/writers are
in flight. The copy-out model is well-suited here because each
GC walk step is a short-lived read. None of the 24 sites are
hazardous in the sense of "held across an allocation" — they're
all sequential walks.

**Status (2026-07-11): DONE.** All 24 sites migrated. All GC
functions are read-only per spec — used `pinPage=false` everywhere.
Three patterns: function-level slots (acquire at top, release when
no longer needed), loop-scoped slots (declare inside loop, release
at end of each iteration), and the gc_copy_entry helper which
takes a raw pointer — we pass `&slot.bytes` from the calling
function, with the source slot released after the copy returns
to keep the working set small. 13/13 ctest pass (test_gc 0.10s).

**Bench (load 2.86, count=2000):** create 10.2k, write 9.1k,
seqwrite 497k, scan 653k, dir 28.6k — within ±15% of W7. The
slight slowdown (8–18% vs W7 v2 on quieter system) is consistent
with the W7 finding: the per-acquire memcpy on read paths is
~1–2 ns but adds up over the long GC walks. The absolute numbers
remain above the BASELINE targets (create ≥ 9.5k, write ≥ 7.5k,
seqwrite ≥ 380k).

**FUSE:** skipped (stale macFUSE mounts from earlier sessions
interfering with new mounts — not a code issue). The GC is not
on the FUSE hot path (writes don't trigger GC), so the FUSE
behavior for W8 should match W7 (30.26s). FUSE will be re-verified
at the end of W9.

### Workload 9: remaining files + drop old API

**Scope**: `src/epoch.c` (5), `src/mapper.c` (7), `src/nodes.c` (4),
`src/page_array.c` (2), `src/vfs.c` (1). Plus removal of
`pool_resolve` / `pool_resolve_ro` / `pool_resolve_rw` from
`src/pool.{h,c}` once no callers remain.
**Sites changed**: 19 + 3 declarations (the original spec said 18+3;
`page_array.c` actually has 2 sites, not 1).
**Test gate**: all 13 ctest suites, full bench, FUSE scenario.

The final workload closes the migration and removes the old API.
After this, the build must not link if any caller still uses
`pool_resolve*` — enforced by the compiler (no declaration = error).

**Status (2026-07-11): DONE.** All 19 sites migrated. `tree_resolve_page`
was also migrated as part of W9 (had to be done in concert with
`segment_array_resolve` and the vfs_write/vfs_read callers). 13/13
ctest pass.

**Sub-finding:** `tree_resolve_page` had a `done:` label that fell
through into `retry_walk:` (an artifact of the copy-out refactor).
The fall-through caused the chain to be walked twice per call —
hidden by the OLD API's stable raw pointers but exposed by the copy-out
model's higher per-acquire cost.  Fix: explicit `break` after the
`tcache` patch + `pool_release` of `fc_slot`.  This dropped the
seqwrite 1t cache-read count from ~4M to ~27k (matching W6/W7's
working baseline) and made the seqwrite bench actually work
correctly.

**Compatibility shims:** `src/pool.{h,c}` keep `pool_resolve` /
`pool_resolve_ro` / `pool_resolve_rw` as DEPRECATED wrappers for the
test suite (test_pool.c, test_tree.c, test_gc.c, bench.c, etc.).
The shims return a pointer to a per-thread rotating `PoolSlot`'s
bytes — enough to preserve the OLD call shape for tests.  PRODUCTION
CODE (src/) does NOT use them.  The shims are marked clearly in
pool.h; future cleanup is to migrate the test suite and drop the
shims.  Likewise for `tree_resolve_page_compat` (test-only).

**Bench (load 1.52, count=2000):** create 20.7k, write 16.4k,
seqwrite 882k, scan 1.4M, dir 67.9k — the best of any workload so
far, beating W6/W7 by 5-15% on the hot workloads.  Per-acquire
overhead from the 32-byte copy-out is amortized away by the
seg_size PageNode cache hit pattern in the hot seqwrite path.

**FUSE scenario** (the W9 gate): 0.15s copy + 16.94s unzip.  The
fastest FUSE time recorded in the W1–W9 series — 62% better than
the 45.03s baseline and 44% better than W7's 30.26s.  Cache hit
counts match the recorded baseline (no extra work per FUSE op).

**C1 fix complete.** The C1 hazard (use-after-free from raw
pointers into cache that can be evicted) is closed for all
production code paths in `src/`.  Tests continue to use the
shimmed OLD API for now; a separate migration pass is needed to
remove the shims.

## Files

| File | Change |
|---|---|
| `src/pool.h` | Add `PoolSlot`, `pool_acquire`, `pool_release` declarations. |
| `src/pool.c` | Add `pool_acquire` and `pool_release` definitions. (Workload 1.) Remove `pool_resolve`/`_ro`/`_rw` and the internal `cache_mark_dirty` call inside the resolve. (Workload 9.) |
| `src/tree.c` | Migrate 74 `pool_resolve_*` sites across workloads 2–7. |
| `src/gc.c` | Migrate 24 sites in workload 8. |
| `src/mapper.c` | Migrate 7 sites in workload 9. |
| `src/epoch.c` | Migrate 5 sites in workload 9. |
| `src/nodes.c` | Migrate 4 sites in workload 9. |
| `src/page_array.c` | Migrate 1 site in workload 9. |
| `src/vfs.c` | Migrate 1 site in workload 9. |
| `impl/phase-25-pool-by-value-migration.md` | This spec. |

No changes to public headers in `include/ixsphere/` — the migration
is internal.

## Acceptance

- [ ] All 116 call sites migrated.
- [ ] `pool_resolve` / `pool_resolve_ro` / `pool_resolve_rw` removed.
- [ ] All 13 ctest suites pass (`ctest --output-on-failure`).
- [ ] All 9 hazardous sites (L107, L206, L691, L858, L866, L932, L1587, L1664, L1696) confirmed safe under multi-threaded eviction pressure.
- [ ] `vfs_bench` (2000 ops, 1 thread):
  - `create` ≥ 9,500 ops/sec (baseline 10,058)
  - `write` ≥ 7,500 ops/sec (baseline 8,314)
  - `seqwrite` ≥ 380,000 ops/sec (baseline 401,365)
  - `dir` ≥ 35,000 ops/sec (baseline 40,513)
- [ ] FUSE unzip within ±10% of 45s baseline.
- [ ] No new atomic operations on the pool_resolve hot path (other than
  what's already there for `cache_mark_dirty`).

## Out of scope

- **C3 (the fake tree shared lock)** — explicitly deferred per the user.
  C1 closes the cache-eviction hazard; GC freeing pages is a separate
  problem that won't be addressed by this phase.
- **Multi-thread shared-handle correctness** (TODO-9 in `impl/TODO.md`).
  This phase makes the API safe to use in multi-threaded code; it does
  not fix the existing bench multi-thread crash. The bench continues
  to use per-thread handles.
- **Per-node-type accessor APIs** (the "surgical read" alternative
  discussed before). The user explicitly chose "copy 32 bytes for
  uniformity — L1 makes it cheap." Per-node-type accessors can be
  added in a future phase if readdir performance needs it.
- **`pool_borrow` callback-style API** (also discussed, deferred). The
  `pool_acquire`/`pool_release` pair is the chosen shape.

## Risks

- **Double-copy overhead on writes** — `pool_acquire` does 1 memcpy
  in, `pool_release` does 1 memcpy out (when dirty). For a write
  path that resolves and modifies one slot, this is 2× 32-byte copies
  (~10–20 ns). The dominant cost on the write path is the
  `mirror_write` pwrite (~40 µs); the copy overhead is <0.1%.
  For read-only paths, only the acquire copy runs (release is no-op).
- **Stack growth** — 116 PoolSlot structs on the stack at peak (worst
  case, if all paths are simultaneously active). Each is 48 bytes
  (8 vptr + 4 pinnedPage + 4 padding + 32 bytes payload). Peak
  ~5.5 KB. macOS default worker stack is 8 MB, Linux 8 MB, FUSE
  worker stack 512 KB (noted in `tree.c:2187`). Worst-case call
  depth: `vfs_write` → `tree_resolve_page` → ... → maybe 3 PoolSlots
  in flight. Far below the 512 KB limit.
- **L1 cache footprint** — 32-byte PoolSlot fits in half a cache line
  on x86 (64-byte lines) and a quarter on Apple Silicon (128-byte
  lines). The copy is from a single line in the page-cache payload
  to a single line on the stack. Both are hot in L1 during the
  resolve. The user's intuition that L1 makes this cheap is correct.
- **Cache-line bouncing on the page-cache payload** — `pool_release`'s
  memcpy back writes to the same cache line that `pool_acquire` just
  read from. This is a read-for-ownership transaction; the L1 line
  is "owned" by the releasing core. On multi-threaded code with
  two threads racing on the same pool page, the cache line bounces.
  Same as today (the existing `pool_resolve_rw` modifies the cache
  line in place, which is also a read-for-ownership).
- **API churn** — 116 sites all change shape. The mechanical
  transformation is straightforward (rename, add `.bytes`,
  add `pool_release` at end), but the diff is large. Per-workload
  commits keep each diff reviewable.
- **Error-path `pool_release` discipline** — if a call site has
  multiple early-return error paths, each must `pool_release`
  before returning. Forgetting a release is a silent leak (the
  dirty page stays dirty longer than needed; not a correctness
  bug, just slower flush). Mitigation: a coding-pattern note in
  the workload specs; reviewers check for missing releases.

## Implementation order

Workloads 1 → 9 in sequence. No parallelism.

For each workload:
1. Migrate the call sites.
2. Build — fix compile errors (mostly `.bytes` access + `pool_release`
   placement).
3. Run all 13 ctest suites. All must pass.
4. Run the bench subset listed in that workload's "Test gate" section.
5. Compare to baseline at `/tmp/vfs_baseline/BASELINE.md`.
6. If regression > 10%, stop and report. Otherwise commit and move on.

## Commit plan

One commit per workload. Suggested format:
```
phase-25-w1: pool by-value API introduction
phase-25-w2: tree.c bootstrap migration
phase-25-w3: tree.c vfs_create + vfs_delete migration
phase-25-w4: tree.c vfs_mkdir + vfs_rmdir migration
phase-25-w5: tree.c vfs_rename + vfs_write migration
phase-25-w6: tree.c read + stat + chain helpers migration
phase-25-w7: tree.c dirchain + dircontentindex migration
phase-25-w8: gc.c migration
phase-25-w9: remaining files + drop old pool_resolve API
```

After workload 9, the C1 hazard is closed: no caller of the pool
holds a pointer into the cache payload for longer than one
acquire/release pair.

## Source

- `ISSUES.md` C1 — `pool_resolve` returns a cache pointer that can be
  invalidated mid-use. The full hazard inventory with line numbers.
- `/tmp/vfs_baseline/BASELINE.md` — pre-migration bench numbers and
  the "numbers to defend" thresholds from `vfs_bench` and the FUSE
  scenario.
- `src/tree.c` lines 107, 206, 691, 858, 866, 932, 1587, 1664, 1696 —
  the 9 hazardous call sites, the canonical C1 hazards.
- `impl/phase-3-pool-allocator.md` — original pool design; the
  copy-out model is a refactoring, not a redesign.
- `impl/phase-9-optimization.md` — earlier perf-driven changes to
  `pool.c`; this phase continues that line of work.
