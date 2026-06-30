# Phase 9: SQLite VFS Integration

## Goal
Integrate iXSphereVFS as a SQLite Virtual File System backend. SQLite's
`sqlite3_vfs` interface is implemented as a thin layer that translates
SQLite's xOpen/xRead/xWrite/xFileSize/xSync/xLock/xUnlock/xFileControl/xClose
callbacks into iXSphereVFS API calls. The CowVfs layer manages per-connection
workspace epochs for snapshot-isolated transactions.

---

## Workload 9.1 — VFS Registration

**What:** Register one or more named VFS instances with SQLite. Each instance
wraps the same underlying iXSphereVFS FileSystem but provides an isolated
connection state.

**Why:** SQLite selects a VFS by name in the connection string. Multiple named
instances allow concurrent connections to share the same backing file while
maintaining per-connection lock and workspace state.

**How:**
- `vfs_register_sqlite(const char* name, vfs_t* vfs)`: create a new
  `sqlite3_vfs` structure that wraps the given iXSphereVFS instance.
  Clone SQLite's default VFS to inherit defaults for xDelete, xAccess,
  xFullPathname, xRandomness, xSleep, xCurrentTime. Override only the I/O
  and locking callbacks.
- The `sqlite3_vfs` structure stores the iXSphereVFS handle and the VFS name
  in its `pAppData` field.
- Register the new VFS with SQLite via `sqlite3_vfs_register`.
- Each `sqlite3_file` (per-open-file handle) holds a `CowFile` state struct
  containing: the iXSphereVFS file nodeId, the current `LockLevel`, the
  `_snapshotEpoch` (workspace epoch or LiveHead sentinel), a reference to
  the shared `SnapshotManager`, and a list of held per-inode locks.
- Journal and WAL files are identified by name suffix (`-journal`, `-wal`)
  and are created as regular iXSphereVFS files. They bypass the PageMap
  and per-table inode logic.

**Acceptance:**
  - `vfs_register_sqlite` returns a valid `sqlite3_vfs*` pointer.
  - SQLite can open a database using the VFS name: `sqlite3_open_v2("test.db",
    ..., "myvfs")`.
  - Journal files are created alongside the main database file.
  - Multiple VFS instances with different names can coexist, sharing the same
    underlying iXSphereVFS instance.

---

## Workload 9.2 — File Mapping (PageMap)

**What:** Map SQLite database page numbers to iXSphereVFS file nodes. Each
SQLite table gets its own file node for snapshot-isolated COW. This is the
per-table inode mechanism.

**Why:** SQLite stores all tables in a single database file. For COW to work
at the table level (a write to table A should not copy table B's pages),
each SQLite btree must have its own iXSphereVFS file node. The PageMap
tracks which SQLite page belongs to which file node.

**How:**
- `PageMap` is a per-connection hash table: `sqlitePageNumber → vfsFileNodeId`.
  Stored in the `CowFile` struct.
- On `xOpen` for the main database (not journal): create a PageMap. The
  default file node (the database root inode) is stored for page 1 (the
  sqlite_master schema table).
- `PageMap.GetInode(sqlitePage, currentRoot)`: if the page is already mapped,
  return its file node. If unmapped, check whether a per-table file node
  already exists (by looking up `__table_N` in the VFS root directory). If
  yes, cache the mapping. If no, and `currentRoot > 1`, auto-assign the page
  to the current root's file node (btree split pages belong to the same table).
  Otherwise return the default file node.
- `PageMap.Ensure(sqlitePage)`: create a new per-table file node for the given
  root page. This is called from the `COW_TABLE_LIST` xFileControl handler
  when SQLite opens a table for writing. The new file node is created under
  root with name `__table_N` and its initial content is copied from the
  default file node at the page's offset.
- `PageMap.ClearPage(sqlitePage)`: when SQLite frees a page, clear its mapping.
  The next page allocated at this slot will auto-assign to its new owner.
- `ResolveOffset(sqliteOffset)`: translate a SQLite file offset into an
  (iXSphereVFS file node, offset within that file node). The offset within
  the file node is the SQLite page offset minus the root page's starting
  offset.

**Acceptance:**
  - Writing to SQLite table "users" (root page 3) creates file node `__table_3`.
  - Writing to table "orders" (root page 4) creates file node `__table_4`.
  - A btree split in table "users" auto-assigns new pages to `__table_3`.
  - Reading table "users" at a snapshot epoch returns consistent data.
  - Journal file pages are NOT mapped to per-table inodes (they use the
    journal file's own file node).

---

## Workload 9.3 — I/O Callbacks

**What:** Implement xRead, xWrite, xFileSize, xSync, xClose as thin wrappers
around iXSphereVFS API calls, with workspace epoch routing.

**Why:** SQLite's pager calls these to read and write database pages. The
CowVfs layer must route reads and writes through the correct epoch (live
head for reads outside transactions, workspace epoch for writes inside
transactions).

**How:**
- `xRead(sqlite3_file*, void* buf, int amt, sqlite3_int64 offset)`:
  1. Resolve the SQLite offset to (fileNode, fileOffset) via PageMap.
  2. Call `vfs_read(vfs, fileNode, buf, fileOffset, amt, cf->_snapshotEpoch)`.
     If the file node has a workspace copy for this epoch, the read returns
     workspace data. Otherwise it falls through to the live head.
  3. If the read returns fewer bytes than requested, zero-fill the remainder.
- `xWrite(sqlite3_file*, const void* buf, int amt, sqlite3_int64 offset)`:
  1. Copy the data from SQLite's buffer into a VFS buffer (SQLite's buffer
     may be in native memory, so a `memcpy` is needed).
  2. Resolve to (fileNode, fileOffset).
  3. Call `vfs_write(vfs, fileNode, data, fileOffset, amt, cf->_snapshotEpoch)`.
     If `_snapshotEpoch` is LiveHead (-1), the write goes to the live head
     directly. If it's a workspace epoch, the write goes through the
     snapshot workspace.
- `xFileSize(sqlite3_file*, sqlite3_int64* size)`:
  1. Use the PageMap to determine which file nodes exist.
  2. Sum the sizes or use the indirection table to compute the database size to compute the total database
     size as `(maxPage + 1) * pageSize`.
- `xSync(sqlite3_file*, int flags)`: call `vfs_flush(vfs)`.
- `xClose(sqlite3_file*)`: release all held per-inode locks. If a workspace
  is active and uncommitted, roll it back. Free the CowFile state.

**Acceptance:**
  - SQLite INSERT → xWrite → data persisted in the correct file node.
  - SQLite SELECT → xRead → returns previously written data.
  - xFileSize returns the correct database size.
  - xSync flushes all dirty pages to disk.
  - xClose cleans up locks and rolls back uncommitted workspaces.

---

## Workload 9.4 — Locking & Workspace Management

**What:** Translate SQLite's lock levels (SHARED, RESERVED, PENDING, EXCLUSIVE)
into iXSphereVFS per-file locks and workspace epochs.

**Why:** SQLite uses file-level locks to coordinate concurrent access. The
CowVfs maps these to iXSphereVFS's dual-mode locking. The transition to
RESERVED lock starts a snapshot workspace for transaction isolation.

**How:**
- `xLock(sqlite3_file*, int level)`:
  - Track the lock level in `cf->LockLevel`. If `level <= LockLevel`, return
    immediately.
  - On transition from below RESERVED to RESERVED or higher:
    - Take a snapshot: `cf->_snapshotEpoch = vfs_snapshot(vfs)`.
    - The snapshot epoch becomes the workspace epoch. All subsequent writes
      at this epoch are isolated from the live head.
  - Acquire the global file lock: `vfs_lock(vfs, fileNode, 0)` (epoch=0
    for SQLite compatibility — serializes all writers).
- `xUnlock(sqlite3_file*, int level)`:
  - If releasing below RESERVED: release the global file lock.
  - Update `LockLevel`.
- `xCheckReservedLock(sqlite3_file*, int* result)`: check whether another
  connection holds the global lock on this file.
- `xFileControl(sqlite3_file*, int op, void* arg)`:
  - `COW_TXN_BEGIN` (op 46): no action needed (workspace starts at RESERVED).
  - `COW_TXN_COMMIT` (op 47): commit the workspace epoch via
    `vfs_commit(vfs, cf->_snapshotEpoch)`. Reset `_snapshotEpoch` to LiveHead.
    Release all per-table locks. Clear the lock level.
  - `COW_TXN_ROLLBACK` (op 48): rollback the workspace (soft-delete the
    snapshot epoch via `vfs_delete_snapshot`). Reset `_snapshotEpoch` to
    LiveHead. Release locks.
  - `COW_PAGE_CTX_BEGIN` (op 49): record the current root page for auto-assign.
  - `COW_PAGE_FREE` (op 52): clear the page mapping via `PageMap.ClearPage`.
  - `COW_TABLE_LIST` (op 51): acquire per-table locks for all tables about
    to be written. For each table root page, call `PageMap.Ensure` and
    `vfs_lock(vfs, tableFileNode, cf->_snapshotEpoch)` (per-epoch lock).
- Workspace lifecycle: starts at RESERVED lock, ends at COMMIT or ROLLBACK.
  If the connection closes with an active workspace, xClose rolls it back.

**Acceptance:**
  - Single writer: RESERVED lock acquired → workspace started → writes at
    snapshot epoch → COMMIT → data visible.
  - Two readers: both hold SHARED locks concurrently.
  - Writer blocks until readers release SHARED (when transitioning to EXCLUSIVE).
  - Crash before COMMIT: on remount, the workspace epoch is gone (soft-deleted
    or rolled back by xClose).
  - Per-table locks: writing to table A does not block reading table B.

---

## Workload 9.5 — Integration Tests

**What:** End-to-end tests exercising SQLite with the CowVfs backend.

**Why:** The SQLite integration must pass functional correctness tests before
performance benchmarking. Basic CRUD, schema changes, concurrent access,
and crash recovery must all work.

**How:**
- Test setup: create an iXSphereVFS instance, register one or more CowVfs
  instances, open a SQLite database using the VFS name.
- Functional tests:
  1. CREATE TABLE, INSERT, SELECT: basic CRUD.
  2. UPDATE and DELETE: in-place modifications.
  3. Transactions: BEGIN → INSERT → COMMIT, BEGIN → INSERT → ROLLBACK.
  4. Schema changes: CREATE INDEX, ALTER TABLE, DROP TABLE.
  5. Concurrency: two connections, writer inserts while reader selects.
  6. Large dataset: insert 10,000 rows, verify COUNT and ORDER BY.
- All tests use the `sqlite3` C API directly.
- Error handling: verify that SQLite errors are propagated correctly through
  the VFS layer.

**Acceptance:**
  - CREATE TABLE + INSERT + SELECT returns correct rows.
  - ROLLBACK reverts uncommitted inserts.
  - Two connections can read and write concurrently without corruption.
  - 10,000 rows inserted and verified via COUNT and ORDER BY.
  - Database survives process restart (close, reopen, verify data).

---

## Deliverables

| File | Purpose |
|------|---------|
| `src/sqlite_vfs.c` | VFS registration, I/O callbacks, workspace management |
| `src/sqlite_pagemap.c` | PageMap: SQLite page → file node mapping |
| `test/test_sqlite.c` | Functional integration tests with SQLite C API |

## Success Criteria
- SQLite can open, read, write, and close a database via the CowVfs.
- Transactions are correctly isolated: uncommitted writes are invisible to
  other connections.
- ROLLBACK correctly reverts uncommitted changes.
- Per-table inodes (PageMap) correctly isolate writes to different tables.
- Concurrent connections do not corrupt data.
- Database survives process restart.
