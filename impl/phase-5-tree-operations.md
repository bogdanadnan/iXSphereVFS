# Phase 5: Tree Operations

## Goal
File/directory CRUD, read/write with version chains, in-memory page arrays, directory listing.

## Workloads

### 5.1 Superblock Bootstrap
- After StorageBackend init: `Acquire(3)` → init superblock
- Set currentEpoch=0, rootNodeOffset=0 (no root dir yet)
- Create root DirNode (nodeId=0), set superblock.rootNodeOffset to its VirtualPtr

### 5.2 File Create / Delete
- `vfs_create(parent, name, epoch)`: allocate FileNode, allocate NameEntry, CAS-prepend DirContent to parent
- `vfs_delete(parent, name, epoch)`: CAS-prepend DirContent with namePtr=0
- Tests: create file, verify parent listing, delete, verify removed

### 5.3 File Write
- `vfs_write(file, offset, data, count, epoch)`:
  1. Walk FileContent → find segment → walk PageNode → resolve VersionPage chain
  2. If VersionPage for current epoch exists → in-place write to dataPage (lazy mirror)
  3. Else → COW: allocate new dataPage, copy base + overlay, CAS-prepend VersionPage
  4. Update FileSize if offset+count > current size
- In-memory array: build on first access to segment (VirtualPtr array)
- Tests: write 1 page, read back. Write 3000 pages → 3 segments → array rebuild on growth

### 5.4 File Read
- `vfs_read(file, buf, offset, count, epoch)`: resolve via in-memory array → walk version chain → read dataPage
- Cross-page reads: split across PageNodes
- Tests: write 2 pages, read back across page boundary

### 5.5 Directory Operations
- `vfs_mkdir`: allocate DirNode, CAS-prepend DirContent to parent
- `vfs_rmdir`: validate empty, CAS-prepend delete entry
- `vfs_readdir`: walk DirNode.headPtr → deduplicate by childNodeId → collect names
- Dentry cache: in-memory `(childNodeId → name, childPtr)` map, invalidated on new DirContent
- Tests: create dir, create file inside, list, delete file, rmdir

### 5.6 Rename
- Same-dir same-epoch: update namePtr in-place on existing DirContent
- Cross-dir or new epoch: create new DirContent at dest, delete entry at source
- Tests: rename same-dir, rename cross-dir, verify both paths

### 5.7 File Size & Stats
- Walk FileNode.sizePtr chain via read rule → get size + mtime
- FileNode.createdAt for ctime
- Tests: write → check size. Write more → check size increased. Snapshot → check old size

## Deliverables
- `src/tree.c` with all operations
- `test/test_tree.c` with CRUD + multi-segment tests
