# Phase 1: Platform & Primitives

## Goal
Build the cross-platform foundation layer. Every subsequent phase depends on
this code. Nothing can be deferred or left "for later."

## Non-Negotiable Constraints

- **C11 only.** No C++, no GNU extensions, no compiler-specific features
  except where hidden behind `#ifdef` in `platform.h`. The code must compile
  with `-std=c11 -pedantic-errors`.
- **No external dependencies.** Only libc and compiler intrinsics. No
  third-party libraries, no package managers, no `dlopen`.
- **All platforms must pass.** The CI must be green on Linux x86_64, Linux
  aarch64, macOS x86_64, macOS aarch64, and Windows x86_64 before Phase 1
  is considered complete.
- **Zero warnings** at `-Wall -Wextra -Wpedantic` on GCC and Clang, `/W4` on
  MSVC.
- **Header-only where possible.** Atomics, page buffer helpers, and platform
  macros must be inline. Only `crc32c.c` and `error.c` need `.c` files.
- **No allocations on the hot path.** `malloc`/`free` are acceptable in the
  test harness only. Production code uses stack buffers or caller-provided
  memory.

---

## Workload 1.1 — Project Skeleton

### What
Create the CMake project, directory structure, public headers, and a test
harness that compiles and runs an empty test suite.

### Exact Deliverables

| File | Contents |
|------|----------|
| `CMakeLists.txt` | Project root, see below |
| `include/ixsphere_vfs.h` | Public header stub |
| `include/vfs_internal.h` | Internal header stub |
| `src/` | (empty directory — source files added in later workloads) |
| `test/test_runner.c` | Test harness with `main()` and `CHECK` macro |

### CMakeLists.txt Requirements
```cmake
cmake_minimum_required(VERSION 3.16)
project(iXSphereVFS VERSION 0.1.0 LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "-Wall -Wextra -Wpedantic")

add_library(ixsphere_vfs STATIC src/vfs.c)   # vfs.c will be added in a later step; this can be an empty file for now
target_include_directories(ixsphere_vfs PUBLIC include)

enable_testing()
add_executable(vfs_test test/test_runner.c)
target_link_libraries(vfs_test ixsphere_vfs pthread)
add_test(NAME vfs_test COMMAND vfs_test)
```

### ixsphere_vfs.h Stub
Must contain:
- Include guard `IXSPHERE_VFS_H`
- `#include <stdint.h>`, `<stddef.h>`, `<stdbool.h>`
- `extern "C"` block for C++ compatibility (this is a C library but may be
  called from C++)
- The `vfs_error_t` enum with all 9 codes (Workload 1.6)
- `const char* vfs_error_string(vfs_error_t err);`
- Opaque typedef: `typedef struct vfs_t vfs_t;`
- Stub declarations for `vfs_open` and `vfs_close` (both return `NULL`/void
  for now — actual implementation in Phase 5)

### vfs_internal.h Stub
Must contain:
- Include guard `VFS_INTERNAL_H`
- `#include "ixsphere_vfs.h"`
- `#define VFS_PAGE_SIZE 8192`
- Placeholder struct `vfs_t { int _placeholder; };`

### test_runner.c
Must contain:
- `#include "ixsphere_vfs.h"` and `<stdio.h>`
- `static int tests_run = 0;` and `static int tests_passed = 0;`
- `CHECK(expr)` macro that increments `tests_run`, checks the expression,
  prints failures with file and line to stderr
- `CHECK_EQ(a, b)` and `CHECK_STREQ(a, b)` convenience macros
- `main()` that prints a header, runs tests by calling `test_*()` functions,
  prints pass/fail summary, returns 0 on all-pass or 1 on failure
- The test functions will be added in later workloads; for now `main()` can
  print "No tests yet" and return 0

### Acceptance (Checklist)
- [ ] `mkdir build && cd build && cmake .. && cmake --build .` succeeds
- [ ] `ctest` passes (empty or simple test)
- [ ] Zero compiler warnings on at least one platform
- [ ] `include/ixsphere_vfs.h` compiles when included from a C++ file

---

## Workload 1.2 — Platform Detection

### What
A single header `src/platform.h` that every other source file includes. It
detects the compiler, architecture, and OS, and defines macros used everywhere.

### Exact Deliverable
One file: `src/platform.h`.

### Required Macros

| Macro | Purpose | Values |
|-------|---------|--------|
| `VFS_COMPILER_GCC` | Defined to 1 if GCC or Clang | 1 or undefined |
| `VFS_COMPILER_MSVC` | Defined to 1 if MSVC | 1 or undefined |
| `VFS_ARCH_X86_64` | x86-64 | 1 or undefined |
| `VFS_ARCH_AARCH64` | ARM 64-bit | 1 or undefined |
| `VFS_OS_LINUX` | Linux | 1 or undefined |
| `VFS_OS_MACOS` | macOS | 1 or undefined |
| `VFS_OS_WINDOWS` | Windows | 1 or undefined |
| `VFS_INLINE` | Force-inline | `static inline` |
| `VFS_LIKELY(x)` | Branch prediction hint | `__builtin_expect(!!(x), 1)` on GCC |
| `VFS_UNLIKELY(x)` | Branch prediction hint | `__builtin_expect(!!(x), 0)` on GCC |
| `VFS_RESTRICT` | Aliasing hint | `__restrict__` on GCC, `__restrict` on MSVC |
| `VFS_CACHELINE` | Cache line size | 64 on x86_64, 128 on Apple Silicon, 64 on Linux aarch64 |
| `VFS_PAGE_SIZE` | Default page size | 8192 |

### How to Implement
- Use predefined compiler macros:
  - `__GNUC__` or `__clang__` → `VFS_COMPILER_GCC`
  - `_MSC_VER` → `VFS_COMPILER_MSVC`
- Architecture:
  - `__x86_64__` or `_M_X64` → `VFS_ARCH_X86_64`
  - `__aarch64__` or `_M_ARM64` → `VFS_ARCH_AARCH64`
- Apple Silicon detection: `VFS_ARCH_AARCH64 && __APPLE__` → `VFS_CACHELINE = 128`
- `#error` if compiler or architecture is unsupported
- Include `<stdint.h>`, `<stddef.h>`, `<string.h>` at the top — every file
  that includes `platform.h` gets these for free

### Acceptance
- [ ] `VFS_PAGE_SIZE == 8192` at compile time
- [ ] `VFS_CACHELINE` is a power of 2 and ≥ 64
- [ ] `VFS_INLINE` function compiles and is actually inlined (check disassembly
  at -O2 or higher)
- [ ] `#error` fires on an unsupported compiler (test by temporarily adding
  an unknown compiler guard)

---

## Workload 1.3 — CRC32C

### What
A single function `vfs_crc32c` that computes CRC-32C (Castagnoli) of any byte
buffer. Hardware-accelerated on x86_64 and aarch64, software fallback
elsewhere. This runs on every page read — it must be fast.

### Exact Deliverable
One file: `src/crc32c.c`. Declaration added to `include/ixsphere_vfs.h`:
```c
uint32_t vfs_crc32c(const uint8_t* data, size_t len);
```

### Required Behavior
- **Empty input:** `vfs_crc32c(NULL, 0)` returns `0x00000000`
- **Known vector:** `vfs_crc32c("123456789", 9)` returns `0xE3069283`
- **Deterministic:** same input always produces same output
- **Unaligned-safe:** works on any pointer, not just aligned addresses
- **8KB page:** the CRC32C of an 8,192-byte buffer of zeros must be identical
  every call
- **Hardware path:** if SSE4.2 (x86_64) or ARMv8 CRC32 (aarch64) is available,
  use it. No runtime dispatch — compile-time `#if` is sufficient

### Software Fallback Implementation
- 256-entry `uint32_t` lookup table
- Polynomial: `0x82F63B78` (reversed Castagnoli)
- Initialize table once with a static flag (`static int table_ready = 0`)
- Algorithm for each byte: `crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8)`
- Start with `crc = 0xFFFFFFFF`, finalize with `crc ^ 0xFFFFFFFF`

### x86_64 Hardware Path
- Include `<nmmintrin.h>` (SSE4.2)
- Use `_mm_crc32_u64` for 8-byte chunks, `_mm_crc32_u32` for 4-byte chunks,
  `_mm_crc32_u8` for remaining bytes
- Load unaligned chunks with `memcpy` into a local variable

### aarch64 Hardware Path
- Include `<arm_acle.h>`
- Use `__crc32cd` (8 bytes), `__crc32cw` (4 bytes), `__crc32cb` (1 byte)

### Acceptance
- [ ] `vfs_crc32c("123456789", 9) == 0xE3069283` on all platforms
- [ ] `vfs_crc32c(zero_page, 8192)` is deterministic on repeated calls
- [ ] `vfs_crc32c(ones_page, 8192)` ≠ `vfs_crc32c(zero_page, 8192)`
- [ ] 64KB of ascending bytes (0, 1, 2, ... 65535) produces repeatable output
- [ ] Unaligned input works: `vfs_crc32c(buf + 1, 8)` where `buf` is aligned

---

## Workload 1.4 — Atomics & Memory Ordering

### What
Inline functions for atomic operations with acquire/release semantics. Used
by every lock-free data structure in the VFS.

### Exact Deliverable
Add to `src/platform.h` (header-only inline functions). No `.c` file.

### Required Functions

For `int64_t`:
```c
int64_t vfs_atomic_load_i64(const int64_t* ptr);        // acquire
void    vfs_atomic_store_i64(int64_t* ptr, int64_t val); // release
int64_t vfs_atomic_add_i64(int64_t* ptr, int64_t delta); // returns new value
int64_t vfs_cas_i64(int64_t* ptr, int64_t expected, int64_t desired); // returns old value
```

Same four for `int32_t` (suffix `_i32`).
Same four for `void*` (suffix `_ptr`).

Memory barriers:
```c
void vfs_mb_acquire(void);  // #LoadLoad + #LoadStore
void vfs_mb_release(void);  // #LoadStore + #StoreStore
void vfs_mb_full(void);     // sequential consistency
```

### CAS Semantics
```c
int64_t old = vfs_cas_i64(&counter, 42, 99);
// If counter was 42 → counter is now 99, old is 42
// If counter was 77 → counter is still 77, old is 77
```
CAS must be the "strong" variant — it will NOT spuriously fail. If the
comparison succeeds, the store is guaranteed.

### GCC/Clang Implementation
Use `__atomic_load_n(ptr, __ATOMIC_ACQUIRE)`, etc. CAS uses
`__atomic_compare_exchange_n` with `__ATOMIC_ACQ_REL` on success and
`__ATOMIC_ACQUIRE` on failure.

### MSVC Implementation
Use `InterlockedCompareExchange64`, `InterlockedExchangeAdd64`, etc.
Barriers use `_ReadWriteBarrier()` and `__faststorefence()`.

### Acceptance
- [ ] `vfs_cas_i64` with matching expected: succeeds, returns old value
- [ ] `vfs_cas_i64` with non-matching expected: fails, returns current value,
  variable unchanged
- [ ] CAS retry loop: `do { cur = load(&x); } while (cas(&x, cur, cur+1) != cur);`
  increments `x` by 1
- [ ] 4 threads, each calling `vfs_atomic_add_i64(&counter, 1)` 100,000 times:
  final counter is exactly 400,000
- [ ] `vfs_atomic_store_i64(&x, 7)` followed by `vfs_atomic_load_i64(&x)`
  returns 7
- [ ] Compiles to `lock cmpxchg` on x86_64, `ldaxr`/`stlxr` on aarch64
  (verify with `objdump -d`)

---

## Workload 1.5 — Page Buffer Helpers

### What
Inline functions for reading/writing integers at byte offsets within an
8KB buffer. Used to serialize/deserialize all on-disk structures.

### Exact Deliverable
One file: `src/page_buf.h` (header-only). Include it from `src/platform.h`
so every file gets these automatically.

### Required Functions
```c
int64_t vfs_rd8(const uint8_t* buf, int offset);  // read int64
int32_t vfs_rd4(const uint8_t* buf, int offset);  // read int32
int16_t vfs_rd2(const uint8_t* buf, int offset);  // read int16
void    vfs_wr8(uint8_t* buf, int offset, int64_t val);
void    vfs_wr4(uint8_t* buf, int offset, int32_t val);
void    vfs_wr2(uint8_t* buf, int offset, int16_t val);
void    vfs_zero_page(uint8_t* buf);               // fill 8192 bytes with 0
void    vfs_copy_page(uint8_t* dst, const uint8_t* src); // memcpy 8192 bytes
```

### Implementation Requirements
- All functions must be `VFS_INLINE`
- Each function must be a single call plus bounds check in debug mode
- `vfs_rd*`/`vfs_wr*` use `memcpy` for unaligned-safe access. The compiler
  will optimize memcpy of 8 bytes to a single `mov` on platforms that support
  unaligned access.
- `vfs_zero_page` default is `memset(buf, 0, VFS_PAGE_SIZE)`. On x86_64, also
  provide `vfs_zero_page_fast` using `_mm_setzero_si128()` with SSE2 128-bit
  stores
- `vfs_copy_page` is `memcpy(dst, src, VFS_PAGE_SIZE)`
- In debug mode, `assert(offset + sizeof(type) <= VFS_PAGE_SIZE)` for each
  read/write. In release, no bounds check — the caller is responsible.

### Acceptance
- [ ] Write int64 at offset 0, read at offset 0: same value
- [ ] Write at offset 8184 (last 8 bytes of page), read back: correct
- [ ] Write int32 at offset 100, read at offset 100: same value
- [ ] Write int16 at offset 200, read at offset 200: same value
- [ ] `vfs_zero_page` on an all-0xFF buffer: all bytes become 0
- [ ] `vfs_copy_page`: destination matches source, no overlap corruption
- [ ] `vfs_zero_page_fast` produces identical result to `memset`

---

## Workload 1.6 — Error Strings

### What
A function that converts `vfs_error_t` codes to human-readable strings.

### Exact Deliverable
One file: `src/error.c`. The enum is already in `include/ixsphere_vfs.h`.

```c
const char* vfs_error_string(vfs_error_t err) {
    switch (err) {
        case VFS_OK:            return "OK";
        case VFS_ERR_IO:        return "I/O error";
        case VFS_ERR_NOTFOUND:  return "Not found";
        case VFS_ERR_EXISTS:    return "Already exists";
        case VFS_ERR_NOTDIR:    return "Not a directory";
        case VFS_ERR_NOTEMPTY:  return "Directory not empty";
        case VFS_ERR_CONFLICT:  return "Conflict";
        case VFS_ERR_FULL:      return "No space left";
        case VFS_ERR_NOMEM:     return "Out of memory";
        default:                return "Unknown error";
    }
}
```

### Acceptance
- [ ] Every `vfs_error_t` value returns a non-NULL, null-terminated string
- [ ] Unknown/out-of-range values return "Unknown error"
- [ ] Function is reentrant (returns string literals, no static buffer)

---

## Final Phase 1 Checklist

Before moving to Phase 2, every item must be checked:

- [ ] `cmake --build .` succeeds with zero warnings on Linux x86_64
- [ ] `ctest` passes all tests
- [ ] `vfs_crc32c("123456789", 9) == 0xE3069283`
- [ ] 4-thread atomic counter reaches exactly 400,000
- [ ] Page buffer helpers round-trip all values
- [ ] `vfs_error_string(VFS_ERR_NOTFOUND)` returns `"Not found"`
- [ ] No `malloc`/`free` in production code (test harness only)
- [ ] `include/ixsphere_vfs.h` is the only public header
- [ ] `src/platform.h` is included by every `.c` file

---

## Review Iteration 1 (2026-07-01)

Findings from first implementation attempt. Fix before marking Phase 1 complete.

### Critical

**1. CRC32C missing hardware paths.**
`src/crc32c.c` implements only the software fallback (256-entry lookup table).
The spec requires hardware-accelerated paths:
- x86_64: SSE4.2 `_mm_crc32_u64` / `_mm_crc32_u32` / `_mm_crc32_u8`
- aarch64: ARMv8 CRC32 `__crc32cd` / `__crc32cw` / `__crc32cb`

Add them under `#if VFS_ARCH_X86_64` and `#elif VFS_ARCH_AARCH64` guards.
The software path stays as `#else` fallback. Expected speedup: ~100× on 8KB pages.

### Medium

**2. CRC32C table init is not thread-safe.**
`crc32c.c` uses `static int s_table_ready` to guard one-time table initialization.
Two threads calling `vfs_crc32c` concurrently race on the flag. Fix: use C11
`call_once` or initialize the table at library load time (a `__attribute__((constructor))`
function, or a static initializer that precomputes the table at compile time).

**3. Missing `vfs_zero_page_fast`.**
`page_buf.h` provides `vfs_zero_page` via `memset` but not the SSE2-accelerated
`vfs_zero_page_fast` required by the spec. Add it under `#if VFS_ARCH_X86_64` using
`_mm_setzero_si128()` with 128-bit stores in a loop. Fall back to `vfs_zero_page` on
other architectures.

### Low

**4. No debug-mode bounds checks.**
`page_buf.h` includes `<assert.h>` but none of the read/write helpers assert bounds.
Add `assert(offset + sizeof(type) <= VFS_PAGE_SIZE)` in each helper, conditionally
compiled under `#ifndef NDEBUG`.

**5. MSVC `vfs_atomic_add_ptr` has a race.**
`platform.h` lines 201-204 implement pointer add via read + manual add + exchange.
This is not atomic if another thread modifies the pointer between steps. Use a CAS
retry loop or `InterlockedExchangeAdd` (if available for pointer-sized values).

**6. `pthread_create` in test — Windows portability.**
`test/test_main.c` line 119 uses `pthread_create`. On Windows this requires
pthreads-win32 or the MSVC C11 threads library. Option: add a platform wrapper
`vfs_thread_create` in `platform.h` that maps to `pthread_create` on Unix and
`_beginthreadex` on Windows. Or use C11 `thrd_create` from `<threads.h>`.
