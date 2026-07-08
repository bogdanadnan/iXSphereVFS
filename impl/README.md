# iXSphereVFS Implementation Plan

Constraints: C11, cross-platform (Linux/macOS/Windows), no external dependencies.
Assembly acceptable for CRC32C and atomic ops where intrinsics are insufficient.
All code optimized for throughput — hot paths avoid allocations, use inline functions, and minimize cache misses.

## Phases

| Phase | Name | Deliverable |
|-------|------|-------------|
| 1 | Platform & Primitives | CRC32C, atomics, memory barriers, page buffer helpers |
| 2 | StorageBackend | Page allocation, indirection table, lazy mirror, file I/O, cache, flush |
| 3 | Pool Allocator | 32-byte fixed-slot pool, VirtualPtr, CAS allocation, arena optimization |
| 4 | Node Types | DirNode, FileNode, DirContent, FileContent, PageNode, VersionPage, FileSize, NameEntry |
| 5 | Tree Operations | Create, read, write, delete, rename, directory listing, in-memory arrays |
| 6 | Epoch System | Snapshot, commit, conflict detection, soft-delete, epoch mapper |
| 7 | Garbage Collection | Shadow compaction, deferred-free queue |
| 8 | Filesystem API | vfs_mount/vfs_unmount/vfs_open/vfs_read/vfs_write/mkdir/readdir/lock/snapshot/commit/gc |
| 9 | SQLite VFS | CowVfs integration layer |
| 10 | Optimization | Profiling, cache tuning, lock contention, allocation hot paths |
| 11 | SQLite Benchmarking | Performance measurement under realistic SQLite workloads |
| 12 | FUSE Integration | Mount iXSphereVFS as a regular filesystem via FUSE |
| 13 | Runtime Structures | Mapper table cache replacing chain walks with in-memory table |
| 14 | VirtualPtr API | vfs_create/vfs_mkdir return VirtualPtr directly, remove resolve_child_vp |
| 15 | Sparse PageNodes | Lazy PageNode allocation, sorted-insert CAS chain, gc_generation cache |
| 16 | Variable Lock-Free Array | VarArray<T> generic lock-free array with lazy growth (foundation for future caches) |
| 17 | NameEntry Hash Fast-Reject | 64-bit name hash in NameEntry offset 0; fast-reject non-matches in dirchain_find_child |

Each phase has its own spec in `impl/phase-N-name.md` with detailed workloads.
