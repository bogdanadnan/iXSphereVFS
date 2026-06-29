# iXSphereVFS Implementation Plan

Constraints: C11, cross-platform (Linux/macOS/Windows), no external dependencies.
Assembly acceptable for CRC32C and atomic ops where intrinsics are insufficient.
All code optimized for throughput — hot paths avoid allocations, use inline functions, and minimize cache misses.

## Phases

| Phase | Name | Deliverable |
|-------|------|-------------|
| 1 | Platform & Primitives | CRC32C, atomics, memory barriers, page buffer helpers |
| 2 | StorageBackend | Page allocation, bitmap, lazy mirror, file I/O, cache, flush |
| 3 | Pool Allocator | 32-byte fixed-slot pool, VirtualPtr, CAS allocation, arena optimization |
| 4 | Node Types | DirNode, FileNode, DirContent, FileContent, PageNode, VersionPage, FileSize, NameEntry, TouchedFile |
| 5 | Tree Operations | Create, read, write, delete, rename, directory listing, in-memory arrays |
| 6 | Epoch System | Snapshot, commit, conflict detection, soft-delete, epoch mapper |
| 7 | Garbage Collection | Shadow compaction, deferred-free queue |
| 8 | Filesystem API | vfs_open/close/read/write/mkdir/readdir/lock/snapshot/commit/gc |
| 9 | SQLite VFS | CowVfs integration layer |
| 10 | Optimization | Profiling, cache tuning, lock contention, allocation hot paths |

Each phase has its own spec in `impl/phase-N-name.md` with detailed workloads.
