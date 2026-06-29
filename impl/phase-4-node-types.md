# Phase 4: Node Types

## Goal
Define all 32-byte pool entry structures with serialization/deserialization.

## Workloads

### 4.1 DirNode / FileNode (16 bytes payload + 16 reserved)
- type(2), rsvd(2), nodeId(4), headPtr(8)
- FileNode adds: sizePtr(8), createdAt(8) — fits in 32 bytes
- Global `nextNodeId` counter in superblock, atomic increment
- Write: `vfs_wr2/4/8` helpers into 32-byte slot buffer
- Read: `vfs_rd2/4/8` from slot buffer

### 4.2 DirContent (32 bytes, fully packed)
- childNodeId(4), epoch(4), childPtr(8), namePtr(8), nextPtr(8)
- namePtr = 0 → deleted in this epoch
- CAS-prepend to DirNode.headPtr chain

### 4.3 FileContent (16 payload + 16 reserved)
- pageRootPtr(8), nextPtr(8)
- NOT epoch-keyed — permanent segment chain
- nextPtr links to next FileContent (higher page range)

### 4.4 PageNode (16 payload + 16 reserved)
- versionRootPtr(8), nextPtr(8)
- One per logical page within a segment
- Chain head CAS'd on first write in epoch

### 4.5 VersionPage (20 payload + 12 reserved)
- epoch(4), rsvd(4), dataPage(8), nextPtr(8)
- CAS-prepend to PageNode.versionRootPtr
- Descending epoch order

### 4.6 FileSize (20 payload + 12 reserved)
- epoch(4), modifiedAt(8), fileSize(8), nextPtr(8)
- Chain at FileNode.sizePtr
- Read rule resolves current file size

### 4.7 NameEntry (24 data + 8 next)
- data[24], nextPtr(8)
- Chained for names > 24 bytes
- Zero-padded UTF-8

### 4.8 TouchedFile (16 payload + 16 reserved)
- epoch(4), nodeId(4), nextPtr(8)
- Per-epoch chain at superblock.touchedFilesPtr
- CAS-prepend on first VersionPage write in epoch per file

### 4.9 MapperEntry (16 payload + 16 reserved)
- fromEpoch(4), toEpoch(4), flags(2), rsvd(6), nextPtr(8)
- Chain at superblock.epochMapperPtr

### 4.10 Tests
- Allocate each type, serialize, read back, verify fields
- Chain walking: prepend 3 entries, walk, verify descending order

## Deliverables
- `src/nodes.c`, `src/nodes.h` with typed structs and accessor functions
- `test/test_nodes.c`
