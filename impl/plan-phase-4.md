# Phase 4: Node Types — Implementation Plan

## Overview
Phase 4 defines all 32-byte pool entry layouts with serialization/deserialization
helpers. Every entry type fits in exactly one 32-byte pool slot (except NameEntry
which chains). This phase produces data structures and read/write helpers — no
runtime tree behavior.

**Files to create:**
- `native/iXSphereVFS/src/nodes.h` — offset constants, struct typedefs, function declarations for all 10 node types
- `native/iXSphereVFS/src/nodes.c` — read/write helpers (Write for every type, Read for every type, plus NameEntry chain helpers)
- `native/iXSphereVFS/test/test_nodes.c` — comprehensive unit tests

**Key dependencies (already complete):**
- `pool.h` / `pool.c` — `Pool`, `pool_alloc`, `pool_resolve`, `VFS_VPTR_MAKE/PAGE/SLOT`
- `page_buf.h` — `vfs_rd2/rd4/rd8`, `vfs_wr2/wr4/wr8`
- `storage.h` / `storage.c` — `storage_open`, `storage_close`, `storage_read`, `storage_write`, etc.

**Critical constraints from the spec:**
- Every entry fits in 32 bytes (only DirNode/FileNode have explicit type field)
- All multi-byte fields are little-endian (use vfs_rdX/vfs_wrX)
- VirtualPtr is the only pointer type
- Zero-initialized slots are safe (nextPtr=0 means null, etc.)
- `nodeId` is passed as parameter — Phase 4 does NOT assign it (caller responsibility)

---

## Phase 4a — Header & Build Setup

- [ ] **Task 4a.1: Create `src/nodes.h`**
  - Define offset constants for all 10 node types (one `#define` block per type)
  - Define type discriminator constants: `NODE_TYPE_DIR = 0x01`, `NODE_TYPE_FILE = 0x03`
  - Declare all Write/Read helpers (one `nodes_write_*` / `nodes_read_*` per type)
  - Declare `nodes_write_name` and `nodes_read_name` (NameEntry chain helpers, take `Pool*`)
  - Include guards, include `pool.h` and `page_buf.h`
  - Use `VFS_POOL_SLOT_SIZE` (32) for sizing, not hardcoded 32

- [ ] **Task 4a.2: Update `CMakeLists.txt`**
  - Add `src/nodes.c` to `ixsphere_vfs` library sources
  - Add `test/test_nodes.c` as a new test executable (`test_nodes`)
  - Link `test_nodes` against `ixsphere_vfs` and `pthread`

---

## Phase 4b — Core Node Types (Workloads 4.1–4.6)

These six workloads are pure serialization — they write into caller-provided
slots. They depend only on Phase 1 primitives (`vfs_wrX`/`vfs_rdX`), not on Pool.

- [ ] **Task 4b.1: Implement DirNode (4.1)**
  - `nodes_write_dirnode(uint8_t* slot, uint32_t nodeId, int64_t headPtr)`
    - Write type=0x01 at offset 0, nodeId at offset 4, headPtr at offset 8
    - Zero-fill reserved bytes (offsets 2-3, 16-31)
  - `nodes_read_dirnode(const uint8_t* slot, uint32_t* nodeId, int64_t* headPtr)`
    - Read nodeId and headPtr; caller checks type before calling
  - Tests: write with nodeId=5, headPtr=VFS_VPTR_MAKE(10,3) → read back; fresh zero slot → type=0x00

- [ ] **Task 4b.2: Implement FileNode (4.2)**
  - `nodes_write_filenode(uint8_t* slot, uint32_t nodeId, int64_t headPtr, int64_t sizePtr, int64_t createdAt)`
    - Write type=0x03 at 0, nodeId at 4, headPtr at 8, sizePtr at 16, createdAt at 24
  - `nodes_read_filenode(const uint8_t* slot, uint32_t* nodeId, int64_t* headPtr, int64_t* sizePtr, int64_t* createdAt)`
  - `nodes_read_filenode_ctime(const uint8_t* slot)` — reads only createdAt
  - Tests: full write+read round-trip; createdAt is valid Unix timestamp (±60s)

- [ ] **Task 4b.3: Implement DirContent (4.3)**
  - `nodes_write_dircontent(uint8_t* slot, uint32_t childNodeId, uint32_t epoch, int64_t childPtr, int64_t namePtr, int64_t nextPtr)`
    - Layout: childNodeId(0), epoch(4), childPtr(8), namePtr(16), nextPtr(24) — all 32 bytes packed
  - `nodes_read_dircontent(...)` — reads all 5 fields
  - Semantics: namePtr=0 means deleted (tombstone); chain via nextPtr in descending epoch
  - Tests: normal entry, tombstone, prepend 3 entries → walk chain → verify descending epoch

- [ ] **Task 4b.4: Implement FileContent (4.4)**
  - `nodes_write_filecontent(uint8_t* slot, int64_t pageRootPtr, int64_t nextPtr)`
    - Layout: pageRootPtr(0), nextPtr(8), reserved zero-fill 16-31
  - `nodes_read_filecontent(...)`
  - Tests: single entry round-trip; chain of two FileContents

- [ ] **Task 4b.5: Implement PageNode (4.5)**
  - `nodes_write_pagenode(uint8_t* slot, int64_t versionRootPtr, int64_t nextPtr)`
    - Layout: versionRootPtr(0), nextPtr(8), reserved zero-fill 16-31
  - `nodes_read_pagenode(...)`
  - Tests: PageNode with versionRootPtr=0; chain of 3 PageNodes

- [ ] **Task 4b.6: Implement VersionPage (4.6)**
  - `nodes_write_versionpage(uint8_t* slot, uint32_t epoch, int64_t dataPage, int64_t nextPtr)`
    - Layout: epoch(0), reserved(4), dataPage(8), nextPtr(16), reserved(24)
  - `nodes_read_versionpage(...)`
  - Tests: VersionPage {epoch=3, dataPage=100, nextPtr=ptr}; chain {epoch=5→epoch=3}

---

## Phase 4c — Metadata & Epoch Types (Workloads 4.7, 4.9, 4.10)

- [ ] **Task 4c.1: Implement FileSize (4.7)**
  - `nodes_write_filesize(uint8_t* slot, uint32_t epoch, int64_t modifiedAt, int64_t fileSize, int64_t nextPtr)`
    - Layout: epoch(0), modifiedAt(4), fileSize(12), nextPtr(20), reserved(28)
  - `nodes_read_filesize(...)`
  - Tests: single entry round-trip; chain of 2 entries with different sizes

- [ ] **Task 4c.2: Implement TouchedFile (4.9)**
  - `nodes_write_touchedfile(uint8_t* slot, uint32_t epoch, uint32_t nodeId, int64_t nextPtr)`
    - Layout: epoch(0), nodeId(4), nextPtr(8), reserved zero-fill 16-31
  - `nodes_read_touchedfile(...)`
  - Tests: write+read round-trip; dedup (caller-side, not in helper — verify second write for same (epoch,nodeId) can be detected)

- [ ] **Task 4c.3: Implement MapperEntry (4.10)**
  - `nodes_write_mapperentry(uint8_t* slot, uint32_t fromEpoch, uint32_t toEpoch, uint16_t flags, int64_t nextPtr)`
    - Layout: fromEpoch(0), toEpoch(4), flags(8), reserved(10), nextPtr(16), reserved(24)
    - `#define MAPPER_FLAG_TRAVERSAL_APPLY 0x0001`
  - `nodes_read_mapperentry(...)`
  - Tests: commit mapping (traversalApply=true); soft-delete (traversalApply=false)

---

## Phase 4d — NameEntry Chain (Workload 4.8)

This is the only workload that depends on a fully functional `Pool*` since
`nodes_write_name` must call `pool_alloc` for chained slots.

- [ ] **Task 4d.1: Implement NameEntry write/read helpers**
  - `nodes_write_name(Pool* pool, const char* utf8_name, int64_t* first_slot_vp)`
    - Computes number of slots needed: `ceil(strlen(name) / 24)`
    - Allocates each slot via `pool_alloc`, writes 24 bytes of UTF-8 data, sets nextPtr
    - Returns number of slots written (1 or more); stores first slot VP in `*first_slot_vp`
    - Last slot has `nextPtr = 0`; data is zero-padded if shorter than 24
  - `nodes_read_name(Pool* pool, int64_t first_slot_vp, char* out_buf, int max_len)`
    - Walks chain via nextPtr, concatenates data bytes
    - Stops at first zero byte in any slot's data OR when nextPtr is 0
    - Returns total name length (excluding null terminator)
  - Internal helper: `nodes_write_name_entry(uint8_t* slot, const uint8_t* data_24, int64_t nextPtr)` — writes a single slot (used by `nodes_write_name`)

- [ ] **Task 4d.2: NameEntry tests**
  - Short name (5 bytes) → 1 slot, nextPtr=0, round-trip
  - 50-byte name → 3 slots (24+24+2), chain linked, concatenation matches
  - Name exactly 24 bytes → 1 slot, no padding issues
  - Name exactly 48 bytes → 2 slots
  - Unicode names (multi-byte UTF-8) — verify byte-exact reconstruction
  - Empty name (0 bytes) — should error or return 0 slots (caller must ensure non-empty)

---

## Phase 4e — Integration Tests & Final Checklist

- [ ] **Task 4e.1: Final acceptance tests in `test/test_nodes.c`**
  - Every node type allocated → written → read back → all fields preserved
  - DirContent chain: prepend 3, walk via nextPtr, verify descending epoch
  - NameEntry chain: 50-byte name → 3 slots → reconstruction matches
  - FileSize chain: 2 entries → query at different epochs → correct sizes
  - TouchedFile dedup: second write for same (epoch, nodeId) creates no duplicate
  - MapperEntry: both flag states writable and readable
  - Fresh zero-filled slot → safe to read as any type (all fields zero)

- [ ] **Task 4e.2: Build and run tests**
  - `mkdir -p build && cd build && cmake .. && make`
  - Run `./test_nodes` — verify all tests pass
  - Run `./test_pool` — verify Phase 3 still passes (no regressions)
  - Run `./test_storage` — verify Phase 2 still passes
  - Run `./vfs_test` — verify Phase 1 still passes

---

## Execution Order

1. **4a.1 + 4a.2** (header file + build) — first, so .c files compile
2. **4b.1 → 4b.6** (core node types) — simplest, no Pool dependency beyond VirtualPtr macros
3. **4c.1 → 4c.3** (metadata types) — similar complexity
4. **4d.1 + 4d.2** (NameEntry) — requires Pool, harder; do after simpler types are working
5. **4e.1 + 4e.2** (integration + final build) — run everything

Tasks within 4b and 4c are independent and can be parallelized across sessions.

## Design Decisions & Notes

- **No runtime behavior:** All helpers just serialize/deserialize. Tree traversal, CAS-prepend, version chain walking — all deferred to Phase 5.
- **VirtualPtr encoding:** Use `VFS_VPTR_MAKE(page, slot)` consistently. All `nextPtr`/`headPtr`/`childPtr`/etc. fields use `int64_t` type for VirtualPtrs.
- **Type checking:** DirNode and FileNode have a `type` field. Caller must check type before calling `nodes_read_dirnode` or `nodes_read_filenode`. The helpers do NOT validate the type field internally (they are low-level).
- **Reserved fields:** Always zero-filled on write. Ignored on read.
- **NameEntry empty-name handling:** `nodes_write_name` should return 0 and set `*first_slot_vp = VFS_VPTR_NULL` for empty strings, since `namePtr=0` is the tombstone sentinel for DirContent.
- **Test file locations:** Tests use `/tmp/test_nodes_*.vfs` for pool-backed tests (NameEntry). Non-pool tests (all other types) operate on stack-allocated `uint8_t slot[32]` — no disk I/O needed.
- **Include dependencies:** `nodes.h` includes `pool.h` (for `Pool*` type and VirtualPtr macros) and `page_buf.h` (for vfs_rdX/vfs_wrX). `nodes.c` includes `nodes.h` plus `<string.h>` for memcpy/strlen.
