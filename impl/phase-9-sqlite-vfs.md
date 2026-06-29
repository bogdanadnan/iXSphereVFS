# Phase 9: SQLite VFS Integration

## Goal
`CowVfs` I/O callback layer mapping SQLite's `sqlite3_vfs` interface to iXSphereVFS.

## Workloads

### 9.1 VFS Registration
- `vfs_register_sqlite(name)` — register named VFS with SQLite
- Clone SQLite's default `sqlite3_vfs`, override `xOpen/xRead/xWrite/xFileSize/xSync/xLock/xUnlock/xFileControl/xClose`
- Allocate `sqlite3_io_methods` vtable

### 9.2 File Mapping
- SQLite database file → iXSphereVFS file node
- Journal/WAL files → iXSphereVFS file nodes (skip PageMap for journal files)
- `XOpen`: create or open file node, allocate CowFile state
- `XClose`: release locks, drop CowFile state

### 9.3 I/O Translation
- `XRead`: offset → page → resolve via vfs_read with current workspace epoch
- `XWrite`: offset → page → vfs_write with current workspace epoch
- `XFileSize`: vfs_file_size via read rule
- `XSync`: vfs_flush

### 9.4 Locking
- `XLock`/`XUnlock`: SQLite lock levels mapped to vfs_lock(global=true) for RESERVED+
- Workspace epoch started at XLock(RESERVED) via vfs_snapshot()
- Committed in `XFileControl(COMMIT)` via vfs_commit()

### 9.5 PageMap (Per-Table Inodes)
- Map SQLite database page → iXSphereVFS file node
- `PageMap_Ensure(pg)`: lookup or create per-table file node
- Auto-assign child pages to current root's per-table inode during CTX_BEGIN scope
- `ClearPage(pg)`: on SQLite page free, clear page→inode mapping

### 9.6 Tests
- SQLite open/create table/insert/select via CowVfs
- Multi-connection concurrency (separate VFS instances, same FileSystem)
- Individual INSERT benchmark: 200 rows × 4 writers
