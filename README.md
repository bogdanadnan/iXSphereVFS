# iXSphereVFS

[![CI](https://github.com/ixsphere/ixsphere/actions/workflows/native-vfs-ci.yml/badge.svg)](https://github.com/ixsphere/ixsphere/actions/workflows/native-vfs-ci.yml)

Standalone C11 implementation of a copy-on-write, epoch-versioned, unified tree
Virtual File System (VFS).  Provides file and directory operations with
snapshot isolation, lazy mirror redundancy, shadow-compaction GC, and a
spin-locked page cache — all backed by a single sparse file.

## Quick Start

```c
#include "ixsphere/vfs.h"
#include "ixsphere/vfs_internal.h"  /* for vfs->ctx->rootNodeOffset */

int main(void) {
    /* Mount (or create) a VFS */
    vfs_t* vfs = vfs_mount("example.vfs", 8192);
    if (!vfs) return 1;

    int64_t root = vfs->ctx->rootNodeOffset;

    /* Create a file */
    int64_t fvp = vfs_create(vfs, root, "hello.txt", 0);
    if (fvp <= 0) { vfs_unmount(vfs); return 1; }

    /* Write data */
    const char* msg = "Hello, iXSphereVFS!";
    vfs_write(vfs, fvp, msg, 0, (int64_t)strlen(msg), 0);

    /* Read it back */
    char buf[128];
    int n = vfs_read(vfs, fvp, buf, 0, sizeof(buf), 0);
    printf("Read %d bytes: %.*s\n", n, n, buf);

    /* Clean up */
    vfs_unmount(vfs);
    return 0;
}
```

Compile:
```
gcc -std=c11 -Ipath/to/ixspherevfs/include -Ipath/to/ixspherevfs/src \
    -o example example.c -Lpath/to/ixspherevfs/build -lixsphere_vfs -lpthread
```

## Build

### Requirements

| Dependency | Version | Notes |
|-----------|---------|-------|
| CMake | ≥ 3.16 | Build system |
| C compiler | C11 (gcc/clang/MSVC) | C11 standard required |
| pthread | POSIX threads | Threading support (libpthread) |
| SSE4.2 / ARMv8 CRC | Hardware CRC32C | Auto-detected; software fallback available |

### Build & Test

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
ctest --output-on-failure
```

### Targets

| Target | Description |
|--------|-------------|
| `ixsphere_vfs` | Static library |
| `vfs_bench` | Multi-workload benchmark tool |
| `vfs_test` | Core VFS unit tests |
| `test_storage` | Storage backend tests |
| `test_pool` | Pool allocator tests |
| `test_nodes` | Node serialization tests |
| `test_tree` | Tree operations tests |
| `test_epoch` | Epoch/snapshot tests |
| `test_mapper` | Mapper chain tests |
| `test_gc` | Garbage collection tests |
| `test_var_array` | Variable array tests |
| `test_crash` | Crash recovery scenarios (Unix only) |
| `test_fuzz` | Deterministic fuzz test (Unix only) |

## Platform Support

| Platform | Arch | Status |
|----------|------|--------|
| Linux (x86_64) | amd64 | ✅ CI |
| Linux (aarch64) | arm64 | ✅ CI |
| macOS (x86_64) | Intel | ✅ CI |
| macOS (aarch64) | Apple Silicon | ✅ CI |
| Windows (x86_64) | amd64 | ✅ CI (no fork tests) |

## Documentation

| Doc | Content |
|-----|---------|
| [SPEC.md](SPEC.md) | Full architecture specification |
| [docs/PERFORMANCE.md](docs/PERFORMANCE.md) | Hot-path profiling, top 5 CPU consumers, SPEC §13 latency comparison |
| [docs/CACHE.md](docs/CACHE.md) | Cache tuning: size sweep, saturation, memory-constrained minimum |
| [docs/CONTENTION.md](docs/CONTENTION.md) | Lock contention: CAS retry rates, throughput scaling, DirContent bottleneck |

## Architecture

- **Copy-on-write tree**: DirNodes, FileNodes, DirContents, VersionPages — all
  allocated from a unified 32-byte slot pool, stored in a single sparse file.
- **Epoch versioning**: Even epochs = writable base, odd epochs = snapshots.
  Mapper chains provide O(read-rule) epoch resolution.
- **Lazy mirror**: Page writes alternate between two physical copies with
  generation-based selection. CRC32C validates on read.
- **Shadow-compaction GC**: Walks live tree, copies entries to fresh pool pages,
  atomically swaps superblock, frees dead pages.
- **Page cache**: Chained hash table with spin-locked buckets, LRU-clock
  eviction, priority-ordered flush (data → pool → superblock).

## License

MIT — see [LICENSE](LICENSE).
