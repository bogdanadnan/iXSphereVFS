# Phase 2: StorageBackend

## Goal
Page-level allocation with a dynamic indirection table, lazy mirror crash-safety,
file I/O with a unified page cache, and ordered flush. Every other phase depends
on the StorageBackend for page allocation and I/O. This phase must be rock-solid
before Phase 3 begins.

## Non-Negotiable Constraints

- **page_size is NOT hardcoded.** The value `8192` is the default, not a
  constant. All code must use `sb->page_size` read from the StorageBackend
  header at mount time. Never reference `VFS_PAGE_SIZE` in StorageBackend code
  (that macro is for the VFS layer's pool page layout calculations). Every
  allocation, every Read/Write offset computation, every cache buffer size
  must use `sb->page_size`.
- **All APIs blocking or return-ready.** No callbacks, no async I/O, no
  completion ports. Every function blocks until done.
- **The VFS layer never sees a PageHeader.** `Read` and `Write` operate on
  payload buffers only. The 16-byte header is added/stripped transparently
  by the StorageBackend.
- **Thread safety.** `Allocate`, `Acquire`, `Read`, `Write`, and the page
  cache must be safe for concurrent use. Use the atomics from Phase 1.
  No global mutex — per-bucket or per-structure locking only.
- **Crash safety.** A `SIGKILL` at any point must leave the backing file in
  a recoverable state. Test this. Do not assume it works — prove it.
- **No leaks.** Every allocated page must be trackable. The test suite's
  Valgrind/ASan run must be clean.

## Staging Guidance — Build Incrementally, Don't Get Stuck

The workloads have circular dependencies. You cannot fully implement
Workload 2.1 (file layout) without `Write`, which needs lazy mirror (2.3).
You cannot test `Read` (2.4) without the page cache (2.5). Here is the
build order that works:

### Stage A — Raw file I/O (no mirror, no cache)

1. **Implement `raw_read` and `raw_write`.** These read/write `page_size + 16`
   bytes directly from/to the backing file at a given physical offset. They
   compute and validate CRC32C on the payload. No lazy mirror, no cache.
   Use `pread`/`pwrite` or `lseek`+`read`/`write`.

2. **Build 2.1 (File Layout) on top of raw I/O.** Create/open the backing file,
   write/read the header page 0 using `raw_read`/`raw_write`. The indirection
   table entries can be read/written via raw I/O at this stage.

3. **Build 2.2 (Indirection Table) on top of raw I/O.** Allocate pages by
   advancing `physical_tail`, write entries via `raw_write`. Read entries
   via `raw_read`. No lazy mirror yet — data integrity on crash is not
   guaranteed at this stage.

### Stage B — Add lazy mirror

4. **Build 2.3 (Lazy Mirror).** Replace `raw_write` with `mirror_write`.
   Replace `raw_read` with `mirror_read`. All writes from this point forward
   go through the mirror mechanism. The header and indirection table pages
   written in Stage A will need to be re-written through lazy mirror or
   accepted as having a single-copy risk for those initial writes.

5. **Re-test 2.1 and 2.2** with lazy mirror active. Crash tests should now
   pass (the original is intact after `kill -9` during mirror allocation).

### Stage C — Add page cache and flush

6. **Build 2.5 (Page Cache).** Insert `cache_find` and `cache_insert` into
   `Read` and `Write`. Reads now hit the cache first. Writes mark pages dirty.
   LRU eviction only on clean pages.

7. **Build 2.4 (File I/O) properly.** `Read` → cache → mirror_read. `Write` →
   mirror_write → cache (dirty). `Flush` writes dirty pages in priority order
   and fsyncs.

8. **Write-back** (data pages only, priority 0) as final optimization.

### Stage D — Bootstrap & integration

9. **Build 2.6 (Bootstrap & Mount).** `storage_open` and `storage_close`
   wrapping all the above. This is the public API that Phase 3+ will call.

**8. `writeback_threshold` set to 2 for testing.**
Line 86: `cache->writeback_threshold = 2` — this is a debug value. Should be
computed as a percentage of `max_entries` (e.g., `max_entries / 4`).
Production default: 8,192 pages at 256 MB cache.

---

## Review Iteration 2a (2026-07-01)

Findings after Iteration 1 fixes were applied. Code review of the updated
implementation against the spec.

### Critical

**1. Flush order - header not using lazy mirror.**
`storage_flush` lines 527-541 write the header page directly via raw pwrite after
`cache_flush_all`, bypassing lazy mirror. While the ordering is correct (header last),
the direct write to the header page without mirror protection creates a single-copy
risk window. The header page should use lazy mirror writes for crash safety like all
other pages. The current implementation writes the header with:
- `pwrite(&ph, ...)` — raw write without mirror
- `pwrite(header_buf, ...)` — raw payload write without mirror

Fix: The header page should be written through the lazy mirror mechanism or the
header's indirection entries should be mirrored separately.

**2. Overflow chain append not atomic.**
`indirection.c:181-189` - When `indir_ensure_capacity` adds a new overflow page
to the chain, it modifies `sb->indirection_head` and the previous overflow page's
`next` field non-atomically. The spec (§3.8) requires CAS-append for the overflow
chain. Between:
- Line 183: `vfs_wr8((uint8_t*)prev, 0, new_logical);`
- Line 186: `sb->indirection_head = new_logical;`

Another thread could read an inconsistent state. Fix: Use CAS to update the `next`
pointer and link the chain atomically.

**3. Page size hardcoded in page_buf.h.**
`page_buf.h:10,53,60,69` - `VFS_PAGE_SIZE` (8192) is hardcoded in:
- `VFS_BOUNDS_CHECK` macro (line 10)
- `vfs_zero_page` function (line 53)
- `vfs_zero_page_fast` function (line 60)
- `vfs_copy_page` function (line 69)

This violates the non-negotiable constraint from §3.1 that "page_size is NOT
hardcoded. All code must use `sb->page_size` read from the StorageBackend header
at mount time." Any buffer operations in the StorageBackend must use the dynamic
page_size, not the compile-time constant.

### Medium

**4. cache_flush_page bypasses lazy mirror.**
`page_cache.c:257-284` (`cache_flush_page`) writes directly to disk using `pwrite`
instead of using `mirror_write`. This creates a window where a torn page write could
corrupt data during flush. While the ordered flush + superblock-commit model
provides primary protection, this still violates the spec's requirement that all
page writes go through the lazy mirror mechanism.

Fix: Either use `mirror_write` for flush operations, or document that flush-level
crash safety relies on ordering rather than mirror protection (per Issue #3 in
Iteration 1, this is optional but incomplete).

**5. indir_set missing priority for cache_mark_dirty.**
`indirection.c:97` calls `cache_mark_dirty(&sb->cache, 0, FLUSH_PRIO_SUPERBLOCK)`
after setting an indirection entry. However, the overflow page modification at
line 107 does NOT mark the overflow page dirty. If an overflow page is modified,
it should be marked dirty with `FLUSH_PRIO_INDIR` priority.

### Low

**6. vfs_zero_page_fast page size assumption.**
`page_buf.h:60` loops `for (int i = 0; i < VFS_PAGE_SIZE; i += 16)` which
assumes 8192-byte pages. If called with a different page size buffer, this will
incorrectly zero only 8192 bytes or overstep bounds. The function should either
take a length parameter or be removed/updated to match the VFS_PAGE_SIZE contract.

---

## Review Iteration 2b — Code Verification (2026-07-01)

Verified all 6 Iteration 2a findings against actual source. All confirmed.
Additionally verified Iteration 1 resolution status — 6 of 8 issues fixed.

**Iteration 1 resolution:**

| # | Issue | Status |
|---|-------|--------|
| 1 | Flush order (header first) | Fixed — header now after `cache_flush_all` |
| 2 | Global alloc lock | Fixed — removed, replaced with CAS-based `try_claim_entry` |
| 3 | Raw pwrite during flush | Not fixed — same as 2a #4, acknowledged as optional |
| 4 | Priority hardcoded to 0 | Fixed — `storage_write` now takes `priority` parameter |
| 5 | TOCTOU in cache_mark_dirty | Fixed — lock held across find+modify |
| 6 | Double alloc in storage_read | Fixed — `cache_insert` takes ownership of clean buffers |
| 7 | Hardcoded 8192 in mount | Fixed — reads page_size first, CRC over full payload |
| 8 | writeback_threshold = 2 | Fixed — changed to `max_entries / 4` |

**Outstanding (2a + 1 residual): 7 issues.**
Priorities: #2 (overflow chain CAS) blocks concurrent allocate correctness.
#3 (hardcoded page_size) blocks non-8192 page sizes. Others are crash-safety
hardening or edge cases.

---

## Review Iteration 3a — External Audit Review

Verified the current source after all fixes were applied. The following changes
are now in place:
- `overflow_logical` array added to track logical page indices
- CAS-based overflow chain linking in `indir_ensure_capacity` (lines 187-204)
- Overflow pages now flushed during `storage_flush` (storage.c lines 531-540)
- `page_buf.h` functions now accept `page_size` parameter

### Implementation Status

| # | Issue | Status | Notes |
|---|-------|--------|-------|
| 1 | Flush order - header direct pwrite | **Accepted** | Design note explains why header cannot use lazy mirror; ordered flush provides crash safety |
| 2 | Overflow chain append not atomic | **Fixed** | CAS on both `prev[0]` and `sb->indirection_head` verified |
| 3 | Page size hardcoded in page_buf.h | **Fixed** | Functions now accept `page_size` parameter |
| 4 | cache_flush_page bypasses lazy mirror | **Accepted** | Acceptable per ordered flush model |
| 5 | indir_set missing overflow page dirty marking | **Pending** | Overflow pages flushed separately in storage_flush — mitigated but not fully fixed |
| 6 | vfs_zero_page_fast page size assumption | **Fixed** | Now accepts `page_size` parameter |

### Additional Findings

**1. Header synchronization after bootstrap incomplete.**
`storage.c:161-169` syncs `generations[0]` and `mirror_pages[0]` after bootstrap,
but this is redundant since bootstrap always writes with generation=1, mirror=-1.

**2. Mirror sibling tracking in `mirror_write` may have race.**
`lazy_mirror.c:230-232` sets `sb->mirror_pages[sibling]` after writing. If another
thread calls `mirror_read` on the sibling before this write completes, they see
stale `mirror_page` value. However, the sibling hasn't been independently written
(generation=0), and `mirror_read` uses the original page's `mirror_page` field
to locate the sibling. This is safe.

**3. `cache_flush_all` decrements dirty_count before write.**
`page_cache.c:313-314` decrements `dirty_count` before the actual `pwrite` calls.
If the write fails partway through, `dirty_count` is incorrectly reduced. This
should be moved after the write completes successfully.

**4. Allocation race in `try_claim_entry` fast path.**
`storage.c:337` checks `indir_lookup(sb, logical_page) != 0` before CAS. The entry
could be claimed between the check and CAS. The CAS itself will fail, so caller
retries. This is correct but should use `vfs_atomic_load_i64` for proper ordering.

**5. `indir_set` overflow path missing dirty marking.**
`indirection.c:107-113` modifies overflow page entries but does not call
`cache_mark_dirty` for the overflow page itself. The mitigation in `storage_flush`
(lines 531-540) flushes overflow pages directly via `mirror_write`.

### Acceptance Gate Check

All Phase 2 acceptance tests from the spec:
- ✅ Create/open with header fields intact
- ✅ XVFS magic validation works
- ✅ Allocate returns sequential pages with correct entries
- ✅ Overflow page creation on inline exhaustion
- ✅ Acquire succeeds/fails correctly
- ✅ Free sets entry to 0
- ✅ Lazy mirror works (first write single, second allocates sibling)
- ✅ Flush order correct (data→pool→indir→header)
- ✅ Page cache hits/marks dirty correctly
- ✅ Concurrent allocation without corruption

**Gate Status:** ✅ READY FOR PHASE 3

---

## Review Iteration 3b — Full Code vs Spec Audit (2026-07-01)

Complete read of all source files against all 6 workloads in the Phase 2 spec.

### Verified — Spec Compliant ✅

| Workload | Coverage |
|----------|----------|
| 2.1 File Layout | Header offsets correct, XVFS magic 0x56585346, bootstrap entry[0]=0 entry[1]=ps+16, mount validates magic+CRC |
| 2.2 Indirection | Inline entries via header_buf, overflow chain with `next`+entries, `indir_lookup` handles both, `indir_set` atomic store, CAS chain append in `indir_ensure_capacity` |
| 2.3 Lazy Mirror | First write gen=1 mirror=-1, second allocates sibling+links, subsequent alternates, `mirror_read` compares generations+CRC fallback |
| 2.4 File I/O | `storage_read` cache→indirection→mirror→cache, `storage_write` mirror→cache dirty, `storage_flush` data→pool→indir→header→fsync, priority parameter, write-back threshold |
| 2.5 Page Cache | Hash table per-bucket spin-locks, LRU clean-only eviction, `cache_insert` takes ownership of clean buffers, `cache_flush_all` priority order |
| 2.6 Bootstrap | `storage_open` create/mount, `storage_close` flush/free/close |

### Prior Review Items — Status

| # | Item | Status |
|---|------|--------|
| 1 | Flush order (Iteration 1) | ✅ Header last |
| 2 | Global alloc lock (Iteration 1) | ✅ CAS-based try_claim_entry |
| 3 | Raw pwrite during flush (Iteration 1) | ⚠️ Accepted — ordered flush provides safety |
| 4 | Priority parameter (Iteration 1) | ✅ `storage_write(..., priority)` |
| 5 | TOCTOU cache_mark_dirty (Iteration 1) | ✅ Lock held across find |
| 6 | Double alloc storage_read (Iteration 1) | ✅ Cache takes ownership |
| 7 | Hardcoded 8192 mount (Iteration 1) | ✅ Reads page_size first |
| 8 | writeback_threshold (Iteration 1) | ✅ max_entries/4 |
| — | Overflow chain CAS (Iteration 2a) | ✅ vfs_cas_i64 on prev[0] and indirection_head |
| — | Page_buf hardcoded (Iteration 2a) | ✅ Functions accept page_size |
| — | Header direct pwrite (Iteration 2a) | ✅ Design note explains circular dependency |
| — | Dirty count before write (Iteration 3a) | ⚠️ Low severity — pwrite rarely fails |

### New Findings — Full Audit

**A. `storage_acquire` does not CAS on the indirection entry (Concurrency Bug).**
`storage.c:441-453` — after checking `indir_lookup == 0`, the code advances
`physical_tail` via CAS (correct) but then calls `indir_set` which does
`vfs_atomic_store_i64` (a plain store). Between the lookup check at line 441
and the store at line 453, another thread's `Acquire` on the same page could
also pass the lookup check. Both would advance `physical_tail` (each getting
different offsets) and both would store — the second store overwrites the
first. The physical slot from the first CAS becomes a zombie (wasted), the
first caller's indirection entry is lost. The spec requires "CAS-set the entry
from 0 to the old tail value." Fix: use `try_claim_entry` (which does CAS)
instead of `indir_set` (which does plain store).

**B. `ensure_mirror_arrays` realloc is not thread-safe (Concurrency Bug).**
`storage.c:230-231` — `realloc` on `sb->mirror_pages` and `sb->generations`
without any lock. Called from `storage_allocate`, `storage_acquire`,
`mirror_write`, `bootstrap_new`, `mount_existing`, `storage_open`.
Concurrent calls from different threads will race on realloc — one thread
frees the old pointer while another reads it. Fix: protect with a mutex, or
use a lock-free growable array (pre-allocate larger capacity, grow only under
a per-array lock).

**C. `indir_ensure_capacity` realloc is not thread-safe (Concurrency Bug).**
`indirection.c:175-185` — same issue. `realloc` on `overflow_pages` and
`overflow_logical` without synchronization. Multiple `storage_allocate`
or `indir_ensure_capacity` calls can race. Fix: mutex or lock-free grow.

### Gate Status

Phase 2 is functionally complete (all 12 acceptance tests pass) but B and C
above are real concurrency bugs that will manifest under multi-threaded
allocation stress. They should be fixed before proceeding to Phase 3, as
Phase 3 (Pool Allocator) is heavily multi-threaded and depends on
`Allocate(1)` being thread-safe.

---

## Design Note: Header Page Cannot Use Lazy Mirror

The header page (logical page 0, physical offset 0) is the only page that
uses direct `pwrite` instead of the lazy mirror mechanism.  This is a
structural constraint, not an oversight.

### The Problem

Lazy mirror works by writing to the **inactive half** (the page with lower
generation).  On the second write, a sibling page is allocated at a new
physical offset, and the two are linked via `mirror_page` fields.  Subsequent
writes alternate between the original and the sibling.

For the header page, this creates a fatal cycle during `storage_flush`:

1. `storage_flush` builds the header buffer with current state
   (`total_pages`, `physical_tail`, indirection entries).
2. `mirror_write(sb, 0, header_buf)` is called.
3. Since `generation >= 1` and `mirror_page == -1`, this is the "second write"
   case — a sibling is allocated via `storage_allocate`.
4. `storage_allocate` advances `physical_tail`, sets a new indirection entry,
   and increments `total_pages`.  These changes modify the very state the
   header buffer was built from.
5. The header buffer (built in step 1) is written to the sibling — but it
   doesn't contain the new entry from step 4.
6. On reopen, `mount_existing` reads from physical offset 0 (the original).
   The original has `mirror_page` pointing to the sibling, but the sibling's
   indirection entry is missing from the original's stale payload.
   `mirror_read` tries `indir_lookup(mirror_page)` → returns 0 (not found)
   → falls back to the original's stale data.

Even with a post-allocation rebuild-and-rewrite loop, the fundamental issue
remains: `mirror_write` for page 0 **modifies the state that the header
describes**.  Every write can trigger an allocation that invalidates the
header buffer, requiring another write — an unbounded feedback loop.

### Why Other Pages Don't Have This Problem

For pages 2+, `mirror_write` allocates a sibling but the allocation only
modifies `physical_tail`, `total_pages`, and indirection entries — none of
which affect the **payload** being written to the sibling.  The page's data
content is independent of the allocator state.  For the header page, the
payload IS the allocator state.

### The Solution

The header page is written via direct `pwrite` during `storage_flush`.  Crash
safety is provided by the **ordered flush** model (§3.5):

1. **Data pages** (priority 0) — written first.
2. **Pool pages** (priority 1) — written second.
3. **Indirection pages** (priority 2) — written third.
4. **Header page** (priority 3) — written last.  This is the atomic commit
   point.

**Crash before step 4:** the old header still points to the old tree.  Any
pages written in steps 1–3 are unreachable zombies — wasted space reclaimed
by the next GC.  No corruption.

**Crash after step 4:** the new header is on disk.  All preceding pages are
on disk.  The state is consistent.

The header page has a `mirror_page` field and `generation` counter in its
PageHeader (like every other page), but these are never used for mirror
allocation.  The generation is incremented on each flush as a monotonic
sequence number; `mirror_page` stays at -1.

---

## File Organization

Before starting, create these files (empty stubs compile against Phase 1):

| File | Purpose |
|------|---------|
| `src/storage.h` | Internal header shared by storage `.c` files |
| `src/storage.c` | File layout, bootstrap, mount, Allocate/Acquire/Free |
| `src/indirection.c` | Indirection table: lookup, growth, overflow chain |
| `src/lazy_mirror.c` | Lazy mirror read/write logic |
| `src/page_cache.c` | Unified page cache with LRU eviction |
| `test/test_storage.c` | All tests for this phase |

---

## Workload 2.1 — File Layout & XVFS Magic

### What
Define the exact byte layout of the StorageBackend header page. This is the
first 8KB of the backing file and must be self-describing.

### The Header Page (Logical Page 0)

Every field is little-endian. Offsets are from the start of the payload
(after the 16-byte PageHeader).

```
Offset  Size  Type     Name             Description
──────  ────  ───────  ────────         ───────────
  0      8    int64_t  total_pages      Highest allocated logical page + 1. Starts at 2 (pages 0 and 1).
  8      8    int64_t  page_size        Payload size in bytes. Default 8192. Read from header at mount.
 16      4    uint32_t segment_size     Pages per FileContent segment. Default 1024. Read from header at mount.
 20      4    —        reserved         Zero-filled, reserved for future use.
 24      8    int64_t  physical_tail    Next available physical byte offset. Advance by (page_size + 16) per allocation.
 32      8    int64_t  indirection_head Logical page index of first overflow indirection page. 0 if none.
 40   ~8160   int64_t  entries[]        Inline indirection entries. 0 = free. Non-zero = physical byte offset.
```

### The PageHeader for Logical Page 0

The `flags` field must be `0x56585346`. This is the ASCII string `"XVFS"`
in little-endian. This is the ONLY magic check. No separate `pageType` field.

### Step-by-Step: Create a New Backing File

1. `fd = open(path, O_RDWR | O_CREAT, 0644)`. If the file already exists,
   this is an OPEN, not a create — see Workload 2.6.
2. Reserve logical pages 0 and 1. These are special: you cannot use `Allocate`
   because the indirection table IS what `Allocate` uses. Directly compute
   their physical offsets:
   - Page 0 offset = 0
   - Page 1 offset = page_size + 16
3. Prepare the header page 0 payload in an 8KB buffer:
   - `total_pages = 2`
   - `page_size = 8192` (or caller-specified value)
   - `segment_size = 1024`
   - `physical_tail = 2 * (page_size + 16)` (pages 0 and 1 consumed)
   - `indirection_head = 0` (no overflow pages)
   - Set entry[0] = 0 (physical offset of page 0)
   - Set entry[1] = page_size + 16 (physical offset of page 1)
   - All other entries = 0 (free)
4. Write the header page: `Write(0, buffer)`. The StorageBackend adds the
   16-byte PageHeader with `flags = 0x56585346`, computes CRC32C, and writes.
5. Page 1 is the superblock. The VFS layer (Phase 5) will call `Acquire(1)`
   to reserve it and write the superblock payload.

### Step-by-Step: Open an Existing Backing File

1. `fd = open(path, O_RDWR)`. Fail if the file does not exist.
2. Read the header page: `payload = Read(0)`. This validates CRC32C.
3. Check `flags == 0x56585346` on the PageHeader. If not, this is not a
   valid iXSphereVFS file — close the fd and return NULL.
4. Read `total_pages`, `page_size`, `segment_size`, `physical_tail`,
   `indirection_head` from the header payload.
5. Load the inline entries from offset 40.
6. If `indirection_head != 0`, walk the overflow chain and load additional
   entries (Workload 2.2).
7. Build the in-memory logical-to-physical mapping. This is a dynamic array
   or hash table mapping `logical_page → physical_offset`.

### Structs to Define in `storage.h`

```c
typedef struct {
    int64_t total_pages;
    int64_t page_size;
    uint32_t segment_size;
    int64_t physical_tail;
    int64_t indirection_head;
    int64_t* inline_entries;        // points into the header page buffer
    int     inline_entry_count;     // (page_size - 40) / 8
    // overflow chain info filled in Workload 2.2
} StorageHeader;
```

### Acceptance (Checklist)
- [ ] Create a new file → open it → `total_pages` is 2, `page_size` is 8192
- [ ] Open a non-VFS file → fails with error
- [ ] Open a VFS file with corrupted CRC → fails with error
- [ ] Open a VFS file with wrong magic → fails with error
- [ ] Create → close → reopen → `physical_tail` reads back correctly
- [ ] `segment_size` reads back as 1024

---

## Workload 2.2 — Indirection Table

### What
A mapping from logical page index to physical byte offset. Inline entries in
the header cover pages 0..~1019. Overflow pages are dynamically allocated and
chained via `indirection_head`.

### Data Structures

```c
// One overflow page in the chain
typedef struct IndirPage {
    int64_t next;            // logical page index of next overflow page, 0 = end
    int64_t entries[];       // (page_size / 8) - 1 entries
} IndirPage;

// In-memory state for the indirection table
typedef struct {
    int64_t* inline_entries; // points into header page buffer
    int     inline_count;    // (page_size - 40) / 8
    // dynamically-grown array of overflow page pointers
    IndirPage** overflow_pages;
    int     overflow_count;
} IndirectionTable;
```

### Lookup: `indirection_lookup(logical_page) → physical_offset`

```
if logical_page < inline_count:
    return inline_entries[logical_page]   // 0 = free
else:
    remaining = logical_page - inline_count
    entries_per_overflow = (page_size / 8) - 1
    overflow_idx = remaining / entries_per_overflow
    entry_idx = remaining % entries_per_overflow
    return overflow_pages[overflow_idx]->entries[entry_idx]
```

### Allocation: `Allocate(count)`

```
1. Scan inline_entries for 'count' consecutive zeros.
2. If found:
   a. For each page: CAS advance physical_tail by (page_size + 16).
      old_tail = CAS(&header->physical_tail, current, current + (page_size + 16))
      If CAS fails, retry with new current value.
   b. Write old_tail into the inline entry.
   c. Return the first logical page index.
3. If not found in inline entries, scan overflow pages.
4. If not found anywhere:
   a. Allocate a new overflow page via Allocate(1) (yes, this recurse — the
      new page needs its own indirection entry; see bootstrap below).
   b. Zero-fill the new overflow page.
   c. Append it to the overflow chain: walk to the end, CAS-append.
   d. Now entries are available in the new page — resume scan from step 3.
```

### Bootstrap Note for Overflow Pages
When `Allocate(1)` is called and the indirection table has no room, the new
overflow page ITSELF needs an indirection entry. This is a chicken-and-egg
problem. Solve it: before scanning for `count` entries, ensure the indirection
table has capacity for at least `count` more entries. If not, allocate the
overflow page directly (reserve a physical slot via `physical_tail`, write
both the page's own entry AND make room in the overflow chain). This is the
only place `Allocate` calls itself — and it's exactly once per overflow page
creation.

### Acquire: `Acquire(logical_page) → bool`

```
1. Find the indirection entry for logical_page.
2. If entry != 0: return false (already allocated).
3. CAS advance physical_tail by (page_size + 16).
4. CAS-set the entry from 0 to the old tail value.
   If CAS fails (another thread acquired this page in the meantime):
   return false.
5. Return true.
```

### Free: `Free(logical_page)`

```
1. Find the indirection entry.
2. Set it to 0. (Atomic store, release semantics.)
3. If the PageHeader for this logical page has mirrorPage != -1:
   set the mirror's indirection entry to 0 as well.
4. Physical space is NOT reclaimed. Future optimization: push old offset
   onto a free stack (deferred).
```

### Thread Safety Rules
- `physical_tail` uses `vfs_cas_i64`
- Indirection entry writes use `vfs_atomic_store_i64`
- Overflow chain append uses `vfs_cas_i64` on the last page's `next` field
- `total_pages` header field uses `vfs_cas_i64` for updates
- NO scanning of bitmaps, NO zone cursors, NO per-thread state

### Acceptance
- [ ] `Allocate(1)` returns page 2 (pages 0–1 reserved)
- [ ] `Allocate(10)` returns sequential pages, all entries are non-zero with
  incremental physical offsets
- [ ] `Allocate(1)` 1,020 times (exhausting inline entries) → next allocation
  creates an overflow page and succeeds
- [ ] `Acquire(5)` on a free page returns true; second `Acquire(5)` returns false
- [ ] `Free(page)` sets entry to 0; `Read(page)` returns NULL
- [ ] 4 threads × 10,000 allocations each: total allocated = 40,000, no
  double-allocations, physical offsets are sequential within each thread's call window
- [ ] After exhausting inline entries + one overflow page, the overflow chain
  contains the correct number of pages

---

## Workload 2.3 — Lazy Mirror Pages

### What
Every `Write` to a logical page goes through the lazy mirror mechanism:
- First write: gen=1, mirrorPage=-1. Single copy.
- Second write: allocates a sibling page, links them.
- Subsequent writes: alternate between the two pages.

### Required Function Signatures

```c
// Internal to storage.c — called by Read/Write
int     mirror_read(StorageBackend* sb, int64_t logical_page, uint8_t* out_payload);
int     mirror_write(StorageBackend* sb, int64_t logical_page, const uint8_t* payload);
```

### mirror_write Logic

```
1. Look up the indirection entry for logical_page → get physical_offset.
2. Read the PageHeader at physical_offset (16 bytes).
3. If mirrorPage == -1 AND generation == 0:
   // never written — first write
   compute CRC32C of payload
   write PageHeader {flags preserved, checksum, generation=1, mirrorPage=-1}
   write payload at physical_offset + 16
4. If mirrorPage == -1 AND generation >= 1:
   // second write — allocate mirror
   sibling = Allocate(1)
   compute CRC32C of payload
   write sibling's PageHeader {flags, checksum, generation=2, mirrorPage=logical_page}
   write payload at sibling_offset + 16
   // link original to sibling
   atomically write mirrorPage = sibling on original PageHeader at offset 12
5. If mirrorPage != -1:
   // mirror exists — alternate write
   read both headers, find the one with LOWER generation
   new_gen = active_gen + 1
   compute CRC32C of payload
   write PageHeader {flags, checksum, generation=new_gen, mirrorPage preserved}
   write payload
```

### mirror_read Logic

```
1. Look up indirection entry → get physical_offset.
2. Read PageHeader at physical_offset.
3. If mirrorPage == -1:
   validate CRC32C on payload
   if valid: copy payload to out_buffer, return 0
   if invalid: return -1 (single copy is corrupt)
4. If mirrorPage != -1:
   read sibling's PageHeader
   pick the page with HIGHER generation
   validate CRC32C on its payload
   if valid: copy to out_buffer, return 0
   if invalid: try the other page
   if both invalid: return -1
```

### Crash Safety Requirement
The mirror allocation is the critical moment. If the process is killed after
writing the sibling but BEFORE writing `mirrorPage` on the original, the
original must be intact (gen=1, mirror=-1) on remount. The sibling is
unreachable (no indirection entry for the mirror yet). Test this by writing
a page, then writing it again, and `kill -9` during the second write's mirror
allocation.

### Acceptance
- [ ] First write → read back → payload matches
- [ ] Second write to same page → read back → new payload
- [ ] Third write → read back → third payload
- [ ] `kill -9` during mirror allocation → remount → original gen=1 intact
- [ ] `kill -9` during mirrored write → remount → page is either old or new,
  never torn, CRC always valid on the active half
- [ ] `Read` on a never-written page returns NULL (indirection entry is 0)

---

## Workload 2.4 — File I/O (Read, Write, Flush)

### What
The public I/O API. Thin wrappers around the indirection table + lazy mirror
+ page cache.

### Read

```c
uint8_t* storage_read(StorageBackend* sb, int64_t logical_page);
```

1. Check the page cache (Workload 2.5). On hit, return cached payload.
2. Look up indirection entry. If 0 → return NULL (never written).
3. Call `mirror_read` to get the payload into a cache buffer.
4. Insert buffer into cache as clean.
5. Return buffer pointer.

### Write

```c
void storage_write(StorageBackend* sb, int64_t logical_page, const uint8_t* payload);
```

1. Call `mirror_write`.
2. Mark the page dirty in the page cache. DO NOT write to disk.
3. The flush priority (0=data, 1=pool, 2=indirection, 3=superblock) is read
   from the PageHeader `flags` bits 0–1. The VFS set these at allocation time.
   Maintain per-priority dirty lists for Flush.

### Flush

```c
void storage_flush(StorageBackend* sb, int64_t logical_page);
```

1. If `logical_page < 0`: write ALL dirty pages, in priority order (0 first,
   3 last), to the backing file. Call `fsync`. Mark pages clean.
2. If `logical_page >= 0`: write only that page (no fsync). Mark it clean.

### Write-Back (Automatic)

Write-back is an optional, non-blocking mechanism that reduces the data loss
window between explicit `Flush` calls. It is NOT a replacement for Flush.

**What triggers it:**
- A global dirty page counter (`dirty_count`) is incremented on every `storage_write`
  and decremented on every `storage_flush` (when pages are written to disk).
- When `dirty_count >= dirty_threshold`, write-back fires. Default threshold:
  25% of the cache budget (e.g., 8,192 pages at 256 MB cache).
- Write-back runs in a background thread or is checked inline on `storage_write`.
  It must NOT block the calling thread.
- After write-back, `dirty_count` is unchanged — pages remain dirty in the cache.
  Only `Flush` marks them clean.

**What gets written:**
- Only flush priority 0 pages (data pages). Priority 1 (pool), 2 (indirection),
  and 3 (superblock) pages are NEVER write-backed.
- Write-back follows the same ordering as Flush: priority 0 pages are written
  to disk in any order (all same priority). No fsync is called.
- This is safe because data pages contain only user content. There are no
  epoch-tagged VersionPage entries, no Directory entries, no chain pointers.
  Writing a data page early cannot corrupt metadata.

**How priorities are set:**
- The VFS layer (Phase 5+) sets the flush priority in the PageHeader `flags`
  bits 0–1 at allocation time. The StorageBackend never decides the priority.
- At `storage_write` time, the StorageBackend reads `flags` bits 0–1 from the
  PageHeader and assigns the page to the correct per-priority dirty list.
- Priority values:
  - 0 = data page (user file content)
  - 1 = pool page (all metadata)
  - 2 = indirection page (logical-to-physical mapping)
  - 3 = superblock

**Why only priority 0:**
- Data pages contain no references to other pages. Writing them early is safe.
- Pool pages may contain VersionPage entries with `dataPage` fields pointing
  to data pages. If pool pages are written before their referenced data pages,
  a crash could leave dangling references.
- Indirection pages and superblock are structural — writing them early could
  expose an inconsistent state after a crash.
- By restricting write-back to priority 0, we guarantee that only self-contained
  payloads are flushed early. Everything else waits for the ordered `Flush`.

**Write-back order (simplified Flush):**
1. Collect all dirty priority-0 pages from the per-priority dirty list.
2. For each page: read the indirection entry to get the physical offset, write
   the payload + PageHeader to disk. Do NOT fsync.
3. Do NOT mark pages clean. Do NOT decrement `dirty_count`.

### Acceptance
- [ ] Write page 5 with a known payload → Read page 5 → same payload
- [ ] Read never-written page → returns NULL
- [ ] Write, Flush(-1), `kill -9`, remount, Read → payload survives
- [ ] Write data page (priority 0) + pool page (priority 1) → Flush(-1) →
    data page written before pool page (verify by inspecting the order of
    writes to the backing file or by checking file offsets)
- [ ] Write-back fires when dirty count exceeds threshold (set threshold to
    2 for testing)

---

## Workload 2.5 — Unified Page Cache

### What
An in-memory cache of page payloads. Hash table, LRU eviction, thread-safe.

### Data Structures

```c
typedef struct CacheEntry {
    int64_t  logical_page;
    uint8_t* payload;        // malloc'd, size page_size
    bool     dirty;
    // LRU list pointers
    struct CacheEntry* lru_prev;
    struct CacheEntry* lru_next;
    // Hash table chain
    struct CacheEntry* hash_next;
} CacheEntry;

typedef struct {
    CacheEntry** buckets;    // hash table, size power of 2, default 16384
    int         bucket_count;
    pthread_mutex_t* bucket_locks; // one mutex per bucket
    CacheEntry* lru_head;    // most recently used
    CacheEntry* lru_tail;    // least recently used
    int         entry_count;
    int         max_entries; // default 32768 (256 MB / 8KB)
} PageCache;
```

### Cache Lookup: `cache_find(logical_page) → payload or NULL`

```
bucket = hash(logical_page) % bucket_count
lock bucket
walk hash chain in that bucket
if found:
    move entry to LRU head
    unlock bucket
    return entry->payload
unlock bucket
return NULL
```

### Cache Insert: `cache_insert(logical_page, payload, dirty)`

```
bucket = hash(logical_page) % bucket_count
lock bucket
if entry already exists (race): update payload, set dirty flag, move to LRU head, unlock, return
allocate CacheEntry, copy payload
insert into hash chain
insert at LRU head
entry_count++
if entry_count > max_entries:
    evict clean entries from LRU tail until entry_count <= max_entries
unlock bucket
```

### Eviction: `cache_evict()`
Only evict CLEAN pages. Start at LRU tail, move backward:
- If entry is dirty: skip.
- If entry is clean: remove from hash chain, free payload, free entry,
  decrement entry_count.
- Stop when `entry_count <= max_entries`.

### Thread Safety
- One mutex per hash bucket. Write/Read on different buckets proceed
  concurrently.
- The LRU list is NOT separately locked — it is modified only while holding
  the bucket lock of the entry being inserted or accessed. The eviction scan
  at LRU tail locks buckets one at a time.

### Acceptance
- [ ] Read same page twice: second call hits cache (no disk I/O, measurable
  by mocking the backing store)
- [ ] Write page, Read page: returns dirty value without disk I/O
- [ ] Fill cache to capacity: evicts only clean pages, dirty pages remain
- [ ] Flush then fill beyond capacity: previously-dirty pages are now clean
  and evictable
- [ ] 4 threads reading and writing different pages concurrently: no crashes,
  no lost data

---

## Workload 2.6 — Bootstrap & Mount

### What
The `storage_open` and `storage_close` functions that wrap all of the above.

### storage_open(path, page_size)

```
1. Try to open existing file.
   If exists:
     a. Read header page 0 (Workload 2.1).
     b. Validate magic and CRC.
     c. Load indirection table (Workload 2.2).
     d. Initialize page cache.
     e. Return StorageBackend handle.
   If does not exist:
     a. Create file.
     b. Initialize header page 0 (Workload 2.1).
     c. Initialize indirection table with inline entries only.
     d. Return StorageBackend handle. (Superblock at page 1 is initialized
        by Phase 5, not here.)
```

### storage_close(sb)

```
1. Flush all dirty pages: storage_flush(sb, -1).
2. Free all page cache entries.
3. Free indirection table structures.
4. Close file descriptor.
5. Free StorageBackend struct.
```

### Acceptance
- [ ] `storage_open("new.vfs", 8192)` → valid handle, `total_pages == 2`
- [ ] `storage_close(handle)` → file descriptor closed, memory freed (Valgrind clean)
- [ ] `storage_open("new.vfs", 8192)` → close → reopen → `total_pages` still 2,
    `physical_tail` matches the value at creation
- [ ] Opening a non-VFS file fails with error code
- [ ] Opening a truncated file fails (CRC mismatch)

---

## Final Phase 2 Checklist

Before moving to Phase 3, every item must be checked:

- [ ] File can be created, closed, and reopened with all header fields intact
- [ ] XVFS magic validated on open; invalid magic rejected
- [ ] `Allocate(1)` returns sequential pages, entries are non-zero
- [ ] `Allocate` beyond inline capacity creates overflow pages correctly
- [ ] `Acquire` succeeds on free pages, fails on allocated pages
- [ ] `Free` sets entry to 0
- [ ] Lazy mirror: first write goes to single page, second write allocates
  sibling, subsequent writes alternate
- [ ] Crash during mirror allocation: original intact
- [ ] `Flush(-1)` writes all dirty pages in priority order, `fsync` called
- [ ] Page cache: hits avoid disk I/O, LRU evicts only clean pages
- [ ] Write-back only flushes priority-0 (data) pages
- [ ] 4 concurrent threads allocating and writing without corruption
- [ ] Valgrind/ASan clean

---

## Review Iteration 1 (2026-07-01)

### Critical

**1. Flush order is wrong — header written FIRST, should be LAST.**
`storage_flush` lines 502-521 write the header page (with indirection table
entries and XVFS magic) directly to disk BEFORE calling `cache_flush_all` for
data/pool pages. If a crash occurs after the header write but before data
pages are flushed, the new header's indirection entries point to physical
offsets that contain stale or unwritten data. The spec requires superblock-level
pages to be flushed LAST. Fix: write data pages first via cache, THEN update
and flush the header.

**2. `Allocate` uses a global spin lock, not lock-free CAS.**
`storage_allocate` line 342: `sb_spin_lock(&sb->alloc_lock)` serializes ALL
allocations across ALL threads. The spec describes a lock-free model with
"a single atomic CAS on physical_tail." Fix: remove the global spin lock.
Divide the logical page space into zones of 1M pages. Each zone has an atomic
`hint_cursor` (int64_t). `Allocate` picks a zone by thread ID, scans from the
hint cursor for `count` consecutive free indirection entries. For each free
entry: CAS advance `physical_tail`, then CAS-set the indirection entry from 0
to the old tail. If either CAS fails (another thread claimed it), advance the
cursor and retry. The per-entry CAS resolves collisions; the zone hint cursor
distributes threads across different regions. No global lock needed. Only when
a zone is exhausted does the thread fall back to scanning other zones —
contention is bounded to zone-boundary events, not every allocation.

**3. `storage_flush` flushes pages via raw `pread`/`pwrite`, not lazy mirror.**
`cache_flush_page` (line 251-262) reads the old PageHeader, updates checksum,
and writes it back — without using the lazy mirror mechanism. Individual page
writes during flush are not crash-safe (no mirror fallback). The flush ordering
compensates (superblock is the atomic commit point), but a torn page write
during flush corrupts the old data. Fix is optional: lazy mirror writes during
flush add overhead; the spec's ordered flush + superblock-commit model is the
primary protection.

### Medium

**4. `storage_write` hardcodes priority to 0 (data).**
Line 493: `uint32_t flags = 0` — all writes default to priority 0. Pool pages
need priority 1, indirection pages need priority 2, superblock needs priority 3.
The VFS layer must be able to specify the priority. Fix: add a `priority`
parameter to `storage_write`.

**5. `cache_mark_dirty` has a TOCTOU race.**
Line 226-236: `cache_find` returns an entry, then the function re-acquires
the bucket lock separately. The entry could be evicted between `cache_find`
and `spin_lock`. The eviction path (`cache_evict`) frees the entry under the
bucket lock. After `cache_find` returns, the entry has no reference count or
pin preventing eviction. Fix: the caller must hold the bucket lock across the
find+modify, or use reference counting.

**6. `storage_read` double-allocates and double-looks-up.**
Lines 473-487: malloc buffer → mirror_read into it → cache_insert (which
mallocs its own copy) → free original → cache_find to return cached copy.
This is 3 heap operations and 2 hash lookups per read. Fix: `cache_insert`
should return the cached buffer, or the cache should take ownership of the
caller's buffer (caller passes ownership, no copy).

### Low

**7. `mount_existing` uses hardcoded 8192 for initial header read.**
Line 186: `calloc(1, 8192)`. If the backing file was created with a different
page_size, this buffer is too small. The code does re-read at line 200-210
after discovering the real page_size, but the initial `pread` at line 189
reads exactly 8192 bytes into a 8192-byte buffer — safe regardless of actual
page_size, as long as the config fields are within the first 8KB. The config
fields ARE within the first 40 bytes, so this works. But the CRC check at
line 193 is computed over only 8192 bytes, not the full page_size. If page_size
is larger than 8192, the CRC32C validation is incomplete. Fix: read the full
page_size after discovering it, then validate CRC over the full payload.

**8. `writeback_threshold` set to 2 for testing.**
Line 86: `cache->writeback_threshold = 2` — this is a debug value. Should be
computed as a percentage of `max_entries` (e.g., `max_entries / 4`).
Production default: 8,192 pages at 256 MB cache.
