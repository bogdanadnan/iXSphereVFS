# Phase 1: Platform & Primitives

## Goal
Establish the cross-platform foundation: compiler and architecture detection,
hardware-accelerated CRC32C, atomic operations with memory ordering, fast
page-buffer serialization, and basic error/logging infrastructure.

All code must compile and run on Linux x86_64, Linux aarch64, macOS x86_64,
macOS aarch64, and Windows x86_64. No dependencies beyond libc and compiler
builtins. No allocations on the hot path.

---

## Workload 1.1 — Project Skeleton

**What:** Create the CMake build system and directory layout for the library
and test suite.

**Why:** Every subsequent phase adds source files and tests. A consistent
build structure from the start eliminates integration friction.

**How:**
- `CMakeLists.txt` at the project root. Set C11 standard, `-O3 -march=native`,
  and strict warnings (`-Wall -Wextra -Wpedantic`).
- Static library target `ixsphere_vfs` built from `src/`.
- Test executable `vfs_test` linked against the library and `pthread`.
- Directory structure: `src/` for library code, `include/` for public headers,
  `test/` for test files.
- Public header `include/ixsphere_vfs.h` starts as a stub — it will grow
  with each phase.
- Internal header `include/vfs_internal.h` starts as a placeholder.
- Test harness in `test/test_runner.c`: a `main()` that calls individual test
  functions and reports total passed vs failed. A `CHECK(expr)` macro that
  increments counters and prints failures with file and line.

**Acceptance:** `cmake .. && cmake --build . && ctest` succeeds with zero
tests (empty test suite is fine for this workload). The build compiles
without warnings on all target platforms.

---

## Workload 1.2 — Platform Detection

**What:** A single header `src/platform.h` that detects the compiler,
architecture, and operating system at compile time and defines consistent
macros used by the rest of the codebase.

**Why:** CRC32C, atomics, memory barriers, and SIMD optimizations all require
platform-specific intrinsics or builtins. Centralizing detection avoids
`#ifdef` sprawl in every source file.

**How:**
- Detect compiler: GCC, Clang, or MSVC. Define `VFS_COMPILER_GCC` or
  `VFS_COMPILER_MSVC` accordingly. Emit `#error` for unsupported compilers.
- Detect architecture: x86_64 or aarch64. Define `VFS_ARCH_X86_64` or
  `VFS_ARCH_AARCH64`. Emit `#error` for unsupported architectures.
- Define utility macros: `VFS_INLINE` (always `static inline`), `VFS_LIKELY`
  and `VFS_UNLIKELY` (branch prediction hints), `VFS_RESTRICT` (aliasing hint).
- Define `VFS_CACHELINE`: 64 on x86_64, 128 on Apple Silicon (aarch64 macOS),
  64 on Linux aarch64.
- Define `VFS_PAGE_SIZE` as 8192 — the fixed page size for this VFS.
- Detect OS: Linux, macOS, Windows. Define `VFS_OS_LINUX`, `VFS_OS_MACOS`,
  `VFS_OS_WINDOWS`.

**Acceptance:** A test that asserts `VFS_PAGE_SIZE == 8192` and verifies
`VFS_CACHELINE` is a power of two ≥ 64. The header compiles cleanly on
all target platforms.

---

## Workload 1.3 — CRC32C

**What:** A hardware-accelerated CRC-32C (Castagnoli) checksum function.

**Why:** Every page in the VFS carries a CRC32C checksum in its header.
CRC validation runs on every disk read and is the primary corruption
detection mechanism. Software CRC32C on 8KB pages costs ~250 microseconds;
hardware CRC32C costs ~2 microseconds — a 100× difference that directly
impacts read throughput.

**How:**
- Public API: `uint32_t vfs_crc32c(const uint8_t* data, size_t len)`.
  Returns `0x00000000` for zero-length input.
- Software fallback: a 256-entry lookup table using the reversed Castagnoli
  polynomial `0x82F63B78`. Table is computed once at first call. Algorithm:
  initialize `crc = 0xFFFFFFFF`, process each byte as `crc = table[(crc ^
  byte) & 0xFF] ^ (crc >> 8)`, finalize with `crc ^ 0xFFFFFFFF`.
- x86_64 hardware path: use SSE4.2 `_mm_crc32_u64`, `_mm_crc32_u32`, and
  `_mm_crc32_u8` intrinsics. Process data in descending chunk sizes: 8-byte
  chunks first, then 4-byte, then individual bytes.
- aarch64 hardware path: use ARMv8 CRC32 intrinsics `__crc32cd` (8-byte),
  `__crc32cw` (4-byte), `__crc32cb` (1-byte).
- Runtime dispatch: on x86_64, always use the hardware path (SSE4.2 is
  guaranteed on x86_64). On aarch64, always use the hardware path (CRC32 is
  mandatory in ARMv8.1-A). On other architectures, use the software fallback.
- The function must handle unaligned input at all alignments. Use `memcpy`
  to load chunks (the compiler optimizes this to direct loads on platforms
  that support unaligned access).

**Acceptance:** Known test vectors produce correct results:
  - Empty input → `0x00000000`
  - `"123456789"` (9 bytes) → `0xE3069283`
  - 8KB of zeros → deterministic, repeatable value
  - 8KB of `0xFF` bytes → different from zeros
  - 64KB of ascending bytes → deterministic, repeatable value
  - The same input always produces the same output on all platforms.

---

## Workload 1.4 — Atomics & Memory Ordering

**What:** Cross-platform atomic operations with acquire/release semantics
for 32-bit integers, 64-bit integers, and pointer-sized values. Includes
compare-and-swap (CAS), atomic add, and memory barriers.

**Why:** The VFS uses lock-free CAS extensively — pool slot allocation,
version chain head updates, directory entry prepends, and the tree lock
all rely on atomic operations. Correct memory ordering prevents torn reads
and ensures writes are visible across threads.

**How:**
- Public API, all declared in `src/platform.h` as inline functions:
  - `vfs_atomic_load_i64(ptr)` — acquire load, returns value
  - `vfs_atomic_store_i64(ptr, val)` — release store
  - `vfs_atomic_add_i64(ptr, delta)` — atomic add, returns new value
  - `vfs_cas_i64(ptr, expected, desired)` — strong CAS, returns the old
    value (caller compares against expected to determine success)
  - Same four operations for `int32_t` (suffix `_i32`).
  - Same four operations for `void*` (suffix `_ptr`).
  - `vfs_mb_acquire()` — acquire fence (LoadLoad + LoadStore)
  - `vfs_mb_release()` — release fence (LoadStore + StoreStore)
  - `vfs_mb_full()` — sequential consistency fence
- GCC/Clang implementation: use `__atomic` builtins with explicit memory
  order arguments.
- MSVC implementation: use `Interlocked*` intrinsics with `_ReadWriteBarrier`
  and `_mm_mfence` for barriers.
- CAS semantics: if `*ptr == expected`, atomically sets `*ptr = desired` and
  returns `expected`. If `*ptr != expected`, does not modify `*ptr` and
  returns the actual value. This is the "strong" variant — it will not
  spuriously fail.
- All functions are marked `VFS_INLINE` (force-inlined) — they compile to
  single instructions on x86_64 (`lock cmpxchg`, `mfence`, etc.) and
  aarch64 (`ldaxr`/`stlxr` pairs).

**Acceptance:**
  - CAS with matching expected value succeeds and returns the old value.
  - CAS with non-matching expected value fails and returns the current value
    without modifying it.
  - CAS retry loop correctly increments a counter to a known value.
  - Four threads each increment a shared counter 100,000 times using
    `vfs_atomic_add_i64`. The final value is exactly 400,000.
  - `vfs_atomic_store_i64` followed by `vfs_atomic_load_i64` returns the
    stored value (single-threaded sanity check).

---

## Workload 1.5 — Page Buffer Helpers

**What:** Inline functions for reading and writing integers at arbitrary
byte offsets within an 8KB page buffer, plus zero-fill and page-copy
operations.

**Why:** Every page in the VFS is an 8KB byte buffer. Fields within a page
are accessed by byte offset, not by C struct. These helpers centralize the
memcpy-based read/write pattern and provide a fast zero-fill path using SIMD
where available. Defined in `src/page_buf.h` as static inline functions.

**How:**
- `vfs_rd8(buf, offset)` — read 8-byte signed integer at `offset` within
  `buf`. Uses `memcpy` for unaligned-safe access.
- `vfs_rd4(buf, offset)` — read 4-byte signed integer.
- `vfs_rd2(buf, offset)` — read 2-byte signed integer.
- `vfs_wr8(buf, offset, val)` — write 8-byte integer.
- `vfs_wr4(buf, offset, val)` — write 4-byte integer.
- `vfs_wr2(buf, offset, val)` — write 2-byte integer.
- `vfs_zero_page(buf)` — fill all 8192 bytes with zero. On x86_64, use
  SSE2 128-bit stores (`_mm_store_si128` in a loop over 16-byte chunks)
  for 2–3× faster zeroing. Falls back to `memset` on other platforms.
- `vfs_copy_page(dst, src)` — copy 8192 bytes from `src` to `dst`.
  Uses `memcpy`; the compiler will optimize to `rep movsb` or equivalent.
- All functions operate on `uint8_t*` buffers. No bounds checking — the
  caller must ensure `offset + size ≤ VFS_PAGE_SIZE`.

**Acceptance:**
  - Write then read an int64 at offset 0 returns the same value.
  - Write then read at offset 8184 (last 8 bytes of a page) works correctly.
  - Write then read int32 and int16 at various offsets round-trip correctly.
  - `vfs_zero_page` fills the entire buffer with zeros.
  - `vfs_copy_page` produces a byte-identical copy.
  - Fast zero-fill (`vfs_zero_page_fast`) produces the same result as
    `memset` on an all-0xFF buffer.

---

## Workload 1.6 — Error Codes & Debug Logging

**What:** A small error-handling enum and a compile-time-optional debug
logging macro.

**Why:** Every VFS function needs to return success/failure with a
descriptive code. The error enum provides a single source of truth for all
error values. The debug macro allows tracing during development without
runtime overhead in release builds.

**How:**
- Define `vfs_error_t` enum in `include/ixsphere_vfs.h` with codes: `VFS_OK`,
  `VFS_ERR_IO`, `VFS_ERR_NOTFOUND`, `VFS_ERR_EXISTS`, `VFS_ERR_NOTDIR`,
  `VFS_ERR_NOTEMPTY`, `VFS_ERR_CONFLICT`, `VFS_ERR_FULL`, `VFS_ERR_NOMEM`.
- Implement `vfs_error_string(vfs_error_t)` in `src/error.c` — returns a
  human-readable string for each code.
- Define `VFS_DEBUG(fmt, ...)` in `src/platform.h`:
  - When `VFS_DEBUG_ENABLED` is not defined, expands to `((void)0)` — zero
    runtime cost.
  - When defined, prints to stderr with file and line prefix.
- Define `VFS_TRACE(fmt, ...)` similarly, intended for per-call tracing.

**Acceptance:** `vfs_error_string(VFS_ERR_NOTFOUND)` returns `"Not found"`.
Compiling without `VFS_DEBUG_ENABLED` produces no debug output even when
`VFS_DEBUG` is called. Compiling with `VFS_DEBUG_ENABLED` prints messages
to stderr.

---

## Deliverables

| File | Purpose |
|------|---------|
| `CMakeLists.txt` | Build system, static library, test target |
| `src/platform.h` | Compiler/arch/OS detection, macros, atomics, logging |
| `src/crc32c.c` | CRC32C implementation with hardware dispatch |
| `src/page_buf.h` | Integer read/write, zero-fill, page copy |
| `src/error.c` | Error string conversion |
| `include/ixsphere_vfs.h` | `vfs_error_t`, `vfs_crc32c`, `vfs_error_string` |
| `include/vfs_internal.h` | Placeholder for Phase 2 |
| `test/test_runner.c` | Test harness with `CHECK` macro |
| `test/test_crc32c.c` | CRC32C tests with known vectors |
| `test/test_atomics.c` | CAS and concurrent increment tests |
| `test/test_page_buf.c` | Read/write, zero-fill, copy tests |

## Success Criteria
- `cmake --build . && ctest` passes all tests
- CRC32C returns `0xE3069283` for `"123456789"` on all platforms
- Four-thread concurrent counter reaches exactly 400,000
- Page buffer helpers round-trip all values correctly
- Zero warnings at `-Wall -Wextra -Wpedantic`
