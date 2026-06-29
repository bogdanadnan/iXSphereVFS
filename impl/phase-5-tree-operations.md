# Phase 5: Tree Operations

## Goal
Implement all filesystem operations: create, read, write, delete, rename,
directory listing, and file stat. This includes building in-memory page
arrays for O(1) lookup, the dentry cache for directory listings, and the
read rule with epoch mapping for snapshot isolation.

---

## Workload 5.1 — Superblock Bootstrap & Root Directory

**What:** Initialize the superblock on first use and create the root directory
node. This is the starting point of the VFS tree.

**Why:** Every tree traversal begins at the root. The superblock holds the
VirtualPtr to the root DirNode. On a fresh VFS instance, both must be
created before any file or directory operations.

**How:**
- After StorageBackend initialization and `Acquire(3)` for the superblock
  page, the VFS layer initializes the superblock payload with `rootNodeOffset
  = 0` (empty tree), `currentEpoch = 0` (first live head), `nextNodeId = 1`
  (nodeId 0 is about to be allocated), `touchedFilesPtr = 0`, `epochMapperPtr = 0`, and all other fields zero.
- Allocate a DirNode via the pool allocator. Assign it nodeId 0 (the root).
  Set `type = 0x01` (DirNode), `headPtr = 0` (no children yet). Write it
  to its pool slot.
- Update `superblock.rootNodeOffset` to the VirtualPtr of the root DirNode.
- On subsequent mounts, read `rootNodeOffset` from the superblock and resolve
  it to load the root DirNode. The root always has nodeId 0.
- During mount, also walk the epoch mapper chain from `epochMapperPtr` in
  the superblock and build the in-memory mapper dictionary.

**Acceptance:**
  - After bootstrap, `vfs_open` returns a handle where the root directory
    exists and has nodeId 0.
  - `nextNodeId` in the superblock is 1 after bootstrap (0 was used for root).
  - Reopening the file and reading the root DirNode returns the same nodeId
    and headPtr.

---

## Workload 5.2 — File Create and Delete

**What:** Create a new file under a parent directory at a given epoch. Delete
a file by prepending a tombstone entry.

**Why:** Files are the primary data containers. Creation allocates a FileNode
and links it into the parent's directory listing. Deletion is represented
as a special DirContent entry with no name, preserving history for snapshots.

**How:**
- `vfs_create(parent_nodeId, name, epoch)`:
  1. Validate that `parent_nodeId` refers to a DirNode (type 0x01).
  2. Increment `nextNodeId` atomically in the superblock to get a new nodeId.
  3. Allocate a FileNode pool slot. Set `type = 0x03`, `nodeId = newId`,
     `headPtr = 0`, `sizePtr = 0`, `createdAt = current Unix timestamp`.
  4. Allocate a NameEntry chain for `name` (one or more pool slots).
  5. Allocate a DirContent slot. Set `childNodeId = newId`, `epoch = epoch`,
     `childPtr = VirtualPtr to FileNode`, `namePtr = VirtualPtr to NameEntry`,
     `nextPtr = current headPtr of parent`.
  6. CAS the parent's `headPtr` from the old value to the new DirContent's
     VirtualPtr. If CAS fails (another thread created a file concurrently),
     retry.
  7. Return the new file's nodeId.
- `vfs_delete(parent_nodeId, name, epoch)`:
  1. Walk the parent's `headPtr` chain at the given epoch to find the
     DirContent entry for `name`. If not found, return `VFS_ERR_NOTFOUND`.
  2. Allocate a new DirContent with the same `childNodeId` and `childPtr`,
     `epoch = epoch`, `namePtr = 0` (tombstone), `nextPtr = current headPtr`.
  3. CAS the parent's `headPtr`. The old file data is preserved — only the
     directory entry is tombstoned. The file can still be read at earlier
     epochs via snapshot isolation.
  4. Return success.

**Acceptance:**
  - Create file "test.txt" under root at epoch 0: `vfs_open_file` at epoch 0
    returns the file's nodeId.
  - Delete "test.txt" at epoch 2: listing root at epoch 2 excludes it. Listing
    root at epoch 0 still includes it (snapshot isolation).
  - Create a file that already exists at the same epoch: returns
    `VFS_ERR_EXISTS` (a second DirContent for the same name at the same epoch
    should not be created for a simple create — the existing entry is reused
    if the caller wants to overwrite, but that's a separate concern).

---

## Workload 5.3 — File Write

**What:** Write data to a file at a given offset and epoch. Handles COW on
first write to a page in an epoch, and in-place writes on subsequent writes
to the same page in the same epoch.

**Why:** This is the primary data mutation path. It must correctly handle
version chain prepends, segment growth, and file size updates, all while
being crash-safe via the lazy mirror mechanism.

**How:**
- `vfs_write(file_nodeId, offset, data, count, epoch)`:
  1. Validate that the file node exists and `epoch` is writable (live head
     or active snapshot per §7.1 of the main spec).
  2. Compute the logical page range `[offset / page_size, (offset + count -
     1) / page_size]`.
  3. For each logical page P in the range:
     a. Resolve to the PageNode. Compute `segmentIdx = P / 1024`. Walk the
        FileContent chain to find the segment. If the segment doesn't exist
        yet (file growth), allocate a new FileContent and PageNodes for all
        1,024 pages in the segment (unwritten pages have `versionRootPtr = 0`).
     b. Walk the PageNode chain within the segment to find entry P. Build
        the in-memory VirtualPtr array on first access to this segment.
     c. Walk the version chain from `versionRootPtr`. Search for an existing
        VersionPage with `epoch == writeEpoch`.
        - FOUND: in-place write. Call `Write(dataPage, newPayload)` via the
          StorageBackend (lazy mirror handles crash safety).
        - NOT FOUND (or `versionRootPtr == 0`): COW. Resolve the base
          physical page from the highest even epoch < writeEpoch in the chain.
          Allocate a new data page via `Allocate(1)`. Read the base page,
          copy it, overlay the new data at the appropriate intra-page offset.
          Call `Write(newDataPage, newPayload)`. Allocate a VersionPage pool
          slot. CAS `versionRootPtr` to the new VirtualPtr.
     d. If this is the first write to this file in this epoch, CAS-prepend
        a TouchedFile entry (Phase 4) for commit conflict tracking.
  4. Compute `newFileSize = max(oldFileSize, offset + count)`. If larger
     than the current size: allocate a FileSize pool slot and CAS-prepend
     to the FileNode's `sizePtr`.
  5. Return the number of bytes written.
- Invalidate the in-memory page array if a new FileContent was added (file
  growth beyond the current segment range).

**Acceptance:**
  - Write 100 bytes at offset 0 to a new file: read back returns the same
    100 bytes.
  - Write 200 bytes at offset 50: the file now spans both pages. Read back
    from offset 0 returns the full 250 bytes.
  - Write to the same offset in the same epoch: second write is in-place
    (no new VersionPage).
  - Write to the same offset in a new epoch (after snapshot): new VersionPage
    is created. Read at the old epoch returns old data. Read at the new epoch
    returns new data.
  - Write 2,000 pages (triggering a second FileContent segment): the in-memory
    array is rebuilt. Reads across segment boundaries return correct data.

---

## Workload 5.4 — File Read

**What:** Read data from a file at a given offset and epoch. Uses the
in-memory page array for O(1) page lookup after the first access.

**Why:** Read performance is critical — SQLite table scans, file serving,
and directory listing all depend on fast random and sequential reads.

**How:**
- `vfs_read(file_nodeId, buf, offset, count, epoch)`:
  1. Apply the epoch mapper: `readEpoch = mapper.resolve(epoch)`.
  2. Compute the logical page range as in Workload 5.3.
  3. For each logical page P in the range:
     a. Resolve to the PageNode via the FileContent chain → in-memory
        VirtualPtr array (or chain walk on first access).
     b. Walk the version chain from `versionRootPtr` using the read rule:
        - If entry.epoch == readEpoch: use it (exact match).
        - If entry.epoch > readEpoch: skip.
        - If entry.epoch < readEpoch AND even: use it (committed base). Stop.
        - If entry.epoch < readEpoch AND odd: skip.
        - Apply `traversalApply` remapping if the entry's epoch has a mapper
          entry with `traversalApply = true` (treat as mapped epoch).
     c. From the resolved VersionPage, read `dataPage`. Call `Read(dataPage)`
        via the StorageBackend. Copy the requested bytes into the output buffer.
  4. If a page has no matching VersionPage (never written at this epoch),
     return zero-filled bytes for that range.
  5. Return the number of bytes read (may be less than `count` if at EOF).

**Acceptance:**
  - Read at an epoch before any data was written: returns zero-filled bytes.
  - Read at an epoch with data: returns the correct payload.
  - Read across a page boundary: correct data from both pages.
  - Read from a committed snapshot (after commit, before GC): returns the
    committed data.
  - Read from a soft-deleted snapshot: returns the pre-snapshot base data.

---

## Workload 5.5 — Directory Operations

**What:** Create and remove directories, list directory contents. Includes
a dentry cache for fast repeated listings.

**Why:** Directories organize the namespace. Listings must correctly handle
renames across epochs and deduplicate entries by childNodeId.

**How:**
- `vfs_mkdir(parent_nodeId, name, epoch)`: same flow as file create, but
  allocates a DirNode (type 0x01) instead of a FileNode.
- `vfs_rmdir(parent_nodeId, name, epoch)`:
  1. Walk the parent's DirContent chain at the given epoch to find the child.
     Verify the child is a DirNode and has no children (walk its `headPtr`
     chain — if any DirContent entry at this epoch has `namePtr != 0`, the
     directory is not empty). If not empty, return `VFS_ERR_NOTEMPTY`.
  2. Create a tombstone DirContent (namePtr=0). CAS-prepend to parent's
     `headPtr`.
- `vfs_readdir(dir_nodeId, entries, max_entries, epoch)`:
  1. Walk the DirNode's `headPtr` chain. Collect all entries where `namePtr
     != 0` (not tombstoned).
  2. For each unique `childNodeId`, keep only the entry with the highest
     `epoch ≤ readEpoch`. This correctly handles renames and deletions across
     epochs.
  3. For each surviving entry, walk the NameEntry chain from `namePtr` to
     reconstruct the name. Determine whether the child is a directory by
     reading the child's node type from its pool slot.
  4. Fill `entries[]` with `{nodeId, name, isDir}` up to `max_entries`.
  5. Return the number of entries written.
- Dentry cache: on first listing of a directory, build an in-memory hash
  table mapping `childNodeId → {name, childPtr, isDir}`. Subsequent listings
  use the cache, avoiding pool chain traversal. The cache is invalidated when
  a new DirContent is prepended to that directory's `headPtr`.

**Acceptance:**
  - Create directory "docs", create file "readme.txt" inside it: listing
    "docs" returns one entry.
  - Delete "readme.txt": listing at the current epoch returns zero entries.
  - Listing "docs" at an epoch before the deletion returns the file.
  - Rename "readme.txt" to "readme2.txt" in the same directory at the same
    epoch: listing shows only "readme2.txt" with the same nodeId.
  - Rename at a new epoch: listing at the old epoch shows the old name;
    listing at the new epoch shows the new name.
  - `vfs_rmdir` on a non-empty directory returns `VFS_ERR_NOTEMPTY`.

---

## Workload 5.6 — File Stat

**What:** Query file metadata: size, modification time, creation time.

**Why:** Standard filesystem metadata operations needed by any VFS consumer.

**How:**
- `vfs_file_size(file_nodeId, epoch)`: walk `FileNode.sizePtr` chain via read
  rule. Return `fileSize` from the first matching FileSize entry. If chain
  is empty, return 0.
- `vfs_file_mtime(file_nodeId, epoch)`: same chain walk, return `modifiedAt`.
- `vfs_file_ctime(file_nodeId)`: read `FileNode.createdAt` directly from the
  FileNode pool slot. This is immutable — no epoch needed.
- All functions require that `file_nodeId` refers to a valid FileNode.

**Acceptance:**
  - Newly created file: size=0, ctime is set, mtime returns the epoch-0
    FileSize timestamp if any write has occurred.
  - After writing 500 bytes: size=500.
  - After writing at a new epoch: size at old epoch still returns old size.
  - `ctime` is unchanged regardless of epoch.

---

## Deliverables

| File | Purpose |
|------|---------|
| `src/tree.c` | Bootstrap, create, read, write, delete, mkdir, rmdir, readdir, rename, stat |
| `src/dentry_cache.c` | Directory entry cache with invalidation |
| `src/page_array.c` | In-memory VirtualPtr array for segment-based O(1) page lookup |
| `test/test_tree.c` | CRUD operations, multi-epoch isolation, directory listing, rename |

## Success Criteria
- Create file, write data, read back: correct payload at all offsets.
- Write across epoch boundaries: snapshot isolation preserves old data.
- Directory listing correctly handles create, delete, rename across epochs.
- In-memory page array provides O(1) lookup after first access (measurable:
  1,000 sequential reads on a 100-page file should take roughly the same time
  as 1,000 reads on a 1-page file after warm-up).
- Dentry cache eliminates repeated pool chain walks on repeated listings.
