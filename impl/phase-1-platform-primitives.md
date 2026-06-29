# Phase 1: Platform & Primitives

## Goal
Establish cross-platform foundation: CRC32C, atomics, memory ordering, page buffer helpers.

## Workloads

### 1.1 Project Skeleton
- `CMakeLists.txt` with C11, `-O3 -march=native`, warning flags
- `src/` directory structure
- `include/ixsphere_vfs.h` public header (stub)
- `include/vfs_internal.h` internal header (stub)
- Test harness: `test/test_runner.c` with `CHECK()` macro

### 1.2 Platform Detection
- `src/platform.h`: detect compiler (GCC/Clang/MSVC), architecture (x86_64/aarch64), OS
- Define `VFS_INLINE`, `VFS_LIKELY`, `VFS_UNLIKELY`, `VFS_RESTRICT`
- Define `VFS_CACHELINE` (64 on x86, 128 on Apple Silicon)

### 1.3 CRC32C
- Software fallback: 256-entry lookup table, polynomial 0x82F63B78
- Hardware: SSE4.2 `_mm_crc32_u32`/`_mm_crc32_u64` on x86_64
- Hardware: `__crc32cd`/`__crc32cw` on aarch64
- Public API: `uint32_t vfs_crc32c(const uint8_t* data, size_t len)`
- Tests: empty, 9-byte "123456789" (0xE3069283), 8KB page, aligned/unaligned

### 1.4 Atomics & Memory Ordering
- `vfs_atomic_load(T* ptr)` — acquire load
- `vfs_atomic_store(T* ptr, T val)` — release store  
- `vfs_cas(T* ptr, T expected, T desired)` — strong CAS, returns old value
- Typed variants for `uint32_t`, `uint64_t`, pointer-sized
- Platform implementations: C11 `stdatomic.h`, GCC `__atomic`, MSVC `Interlocked*`
- Tests: concurrent increment, CAS loop, acquire/release ordering

### 1.5 Page Buffer Helpers
- `vfs_rd8(buf, offset)` / `vfs_wr8(buf, offset, val)` — int64
- `vfs_rd4(buf, offset)` / `vfs_wr4(buf, offset, val)` — int32
- `vfs_rd2(buf, offset)` / `vfs_wr2(buf, offset, val)` — int16
- `vfs_zero_page(buf)` — zero-fill 8192 bytes
- `vfs_copy_page(dst, src)` — copy 8192 bytes
- All inline, assume page buffer is at least page_size bytes

### 1.6 Error & Logging
- `vfs_error_t` enum
- `vfs_error_string()` 
- `VFS_DEBUG(fmt, ...)` / `VFS_TRACE(fmt, ...)` macros (compile-time optional)
- Default: stderr output; configurable callback

## Deliverables
- `src/crc32c.c`, `src/atomics.c` (or header-only), `src/page_buf.c`
- `test/test_crc32c.c`, `test/test_atomics.c`
- All tests pass on x86_64 and aarch64
