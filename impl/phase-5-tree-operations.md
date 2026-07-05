# Phase 5: Tree Operations

## Goal
Implement all filesystem operations: create, read, write, delete, rename,
directory listing, and file stat. This includes building in-memory page
arrays for O(1) lookup, the dentry cache for directory listings, and the
read rule with epoch mapping for snapshot isolation.

## Non-Negotiable Constraints

- **All API functions must validate inputs first.** Check that nodeIds refer
  to the correct type before operating. Return `VFS_ERR_NOTDIR` if caller
  tries to list a file. Return `VFS_ERR_NOTFOUND` if a name doesn't exist.
- **No disk I/O on the hot path after warm-up.** In-memory arrays and dentry
  cache must eliminate repeated chain walks.
- **Every mutation must be crash-safe.** This means: allocate new pool slots
  before CAS-linking them into chains. Never mutate a live slot in-place
  except for `namePtr` on same-epoch rename (8-byte atomic store, documented
  risk).
- **Epoch validation on every write.** Call the Valid Epochs check from
  Phase 6 Workload 6.1 before any mutation.

## File Organization

| File | Purpose |
|------|---------|
| `src/tree.c` | Bootstrap, CRUD, directory ops, rename, stat |
| `src/dentry_cache.c` | Directory entry cache with invalidation |
| `src/page_array.c` | In-memory VirtualPtr array for segment-based O(1) lookup |
| `test/test_tree.c` | All functional tests |

## Dependencies
- Phase 2 (StorageBackend) for `storage_read`/`storage_write`/`storage_allocate`/`storage_acquire`
- Phase 3 (Pool) for `pool_alloc`/`pool_resolve` and VirtualPtr
- Phase 4 (Node Types) for all `nodes_write_*`/`nodes_read_*` helpers
- Phase 6 (Epoch System) for `mapper_resolve` and `vfs_epoch_is_writable` —
  **stub these now.** Create `src/epoch.c` with:
  ```c
  int64_t mapper_resolve(void* mapper, int64_t epoch) { return epoch; }
  bool vfs_epoch_is_writable(void* sb, int64_t epoch, void* mapper) { return true; }
  ```
  Replace with real implementations in Phase 6.

## Staging Guidance

Phase 5 has the most complex inter-workload dependencies of any phase. Build
in this order to avoid getting stuck:

### Stage A — Bootstrap (self-contained)
- 5.1: Superblock bootstrap + root directory. This gives you a writeable tree
  with a root node. Test: create file, open/close file.

### Stage B — Core operations (depends on A)
- 5.2: File Create + Delete. Uses pool, nodes, superblock.
- 5.6: File Stat. Simple chain walks on sizePtr. Good for validating chains.
- 5.5: Directory Operations. Read, write, list. Depends on 5.2 for create.

### Stage C — I/O operations (depends on B, needs stub from Phase 6)
- 5.3 first: `tree_resolve_page` + in-memory page array. This is the shared
  utility used by both read and write. Build and test it standalone first.
- 5.3: File Write (COW + in-place).
- 5.4: File Read (uses tree_resolve_page + in-memory array from 5.3).

### Shared Utilities
`tree_resolve_page(file, logical_page, epoch) → PageNode*` is the critical
shared function. Build it as a separate internal function before 5.3 and 5.4.
It:
1. Walks FileContent chain to find the segment
2. Creates missing FileContent + PageNodes on file growth
3. Builds the in-memory VirtualPtr array on first access to a segment
4. Returns a pointer to the PageNode's pool slot for the given page

### Stub Functions Needed
Create `src/epoch.c` with these stubs before starting Stage C:

```c
/* Replace with real implementation in Phase 6 */
int64_t mapper_resolve(void* mapper, int64_t epoch) {
    (void)mapper;
    return epoch;  // identity — no mapping
}

bool vfs_epoch_is_writable(void* sb, int64_t epoch, void* mapper) {
    (void)sb; (void)mapper;
    return true;  // all epochs writable — Phase 6 adds restrictions
}

/* TouchedFile — no-op stub */
void touchedfile_add(void* vfs, int64_t epoch, uint32_t nodeId) {
    (void)vfs; (void)epoch; (void)nodeId;
}
```

## Phase 4 Debt Check

| Item | Status |
|------|--------|
| All 10 node types with correct field offsets | ✅ |
| NameEntry multi-slot chaining | ✅ |
| Zero-length name → VFS_VPTR_NULL sentinel | ✅ |
| nodeId assignment is caller's responsibility | ✅ |
| No blocking debt | ✅ |

## Gaps Noted

| # | Gap | Severity | Resolution |
|---|-----|----------|------------|
| 1 | No staging guidance or build order | Medium | Added above |
| 2 | `tree_resolve_page` not called out as shared utility | Medium | Added as "Shared Utilities" section |
| 3 | No `tree_init` entry point | Low | Bootstrap in 5.1 is the implicit init |
| 4 | Superblock persistence protocol not described | Low | `storage_write(..., FLUSH_PRIO_SUPERBLOCK)` — implied but should be explicit |
| 5 | No dentry cache eviction or memory limit | Low | Acceptable for Phase 5; add in Phase 10 |
| 6 | FileContent CAS-append not specific about CAS field | Low | CAS on `FileContent.nextPtr` — documented in Phase 4 |
| 7 | `page_array` dependency on `sb->segment_size` not explicit | Low | Read from StorageBackend header at mount; store in tree context |

## Circular Dependencies

| Workload | Depends On | Stub Needed |
|----------|-----------|-------------|
| 5.3 Write | Phase 6 (TouchedFile, epoch validation) | `vfs_epoch_is_writable`, `touchedfile_add` |
| 5.4 Read | Phase 6 (mapper_resolve) | `mapper_resolve` |
| 5.5 Rename | Phase 6 (epoch validation) | `vfs_epoch_is_writable` |

All three resolved by the stubs above. No true circular dependency — Phase 5
can be fully built and tested with stub epoch functions that always return
"writable" and "no mapping." The stubs are replaced with real implementations
in Phase 6 without changing Phase 5 code.

---

## Workload 5.1 — Superblock Bootstrap & Root Directory

### What
Initialize the superblock on first use and create the root directory node.
This is the starting point of the VFS tree. Called once by `vfs_mount` when
the backing file is new.

### Step-by-Step

1. **Acquire superblock page.** Call `Acquire(1)` on the StorageBackend.
   Page 1 is reserved for the superblock. If this is a reopen, `Acquire`
   returns false — skip to step 5.
2. **Prepare superblock payload** in a local 8KB buffer:
   - `rootNodeOffset = 0` (no root yet)
   - `currentEpoch = 0` (first live head)
   - `epochMapperPtr = 0` (no mapper entries)
   - `poolListHead = 0` (no pool pages yet)
   - `treeLockState = 0`
   - `nextNodeId = 1` (nodeId 0 will be used for root)
   - `touchedFilesPtr = 0`
3. **Write superblock:** `storage_write(sb, 1, buffer)`. Flush priority 3.
4. **Create root DirNode:**
   - Allocate a pool slot: `root_vp = pool_alloc(pool)`.
   - Write DirNode to that slot: type=0x01, nodeId=0, headPtr=0.
   - Update superblock: `rootNodeOffset = root_vp`. Write superblock again.
5. **On reopen (file exists):**
   - Read superblock page: `payload = storage_read(sb, 1)`.
   - Read `rootNodeOffset`, `currentEpoch`, `nextNodeId`, `epochMapperPtr`,
     `poolListHead`, `touchedFilesPtr`, `treeLockState`.
   - Build in-memory mapper dictionary by walking the epoch mapper chain
     from `epochMapperPtr` (Phase 6).
   - Root is always at nodeId 0. Read it to verify.

### Error Conditions
- If `Acquire(1)` fails on a fresh file → something is wrong with StorageBackend
  bootstrap. This is a fatal error.
- If root DirNode's type is not 0x01 on reopen → file is corrupted.

### Acceptance
- [ ] After bootstrap: root exists, nodeId=0, headPtr=0
- [ ] `nextNodeId` in superblock is 1 (0 was used for root)
- [ ] Reopen file: superblock fields read back correctly, root accessible
- [ ] Reopen file: in-memory mapper dictionary populated from `epochMapperPtr` chain

---

## Workload 5.2 — File Create and Delete

### What
Create a file under a parent directory. Delete a file by prepending a
tombstone DirContent entry.

### Create: `vfs_create(vfs, parent_nodeId, name, epoch) → new_nodeId`

```
1. Validate epoch is writable (Phase 6, Workload 6.1).
2. Read parent DirNode from pool. Verify type == 0x01 (DirNode).
3. Walk parent's headPtr DirContent chain at this epoch.
   If a DirContent with matching name exists at this epoch:
       return VFS_ERR_EXISTS
4. Increment nextNodeId in superblock atomically:
   new_nodeId = vfs_atomic_add_i32(&superblock->nextNodeId, 1)
5. Allocate FileNode slot: file_vp = pool_alloc(pool).
   nodes_write_filenode(slot, 0x03, new_nodeId, 0, 0, time(NULL))
6. Allocate NameEntry chain for 'name': name_vp = pool_alloc_name(pool, name).
7. Allocate DirContent slot: dc_vp = pool_alloc(pool).
   Read current parent->headPtr via atomic_load.
   nodes_write_dircontent(slot, new_nodeId, epoch, file_vp, name_vp, headPtr)
8. Release memory barrier (vfs_mb_release).
9. CAS parent->headPtr from old value to dc_vp.
   If CAS fails: retry from step 7 (read new headPtr, update nextPtr, CAS again).
10. Return new_nodeId.
```

### Delete: `vfs_delete(vfs, parent_nodeId, name, epoch) → VFS_OK`

```
1. Validate epoch is writable.
2. Walk parent's headPtr DirContent chain. Find the entry with matching name
   at epoch ≤ query_epoch and highest such epoch.
3. If not found: return VFS_ERR_NOTFOUND.
4. Allocate DirContent slot: dc_vp = pool_alloc(pool).
   Set childNodeId = found_entry.childNodeId
   Set childPtr = found_entry.childPtr
   Set namePtr = 0  ← this is the tombstone
   Set nextPtr = current headPtr
5. Release memory barrier.
6. CAS parent->headPtr to dc_vp.
7. Return VFS_OK.
```

### Acceptance
- [ ] Create "test.txt" under root at epoch 0 → `vfs_mount` returns file's nodeId
- [ ] Create same name at same epoch → VFS_ERR_EXISTS
- [ ] Delete "test.txt" at epoch 2 → listing at epoch 2 excludes it, listing at
  epoch 0 still includes it
- [ ] Delete non-existent file → VFS_ERR_NOTFOUND
- [ ] Create file under non-directory nodeId → VFS_ERR_NOTDIR

---

## Workload 5.3 — File Write

### What
Write data to a file at a given offset and epoch. Handles COW on first write
to a page in an epoch, in-place on subsequent writes to the same page in the
same epoch. This is the most complex operation.

### Writable File Validation

Before any operations, call the Valid Epochs check:

```c
bool vfs_epoch_is_writable(Superblock* sb, int64_t epoch, Mapper* mapper) {
    if (epoch == sb->currentEpoch) return true;           // live head
    if (epoch % 2 == 1 && !mapper_has_entry(mapper, epoch)) return true; // active snapshot
    return false;
}
```

If this returns false, fail with `VFS_ERR_IO` (`write to frozen epoch`).

### Resolving a Logical Page to a PageNode

This function is called for every page touched by a read or write. On first
call for a segment, it builds the in-memory array.

```
PageNode* tree_resolve_page(FileNode* file, int64_t logical_page, int64_t epoch) {
    segment_idx = logical_page / superblock->segment_size
    page_in_segment = logical_page % superblock->segment_size

    // Walk FileContent chain to find segment
    fc_vp = file->headPtr
    for i = 0; i < segment_idx; i++:
        if fc_vp == 0: // segment doesn't exist yet → file growth
            allocate new FileContent + 1024 PageNodes (all versionRootPtr=0)
            CAS-append to FileContent chain
        fc_vp = fc->nextPtr

    // Access PageNode array
    if not in in-memory array:
        walk PageNode chain from fc->pageRootPtr
        build in-memory array of VirtualPtrs to each PageNode
    return &in_memory_array[page_in_segment]
}
```

### `vfs_write(vfs, file_nodeId, offset, data, count, epoch) → bytes_written`

```
1. Validate epoch is writable.
2. Read FileNode from pool. Verify type == 0x03.
3. Compute page range: [offset / page_size, (offset + count - 1) / page_size]
4. For each logical page P in the range:
   a. PageNode* pn = tree_resolve_page(file, P, epoch)
   b. Walk version chain from pn->versionRootPtr:
      - Search for VersionPage with epoch == write_epoch
      - FOUND → in-place write:
           data_page = vp->dataPage
           Read current page: old = storage_read(sb, data_page)
           Overlay new data into old at intra-page offset
           storage_write(sb, data_page, old)   // lazy mirror handles crash safety
      - NOT FOUND (or chain is empty) → COW:
           // Find base page: walk chain, find highest even epoch < write_epoch
           base_page = highest_even_below(chain, write_epoch)
           if base_page found:
               old = storage_read(sb, base_page->dataPage)
               copy old into new_payload
           else:
               zero_fill new_payload   // page never written before
           overlay new data into new_payload at intra-page offset
           new_data_page = Allocate(1)
           storage_write(sb, new_data_page, new_payload)
           // Create VersionPage
           vp_slot = pool_alloc(pool)
           nodes_write_versionpage(vp_slot, write_epoch, new_data_page,
                                    pn->versionRootPtr)
           // CAS-prepend to PageNode
           vp = pool_vptr(vp_slot)
           old_head = atomic_load_i64(&pn->versionRootPtr)
           do {
               nodes_update_nextptr(vp_slot, old_head)
               new_vp = pool_make_vptr(vp_slot)
           } while (CAS(&pn->versionRootPtr, old_head, new_vp) != old_head)
   c. If first write to this file in this epoch:
        CAS-prepend TouchedFile entry (Phase 6)
5. Compute new size = max(old_size, offset + count)
6. If file grew:
   a. Allocate FileSize pool slot
   b. nodes_write_filesize(slot, epoch, time(NULL), new_size, current_sizePtr)
   c. CAS-prepend to FileNode.sizePtr
7. Return count
```

### In-Memory Page Array Details

```c
typedef struct {
    int64_t* vptr_array;     // malloc'd, size = segment_size
    bool     built;
} SegmentArray;

// On first access to segment:
void segment_array_build(FileContent* fc, SegmentArray* arr) {
    int64_t vp = fc->pageRootPtr;
    for (int i = 0; i < segment_size; i++) {
        arr->vptr_array[i] = vp;  // stores VirtualPtr to PageNode slot
        PageNode pn;
        nodes_read_pagenode(pool_resolve(vp), &vp, &next); // vp = next PageNode
    }
    arr->built = true;
}
```

### Acceptance
- [ ] Write 100 bytes at offset 0 → read back 100 bytes match
- [ ] Write 200 bytes at offset 50 → read offset 0 gets 250 bytes (cross-page)
- [ ] Same offset, same epoch: second write is in-place (no new VersionPage)
- [ ] Same offset, new epoch (after snapshot): new VersionPage created, old
  epoch returns old data, new epoch returns new data
- [ ] Write 2,000 pages: second FileContent segment created, in-memory array
  rebuilt, reads across segment boundary correct
- [ ] Write to frozen epoch → VFS_ERR_IO

---

## Workload 5.4 — File Read

### What
Read data from a file at a given offset and epoch. Uses in-memory page arrays
for O(1) lookup after first access.

### `vfs_read(vfs, file_nodeId, buf, offset, count, epoch) → bytes_read`

```
1. Apply epoch mapper: read_epoch = mapper_resolve(mapper, epoch)
2. Compute page range as in Write
3. For each logical page P:
   a. PageNode* pn = tree_resolve_page(file, P, read_epoch)
   b. Walk version chain from pn->versionRootPtr:
      - Apply traversal remapping if mapper has entry for entry's epoch
      - If entry.epoch == read_epoch: use it (exact match, even or odd)
      - If entry.epoch > read_epoch: skip, continue
      - If entry.epoch < read_epoch AND even: use it, stop
      - If entry.epoch < read_epoch AND odd: skip, continue
   c. If a VersionPage was found:
        data = storage_read(sb, vp->dataPage)
        copy intra-page portion into output buffer
   d. If no VersionPage found:
        zero-fill that portion of output buffer (page never written at this epoch)
4. Return total bytes copied (may be < count at EOF)
```

### Acceptance
- [ ] Read before any write → returns zero-filled bytes
- [ ] Read after write at same epoch → returns written data
- [ ] Read at older epoch → returns data as of that epoch
- [ ] Read across page boundary → correct data from both pages
- [ ] Read from committed snapshot (after commit, before GC) → returns committed data
- [ ] Read from soft-deleted snapshot → returns pre-snapshot base

---

## Workload 5.5 — Directory Operations

### What
Create and remove directories, list contents. Includes a dentry cache.

### `vfs_mkdir(vfs, parent_nodeId, name, epoch) → VFS_OK`

Same flow as file create, but allocates DirNode (type 0x01) instead of FileNode.

### `vfs_rmdir(vfs, parent_nodeId, name, epoch) → VFS_OK`

```
1. Walk parent's DirContent chain to find the child. Verify child is DirNode.
2. Check if directory is empty: walk child's headPtr chain. If any DirContent
   entry at this epoch has namePtr != 0 → return VFS_ERR_NOTEMPTY.
3. Create tombstone DirContent (namePtr=0) and CAS-prepend to parent's headPtr.
```

### `vfs_readdir(vfs, dir_nodeId, entries, max, epoch) → count`

```
1. Walk DirNode's headPtr chain, collecting all entries where namePtr != 0.
2. Deduplicate by childNodeId: for each child, keep only the entry with the
   highest epoch ≤ query_epoch.
3. For each surviving entry:
   - Resolve name from NameEntry chain starting at namePtr
   - Determine isDir by reading child's pool slot (check type field)
   - Write {nodeId=childNodeId, name, isDir} into entries[] array
4. Return number of entries written (up to max).
```

### Dentry Cache

```c
typedef struct {
    // key: childNodeId, value: {name, childPtr, isDir}
    // Simple hash table, invalidated when directory's headPtr changes
    bool     valid;
    uint32_t last_headPtr_page;  // page part of headPtr when cache was built
} DentryCache;
```

On first readdir: build cache. On subsequent readdir: if cache.valid AND
directory's headPtr hasn't changed (same page as when cache was built),
return cached entries. On ANY create/delete/rename in this directory:
set cache.valid = false.

### `vfs_rename(vfs, src_parent, src_name, dst_parent, dst_name, epoch)`

```
1. Validate epoch is writable.
2. Find source DirContent entry (as in readdir dedup).
3. If same directory AND same epoch:
     Allocate new NameEntry for dst_name
     Atomic store namePtr = new_name_vp on existing DirContent (release semantics)
4. Else (cross-dir or different epoch):
     Allocate new DirContent at dst_parent with:
       childNodeId = src_entry.childNodeId
       childPtr = src_entry.childPtr
       namePtr = new NameEntry for dst_name
       nextPtr = dst_parent headPtr
     CAS-prepend to dst_parent headPtr
     Create tombstone DirContent (namePtr=0, same childNodeId/childPtr) at src_parent
     CAS-prepend to src_parent headPtr
```

### Acceptance
- [ ] mkdir "a", mkdir "a/b", create "a/b/c.txt" → readdir "a/b" returns 1 entry
- [ ] rmdir on non-empty directory → VFS_ERR_NOTEMPTY
- [ ] Rename same-dir same-epoch: old name gone, new name appears, nodeId unchanged
- [ ] Rename cross-dir: source loses entry, destination gains it
- [ ] Dentry cache: second readdir on unchanged directory uses cache (no pool chain walks)
- [ ] Dentry cache: after create in directory, cache invalidated, next readdir rebuilds

---

## Workload 5.6 — File Stat

### What
Query file metadata: size, modification time, creation time.

### `vfs_file_size(vfs, file_nodeId, epoch) → size`

```
1. Read FileNode. Walk sizePtr chain.
2. Apply read rule: first FileSize entry with epoch ≤ query_epoch (mapped) AND
   entry.epoch is even (or exact match) → use its fileSize.
3. If chain is empty: return 0.
```

### `vfs_file_mtime(vfs, file_nodeId, epoch) → timestamp`
Same chain walk as file_size, returns `modifiedAt`.

### `vfs_file_ctime(vfs, file_nodeId) → timestamp`
Read `createdAt` directly from FileNode pool slot. Immutable — no epoch needed.

### Acceptance
- [ ] New file: size=0, ctime set, mtime returns time of first FileSize entry
- [ ] After writing 500 bytes: size=500
- [ ] Write at new epoch: old epoch still returns old size
- [ ] ctime unchanged across epochs

---

## Final Phase 5 Checklist

- [ ] Create file → write → read → delete: full CRUD cycle passes
- [ ] Cross-epoch isolation: writes at snapshot epoch don't affect live-head reads
- [ ] In-memory page array: 1,000 reads on 100-page file after warm-up takes
  roughly same time as 1,000 reads on 1-page file
- [ ] Dentry cache avoids repeated pool chain walks
- [ ] Rename preserves nodeId, correctly handles same-epoch and cross-epoch cases
- [ ] All operations validate inputs (wrong nodeId type → appropriate error)
- [ ] All writes check epoch validity before mutating
