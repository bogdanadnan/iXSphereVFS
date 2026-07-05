# Phase 10: SQLite VFS Integration

## Goal
Integrate iXSphereVFS as a SQLite Virtual File System backend. SQLite's
`sqlite3_vfs` interface is implemented as a thin layer that translates
SQLite's I/O and locking callbacks into iXSphereVFS API calls. The CowVfs
layer manages per-connection workspace epochs for snapshot-isolated
transactions.

## Non-Negotiable Constraints

- **Must pass SQLite's built-in tests.** SQLite has its own test harness
  (`sqlite3_test` or the TCL test suite). At minimum, the VFS must survive
  `PRAGMA integrity_check` and basic CRUD.
- **Must not corrupt data.** A crash during a transaction must leave the
  database recoverable, either via the workspace epoch being rolled back or
  via SQLite's own recovery if journal files are in use.
- **Journal and WAL files must work.** These are just regular iXSphereVFS
  files created alongside the main database. They bypass the PageMap.
- **Per-table inodes via PageMap.** Each SQLite table gets its own iXSphereVFS
  file node for COW isolation at the btree level. Writes to table A don't
  copy table B's pages.

## File Organization

| File | Purpose |
|------|---------|
| `src/sqlite_vfs.c` | VFS registration, I/O callbacks, workspace management |
| `src/sqlite_pagemap.c` | PageMap: SQLite page → iXSphereVFS file node mapping |
| `test/test_sqlite.c` | Functional integration tests with SQLite C API |

## Dependencies
- Phase 8 (Filesystem API) — fully functional
- SQLite amalgamation (`sqlite3.c` + `sqlite3.h`) — compile against it
- SQLite must be compiled with `-DSQLITE_OS_OTHER=1` or the VFS registered
  via `sqlite3_vfs_register`

---

## Workload 9.1 — VFS Registration

### What
Register a named VFS with SQLite. Each instance wraps an iXSphereVFS handle
and provides a `sqlite3_vfs` structure with overridden I/O methods.

### Data Structures

```c
typedef struct {
    sqlite3_vfs    base;           // must be first field
    vfs_t*         ixs;            // iXSphereVFS handle
} IxVfs;

typedef struct {
    sqlite3_file   base;           // must be first field
    IxVfs*         vfs;            // back-pointer to the VFS
    int64_t        fileNodeId;     // iXSphereVFS nodeId for this file
    int            lockLevel;      // current SQLite lock level (0=none, 1=shared, 2=reserved, 3=pending, 4=exclusive)
    int64_t        snapshotEpoch;  // workspace epoch, or -1 (LiveHead)
    PageMap*       pageMap;        // SQLite page → VFS file node mapping (main DB only)
    bool           isJournal;      // true if this file is a journal or WAL
    bool           isTemp;         // true for temporary files
} IxFile;
```

### `int ix_vfs_register(const char* name, vfs_t* ixs)`

```
1. Allocate IxVfs struct. memset to zero.
2. Copy SQLite's default VFS as a template: memcpy(&ix->base, sqlite3_vfs_find(NULL), sizeof(sqlite3_vfs))
3. Set ix->base.szOsFile = sizeof(IxFile)
4. Set ix->base.zName = strdup(name)
5. Set ix->base.pAppData = ix  // back-pointer for callbacks
6. Override method pointers:
   ix->base.xOpen       = ixOpen
   ix->base.xRead       = ixRead
   ix->base.xWrite      = ixWrite
   ix->base.xFileSize   = ixFileSize
   ix->base.xSync       = ixSync
   ix->base.xLock       = ixLock
   ix->base.xUnlock     = ixUnlock
   ix->base.xCheckReservedLock = ixCheckReservedLock
   ix->base.xClose      = ixClose
   ix->base.xFileControl = ixFileControl
   ix->base.xSectorSize = 8192
   ix->base.xDeviceCharacteristics = SQLITE_IOCAP_ATOMIC | SQLITE_IOCAP_SAFE_APPEND
7. Set ix->ixs = ixs
8. sqlite3_vfs_register(&ix->base, 0)  // 0 = make default? No — use 1 to make non-default
9. Return 0 on success.
```

### Acceptance
- [ ] `ix_vfs_register("ixsphere", ixs)` returns 0
- [ ] `sqlite3_open_v2("test.db", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "ixsphere")` succeeds
- [ ] Journal file created alongside main database
- [ ] Two connections with different VFS names can share same iXSphereVFS handle

---

## Workload 9.2 — PageMap (Per-Table Inode Mapping)

### What
Maps SQLite database page numbers to iXSphereVFS file nodeIds. Each SQLite
btree root gets its own file node. Journal pages bypass the PageMap.

### Data Structure

```c
typedef struct {
    // Simple array-based map: pageNumber → fileNodeId
    // Sparse — most pages are unmapped. Use hash table or dynamic array.
    int64_t*  nodeIds;       // nodeIds[N] = file nodeId for SQLite page N, or 0 (unmapped)
    int       capacity;      // allocated size of nodeIds[]
    int64_t   defaultNodeId; // the main database file's nodeId (used for unmapped pages)
} PageMap;
```

### `pagemap_get(PageMap* pm, int sqlitePage, int currentRoot) → fileNodeId`

```
1. If sqlitePage < pm->capacity && pm->nodeIds[sqlitePage] != 0:
     return pm->nodeIds[sqlitePage]  // already mapped
2. If currentRoot > 1:
     // Auto-assign: this page belongs to the current root's file node
     // (btree internal pages use the same file node as their root)
     return pagemap_get(pm, currentRoot, 0)
3. return pm->defaultNodeId  // unmapped page → use main database file
```

### `pagemap_ensure(PageMap* pm, int sqlitePage, vfs_t* ixs, int64_t epoch) → fileNodeId`

Called when SQLite opens a table for writing (COW_TABLE_LIST op).

```
1. Create a new iXSphereVFS file node under root with name "__table_N"
   where N = sqlitePage (the root page).
   fileNodeId = vfs_create(ixs, ROOT_NODE_ID, name, epoch)
2. Copy initial content: read the page from the default file node at the
   SQLite offset, write it to the new file node.
3. Set pm->nodeIds[sqlitePage] = fileNodeId
4. Also map the root page itself: pm->nodeIds[sqlitePage] = fileNodeId
5. Return fileNodeId
```

### `pagemap_clear(PageMap* pm, int sqlitePage)`

```
1. If sqlitePage < pm->capacity:
     pm->nodeIds[sqlitePage] = 0
```

### Acceptance
- [ ] Write to table "users" (root page 3): creates file node `__table_3`
- [ ] Write to table "orders" (root page 4): creates file node `__table_4`
- [ ] Btree split in "users": new pages auto-assigned to `__table_3`
- [ ] Journal pages always return defaultNodeId (never mapped)
- [ ] `pagemap_clear` correctly unmaps freed pages

---

## Workload 9.3 — I/O Callbacks

### `static int ixOpen(sqlite3_vfs* vfs, const char* name, sqlite3_file* file, int flags, int* outFlags)`

```
1. Determine file type from name:
   - name ends with "-journal" → isJournal = true
   - name ends with "-wal" → isJournal = true
   - name is NULL or empty → temp file
2. If file does not exist AND flags includes SQLITE_OPEN_CREATE:
   - If isJournal: vfs_create(ixs, ROOT_NODE_ID, filename, -1)
   - Else (main DB): vfs_create(ixs, ROOT_NODE_ID, filename, -1)
     and create PageMap with defaultNodeId = new file's nodeId
3. If file exists: vfs_open(ixs, ROOT_NODE_ID, filename, -1)
4. Initialize IxFile fields:
   file->fileNodeId = result from step 2 or 3
   file->lockLevel = 0
   file->snapshotEpoch = -1 (LiveHead)
   file->pageMap = (isJournal ? NULL : new PageMap)
5. Return SQLITE_OK.
```

### `static int ixRead(sqlite3_file* file, void* buf, int amt, sqlite3_int64 offset)`

```
IxFile* ix = (IxFile*)file
1. Resolve to (fileNodeId, fileOffset) via PageMap:
   sqlitePage = offset / 8192
   fileNodeId = pagemap_get(ix->pageMap, sqlitePage, 0)
   fileOffset = (offset - rootPageOffset) ... // compute offset within the file node
                                               // For the main DB file, this is just 'offset'
                                               // For per-table files, adjust based on root page
2. epoch = (ix->snapshotEpoch == -1) ? -1 : ix->snapshotEpoch
3. bytes = vfs_read(ix->vfs->ixs, fileNodeId, buf, fileOffset, amt, epoch)
4. If bytes < amt: memset(buf + bytes, 0, amt - bytes)  // zero-fill short reads
5. Return SQLITE_IOERR_SHORT_READ if bytes < amt, else SQLITE_OK
```

### `static int ixWrite(sqlite3_file* file, const void* buf, int amt, sqlite3_int64 offset)`

```
IxFile* ix = (IxFile*)file
1. Resolve via PageMap (same as Read).
2. epoch = (ix->snapshotEpoch == -1) ? -1 : ix->snapshotEpoch
3. bytes = vfs_write(ix->vfs->ixs, fileNodeId, buf, fileOffset, amt, epoch)
4. If bytes != amt: return SQLITE_FULL
5. Return SQLITE_OK
```

### `static int ixFileSize(sqlite3_file* file, sqlite3_int64* size)`

```
IxFile* ix = (IxFile*)file
1. If journal: *size = vfs_file_size(ix->vfs->ixs, ix->fileNodeId, -1)
2. Else (main DB):
   // Sum sizes of all per-table file nodes + default file node
   // OR use the VFS's own tracking
   *size = (max_allocated_page + 1) * 8192
3. Return SQLITE_OK
```

### `static int ixSync(sqlite3_file* file, int flags)`

```
1. vfs_flush(ix->vfs->ixs)
2. Return SQLITE_OK
```

### `static int ixClose(sqlite3_file* file)`

```
IxFile* ix = (IxFile*)file
1. If ix->snapshotEpoch != -1:  // uncommitted workspace
     vfs_delete_snapshot(ix->vfs->ixs, ix->snapshotEpoch)  // rollback
2. Release all held locks via vfs_unlock
3. Free PageMap
4. Free IxFile
5. Return SQLITE_OK
```

### Acceptance
- [ ] SQLite INSERT → ixWrite called → data written to correct file node
- [ ] SQLite SELECT → ixRead called → returns previously written data
- [ ] ixFileSize returns correct size
- [ ] ixSync flushes dirty pages
- [ ] ixClose cleans up uncommitted workspace and releases locks

---

## Workload 9.4 — Locking & Workspace Management

### What
Translate SQLite's 5 lock levels into iXSphereVFS locks and workspace epochs.
The transition to RESERVED starts a snapshot workspace.

### `static int ixLock(sqlite3_file* file, int level)`

```
IxFile* ix = (IxFile*)file
if level <= ix->lockLevel: return SQLITE_OK

if ix->lockLevel < 2 && level >= 2:  // transitioning TO RESERVED or higher
    // Start workspace
    ix->snapshotEpoch = vfs_snapshot(ix->vfs->ixs)
    // Acquire global file lock for SQLite compatibility
    vfs_lock(ix->vfs->ixs, ix->fileNodeId, 0)

if level >= 4:  // EXCLUSIVE
    // Ensure no other readers: acquire exclusive access
    // SQLite ensures this by the time we get to EXCLUSIVE

ix->lockLevel = level
return SQLITE_OK
```

### `static int ixUnlock(sqlite3_file* file, int level)`

```
IxFile* ix = (IxFile*)file
if level >= ix->lockLevel: return SQLITE_OK

if ix->lockLevel >= 2 && level < 2:  // releasing FROM RESERVED or higher
    // Release global file lock
    vfs_unlock(ix->vfs->ixs, ix->fileNodeId, 0)

ix->lockLevel = level
return SQLITE_OK
```

### `static int ixCheckReservedLock(sqlite3_file* file, int* result)`

```
// Check if another connection holds the global lock on this file.
// This is a best-effort check: try vfs_lock with non-blocking, then unlock.
*result = 0  // simplified: assume no reserved lock; SQLite handles contention
return SQLITE_OK
```

### `static int ixFileControl(sqlite3_file* file, int op, void* arg)`

```
switch op:
    case COW_TXN_COMMIT:   // op 47
        vfs_commit(ix->vfs->ixs, ix->snapshotEpoch)
        // Release per-table locks (acquired during COW_TABLE_LIST)
        for each per-table file node locked:
            vfs_unlock(ix->vfs->ixs, tableNodeId, ix->snapshotEpoch)
        ix->snapshotEpoch = -1
        ix->lockLevel = 0
        return SQLITE_OK

    case COW_TXN_ROLLBACK:  // op 48
        vfs_delete_snapshot(ix->vfs->ixs, ix->snapshotEpoch)
        ix->snapshotEpoch = -1
        ix->lockLevel = 0
        return SQLITE_OK

    case COW_TABLE_LIST:    // op 51
        // arg is pointer to array of root page numbers (int32_t*)
        // For each root page:
        for each root in arg:
            fileNodeId = pagemap_ensure(ix->pageMap, root, ix->vfs->ixs, ix->snapshotEpoch)
            vfs_lock(ix->vfs->ixs, fileNodeId, ix->snapshotEpoch)  // per-epoch lock
        return SQLITE_OK

    case COW_PAGE_FREE:     // op 52
        sqlitePage = *(int*)arg
        pagemap_clear(ix->pageMap, sqlitePage)
        return SQLITE_OK

    default:
        return SQLITE_NOTFOUND
```

### Acceptance
- [ ] Single writer: RESERVED → workspace started → writes isolated → COMMIT → visible
- [ ] Two readers: both hold SHARED concurrently
- [ ] `kill -9` before COMMIT → remount → workspace epoch rolled back → database intact
- [ ] Per-table locks: writing table A doesn't block reading table B
- [ ] COW_TABLE_LIST correctly creates and locks per-table file nodes

---

## Workload 9.5 — Integration Tests

### What
End-to-end tests using the `sqlite3` C API against the CowVfs backend.

### Test Cases (All Required)

```
1. test_create_table()
   CREATE TABLE t1(a INTEGER PRIMARY KEY, b TEXT)
   Verify: table exists (sqlite_master query)

2. test_insert_select()
   INSERT INTO t1 VALUES (1, 'hello'), (2, 'world')
   SELECT * FROM t1 ORDER BY a
   Verify: two rows, correct values

3. test_update()
   UPDATE t1 SET b = 'updated' WHERE a = 1
   SELECT b FROM t1 WHERE a = 1
   Verify: returns 'updated'

4. test_delete()
   DELETE FROM t1 WHERE a = 2
   SELECT COUNT(*) FROM t1
   Verify: returns 1

5. test_transaction_commit()
   BEGIN; INSERT INTO t1 VALUES (3, 'txn'); COMMIT;
   SELECT COUNT(*) FROM t1
   Verify: returns 2 (assuming one row from previous tests)

6. test_transaction_rollback()
   BEGIN; INSERT INTO t1 VALUES (4, 'rollback'); ROLLBACK;
   SELECT COUNT(*) FROM t1
   Verify: still 2 (rollback reverted)

7. test_concurrency()
   Connection 1: BEGIN; INSERT INTO t1 VALUES (5, 'concurrent');
   Connection 2: SELECT COUNT(*) FROM t1 (should see pre-insert count)
   Connection 1: COMMIT;
   Connection 2: SELECT COUNT(*) FROM t1 (now sees +1)
   Verify: isolation works

8. test_large_insert()
   CREATE TABLE t2(a INTEGER, b TEXT)
   Insert 10,000 rows
   SELECT COUNT(*) FROM t2
   Verify: returns 10000

9. test_reopen()
   Close database, reopen
   SELECT COUNT(*) FROM t1
   Verify: data survived close/reopen

10. test_integrity()
    PRAGMA integrity_check
    Verify: returns 'ok'
```

### Acceptance
- [ ] All 10 test cases pass
- [ ] `PRAGMA integrity_check` returns "ok"
- [ ] No SQLite warnings or errors in the log
- [ ] Multi-connection test passes (isolation verified)

---

## Final Phase 9 Checklist

- [ ] VFS registered with SQLite; database can be opened
- [ ] CREATE TABLE, INSERT, SELECT, UPDATE, DELETE all work
- [ ] Transactions: COMMIT persists, ROLLBACK reverts
- [ ] Concurrent connections read and write without corruption
- [ ] Per-table inodes created for each SQLite btree
- [ ] `kill -9` during transaction → database recoverable after remount
- [ ] `PRAGMA integrity_check` passes on a populated database
