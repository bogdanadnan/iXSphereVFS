# Phase 1: Platform & Primitives

## Goal
Establish cross-platform foundation: CRC32C, atomics, memory ordering, page buffer helpers.
All code must compile and run on: Linux x86_64, Linux aarch64, macOS x86_64, macOS aarch64, Windows x86_64.
No external dependencies beyond libc and compiler builtins.

---

## 1.1 Project Skeleton

### Directory structure
```
src/
  crc32c.c
  platform.h
  page_buf.h          (header-only)
include/
  ixsphere_vfs.h      (stub — just error codes for now)
  vfs_internal.h      (stub)
test/
  test_runner.c
  test_crc32c.c
  test_atomics.c
  test_page_buf.c
CMakeLists.txt
```

### CMakeLists.txt
```cmake
cmake_minimum_required(VERSION 3.16)
project(iXSphereVFS VERSION 0.1.0 LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "-O3 -march=native -Wall -Wextra -Wpedantic")

add_library(ixsphere_vfs STATIC
    src/crc32c.c
)
target_include_directories(ixsphere_vfs PUBLIC include src)

enable_testing()
add_executable(vfs_test
    test/test_runner.c
    test/test_crc32c.c
    test/test_atomics.c
    test/test_page_buf.c
)
target_link_libraries(vfs_test ixsphere_vfs pthread)
add_test(NAME vfs_test COMMAND vfs_test)
```

### Test harness (`test/test_runner.c`)
```c
#include <stdio.h>
#include <stdlib.h>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(expr) do { \
    tests_run++; \
    if (expr) { tests_passed++; } \
    else { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } \
} while(0)

#define CHECK_EQ(a, b) CHECK((a) == (b))
#define CHECK_NEQ(a, b) CHECK((a) != (b))

extern void test_crc32c(void);
extern void test_atomics(void);
extern void test_page_buf(void);

int main(void) {
    printf("=== iXSphereVFS Phase 1 Tests ===\n\n");
    test_crc32c();
    test_atomics();
    test_page_buf();
    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
```

---

## 1.2 Platform Detection

File: `src/platform.h`

### Compiler detection
```c
#if defined(__GNUC__) || defined(__clang__)
  #define VFS_COMPILER_GCC 1
#elif defined(_MSC_VER)
  #define VFS_COMPILER_MSVC 1
#else
  #error "Unsupported compiler"
#endif
```

### Architecture detection
```c
#if defined(__x86_64__) || defined(_M_X64)
  #define VFS_ARCH_X86_64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
  #define VFS_ARCH_AARCH64 1
#else
  #error "Unsupported architecture"
#endif
```

### Platform macros
```c
#define VFS_INLINE      static inline
#define VFS_LIKELY(x)   __builtin_expect(!!(x), 1)
#define VFS_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define VFS_RESTRICT     __restrict__

#if VFS_ARCH_AARCH64 && defined(__APPLE__)
  #define VFS_CACHELINE 128
#else
  #define VFS_CACHELINE 64
#endif

#define VFS_PAGE_SIZE   8192
```

### Tests (in `test/test_platform.c` or embedded in test_runner)
- Verify `VFS_PAGE_SIZE == 8192`
- Verify `VFS_CACHELINE` is a power of 2 ≥ 64

---

## 1.3 CRC32C

File: `src/crc32c.c` + declaration in `include/ixsphere_vfs.h`

### Public API
```c
#include <stdint.h>
#include <stddef.h>

/* Compute CRC-32C (Castagnoli) over data. Hardware-accelerated on x86_64 and aarch64. */
uint32_t vfs_crc32c(const uint8_t* data, size_t len);
```

### Software fallback
```c
static const uint32_t crc32c_table[256]; // precomputed

static void crc32c_init_table(void) {
    const uint32_t poly = 0x82F63B78u;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? poly ^ (c >> 1) : c >> 1;
        crc32c_table[i] = c;
    }
}

static uint32_t crc32c_software(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = crc32c_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}
```

### x86_64 hardware path (SSE4.2)
```c
#if VFS_ARCH_X86_64
#include <nmmintrin.h>  // SSE4.2

static uint32_t crc32c_hw_x86(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    /* Process 8-byte chunks where possible */
    while (len >= 8) {
        uint64_t chunk;
        memcpy(&chunk, data, 8);   /* unaligned read */
        crc = (uint32_t)_mm_crc32_u64(crc, chunk);
        data += 8; len -= 8;
    }
    /* Process 4-byte chunk */
    if (len >= 4) {
        uint32_t chunk;
        memcpy(&chunk, data, 4);
        crc = _mm_crc32_u32(crc, chunk);
        data += 4; len -= 4;
    }
    /* Process remaining bytes */
    while (len > 0) {
        crc = _mm_crc32_u8(crc, *data);
        data++; len--;
    }
    return crc ^ 0xFFFFFFFFu;
}
#endif
```

### aarch64 hardware path (ARMv8 CRC32)
```c
#if VFS_ARCH_AARCH64
#include <arm_acle.h>

static uint32_t crc32c_hw_arm(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    while (len >= 8) {
        crc = __crc32cd(crc, *(const uint64_t*)data);
        data += 8; len -= 8;
    }
    while (len >= 4) {
        crc = __crc32cw(crc, *(const uint32_t*)data);
        data += 4; len -= 4;
    }
    while (len > 0) {
        crc = __crc32cb(crc, *data);
        data++; len--;
    }
    return crc ^ 0xFFFFFFFFu;
}
#endif
```

### Runtime dispatch
```c
uint32_t vfs_crc32c(const uint8_t* data, size_t len) {
    if (len == 0) return 0x00000000u;
#if VFS_ARCH_X86_64
    return crc32c_hw_x86(data, len);
#elif VFS_ARCH_AARCH64
    return crc32c_hw_arm(data, len);
#else
    static int table_ready = 0;
    if (!table_ready) { crc32c_init_table(); table_ready = 1; }
    return crc32c_software(data, len);
#endif
}
```

### Known test vectors (CRC-32C / Castagnoli / iSCSI)
| Input | CRC32C |
|-------|--------|
| `""` (0 bytes) | `0x00000000` |
| `"123456789"` (9 bytes) | `0xE3069283` |
| 8,192 zero bytes | `0x8A9136AA` (verify by computing once) |

### Test file (`test/test_crc32c.c`)
```c
void test_crc32c(void) {
    printf("1.3 CRC32C...\n");

    /* Empty input */
    CHECK_EQ(vfs_crc32c(NULL, 0), 0x00000000u);

    /* Known vector: "123456789" */
    CHECK_EQ(vfs_crc32c((const uint8_t*)"123456789", 9), 0xE3069283u);

    /* 4-byte aligned */
    uint32_t val = 0xDEADBEEF;
    uint32_t c1 = vfs_crc32c((const uint8_t*)&val, 4);
    CHECK_NEQ(c1, 0x00000000u);

    /* Unaligned: offset by 1 byte */
    uint8_t buf[9] = {0};
    for (int i = 0; i < 9; i++) buf[i] = (uint8_t)i;
    uint32_t c2 = vfs_crc32c(buf + 1, 8);
    CHECK_NEQ(c2, 0x00000000u);

    /* 8KB page of zeros — must match on every run */
    static uint8_t zero_page[8192];
    uint32_t c3 = vfs_crc32c(zero_page, 8192);
    uint32_t c4 = vfs_crc32c(zero_page, 8192);
    CHECK_EQ(c3, c4);

    /* 8KB page of ones */
    static uint8_t ones_page[8192];
    memset(ones_page, 0xFF, 8192);
    uint32_t c5 = vfs_crc32c(ones_page, 8192);
    CHECK_NEQ(c5, c3);  /* different from zeros */

    /* Large input: 64KB of ascending bytes */
    static uint8_t asc[65536];
    for (int i = 0; i < 65536; i++) asc[i] = (uint8_t)i;
    uint32_t c6 = vfs_crc32c(asc, 65536);
    uint32_t c7 = vfs_crc32c(asc, 65536);
    CHECK_EQ(c6, c7);   /* deterministic */
}
```

---

## 1.4 Atomics & Memory Ordering

File: `src/platform.h` (header-only inline functions)

### API surface
```c
/* 64-bit operations */
int64_t  vfs_atomic_load_i64(const int64_t* ptr);
void     vfs_atomic_store_i64(int64_t* ptr, int64_t val);
int64_t  vfs_atomic_add_i64(int64_t* ptr, int64_t delta);
int64_t  vfs_cas_i64(int64_t* ptr, int64_t expected, int64_t desired);
          /* returns old value; if old == expected, *ptr = desired */

/* 32-bit operations */
int32_t  vfs_atomic_load_i32(const int32_t* ptr);
void     vfs_atomic_store_i32(int32_t* ptr, int32_t val);
int32_t  vfs_atomic_add_i32(int32_t* ptr, int32_t delta);
int32_t  vfs_cas_i32(int32_t* ptr, int32_t expected, int32_t desired);

/* Pointer-sized operations */
void*    vfs_atomic_load_ptr(void* const* ptr);
void     vfs_atomic_store_ptr(void** ptr, void* val);
void*    vfs_cas_ptr(void** ptr, void* expected, void* desired);

/* Memory barriers */
void     vfs_mb_acquire(void);   /* #LoadLoad + #LoadStore barrier */
void     vfs_mb_release(void);   /* #LoadStore + #StoreStore barrier */
void     vfs_mb_full(void);      /* full sequential consistency */
```

### Implementation (GCC/Clang)
```c
#if VFS_COMPILER_GCC
  #define VFS_ATOMIC_LOAD(ptr)     __atomic_load_n(ptr, __ATOMIC_ACQUIRE)
  #define VFS_ATOMIC_STORE(ptr,v)  __atomic_store_n(ptr, v, __ATOMIC_RELEASE)
  #define VFS_ATOMIC_ADD(ptr,v)    __atomic_add_fetch(ptr, v, __ATOMIC_ACQ_REL)
  #define VFS_CAS(ptr,exp,des)     ({ typeof(exp) _e = (exp); \
      __atomic_compare_exchange_n(ptr, &_e, des, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE); \
      _e; })

  VFS_INLINE int64_t vfs_atomic_load_i64(const int64_t* p) { return VFS_ATOMIC_LOAD(p); }
  VFS_INLINE void    vfs_atomic_store_i64(int64_t* p, int64_t v) { VFS_ATOMIC_STORE(p, v); }
  VFS_INLINE int64_t vfs_atomic_add_i64(int64_t* p, int64_t v) { return VFS_ATOMIC_ADD(p, v); }
  VFS_INLINE int64_t vfs_cas_i64(int64_t* p, int64_t e, int64_t d) { return VFS_CAS(p, e, d); }
  /* similarly for i32 and ptr variants */
  VFS_INLINE void vfs_mb_acquire(void) { __atomic_thread_fence(__ATOMIC_ACQUIRE); }
  VFS_INLINE void vfs_mb_release(void) { __atomic_thread_fence(__ATOMIC_RELEASE); }
  VFS_INLINE void vfs_mb_full(void)    { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
#endif
```

### Implementation (MSVC)
```c
#if VFS_COMPILER_MSVC
  #include <intrin.h>
  VFS_INLINE int64_t vfs_atomic_load_i64(const volatile int64_t* p) {
      return _InterlockedCompareExchange64((volatile int64_t*)p, 0, 0); }
  VFS_INLINE void vfs_atomic_store_i64(volatile int64_t* p, int64_t v) {
      _InterlockedExchange64(p, v); }
  VFS_INLINE int64_t vfs_cas_i64(volatile int64_t* p, int64_t e, int64_t d) {
      return _InterlockedCompareExchange64(p, d, e); }
  /* ... */
#endif
```

### Tests (`test/test_atomics.c`)
```c
#include <pthread.h>

static int64_t g_counter = 0;

static void* thread_inc(void* arg) {
    int n = *(int*)arg;
    for (int i = 0; i < n; i++)
        vfs_atomic_add_i64(&g_counter, 1);
    return NULL;
}

void test_atomics(void) {
    printf("1.4 Atomics...\n");

    /* Basic CAS */
    int64_t val = 42;
    int64_t old = vfs_cas_i64(&val, 42, 99);
    CHECK_EQ(old, 42);
    CHECK_EQ(val, 99);

    /* Failed CAS */
    old = vfs_cas_i64(&val, 42, 77);
    CHECK_EQ(old, 99);   /* val unchanged, old value returned */
    CHECK_EQ(val, 99);

    /* CAS loop (compare-and-swap retry) */
    val = 0;
    int64_t cur;
    do { cur = vfs_atomic_load_i64(&val); }
    while (vfs_cas_i64(&val, cur, cur + 1) != cur);
    CHECK_EQ(val, 1);

    /* Concurrent increment: 4 threads × 100000 */
    g_counter = 0;
    pthread_t t[4];
    int n = 100000;
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, thread_inc, &n);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    CHECK_EQ(g_counter, 400000);

    /* Load/store ordering (basic — won't catch subtle bugs, but verifies API works) */
    int64_t x = 0;
    vfs_atomic_store_i64(&x, 7);
    CHECK_EQ(vfs_atomic_load_i64(&x), 7);

    /* Pointer CAS */
    int a = 1, b = 2;
    int* ptr = &a;
    void* oldp = vfs_cas_ptr((void**)&ptr, &a, &b);
    CHECK_EQ(oldp, &a);
    CHECK_EQ(ptr, &b);
}
```

---

## 1.5 Page Buffer Helpers

File: `src/page_buf.h` (header-only inline)

### API
```c
#include <string.h>

/* Read integers from byte buffer at given offset (unaligned-safe) */
VFS_INLINE int64_t vfs_rd8(const uint8_t* buf, int offset) {
    int64_t v;
    memcpy(&v, buf + offset, 8);
    return v;
}
VFS_INLINE int32_t vfs_rd4(const uint8_t* buf, int offset) {
    int32_t v;
    memcpy(&v, buf + offset, 4);
    return v;
}
VFS_INLINE int16_t vfs_rd2(const uint8_t* buf, int offset) {
    int16_t v;
    memcpy(&v, buf + offset, 2);
    return v;
}

/* Write integers to byte buffer at given offset (unaligned-safe) */
VFS_INLINE void vfs_wr8(uint8_t* buf, int offset, int64_t val) {
    memcpy(buf + offset, &val, 8);
}
VFS_INLINE void vfs_wr4(uint8_t* buf, int offset, int32_t val) {
    memcpy(buf + offset, &val, 4);
}
VFS_INLINE void vfs_wr2(uint8_t* buf, int offset, int16_t val) {
    memcpy(buf + offset, &val, 2);
}

/* Zero-fill and copy */
VFS_INLINE void vfs_zero_page(uint8_t* buf) {
    memset(buf, 0, VFS_PAGE_SIZE);
}
VFS_INLINE void vfs_copy_page(uint8_t* VFS_RESTRICT dst,
                               const uint8_t* VFS_RESTRICT src) {
    memcpy(dst, src, VFS_PAGE_SIZE);
}

/* Fast zero-fill using SIMD when available */
#if VFS_ARCH_X86_64
#include <emmintrin.h>
VFS_INLINE void vfs_zero_page_fast(uint8_t* buf) {
    __m128i z = _mm_setzero_si128();
    for (int i = 0; i < VFS_PAGE_SIZE; i += 16)
        _mm_store_si128((__m128i*)(buf + i), z);
}
#else
#define vfs_zero_page_fast vfs_zero_page
#endif
```

### Tests (`test/test_page_buf.c`)
```c
void test_page_buf(void) {
    printf("1.5 Page Buffer...\n");

    uint8_t buf[8192];

    /* Write/read int64 at various offsets */
    vfs_wr8(buf, 0, 0x1122334455667788LL);
    CHECK_EQ(vfs_rd8(buf, 0), 0x1122334455667788LL);

    vfs_wr8(buf, 8184, -1);  /* last 8 bytes */
    CHECK_EQ(vfs_rd8(buf, 8184), -1);

    /* Write/read int32 */
    vfs_wr4(buf, 100, 0x7FFFFFFF);
    CHECK_EQ(vfs_rd4(buf, 100), 0x7FFFFFFF);

    /* Write/read int16 */
    vfs_wr2(buf, 200, (int16_t)0x7FFF);
    CHECK_EQ(vfs_rd2(buf, 200), (int16_t)0x7FFF);

    /* Zero-fill */
    vfs_zero_page(buf);
    for (int i = 0; i < VFS_PAGE_SIZE; i++) CHECK_EQ(buf[i], 0);

    /* Copy */
    uint8_t src[8192], dst[8192];
    for (int i = 0; i < VFS_PAGE_SIZE; i++) src[i] = (uint8_t)(i & 0xFF);
    vfs_copy_page(dst, src);
    CHECK_EQ(memcmp(src, dst, VFS_PAGE_SIZE), 0);

    /* Fast zero-fill matches slow */
    uint8_t buf2[8192];
    memset(buf2, 0xFF, VFS_PAGE_SIZE);
    vfs_zero_page_fast(buf2);
    for (int i = 0; i < VFS_PAGE_SIZE; i++) CHECK_EQ(buf2[i], 0);
}
```

---

## 1.6 Error & Logging

File: `include/ixsphere_vfs.h` (stub section)

### Error codes
```c
typedef enum {
    VFS_OK = 0,
    VFS_ERR_IO = -1,
    VFS_ERR_NOTFOUND = -2,
    VFS_ERR_EXISTS = -3,
    VFS_ERR_NOTDIR = -4,
    VFS_ERR_NOTEMPTY = -5,
    VFS_ERR_CONFLICT = -6,
    VFS_ERR_FULL = -7,
    VFS_ERR_NOMEM = -8,
} vfs_error_t;

const char* vfs_error_string(vfs_error_t err);
```

### Implementation (`src/error.c`)
```c
const char* vfs_error_string(vfs_error_t err) {
    switch (err) {
        case VFS_OK:        return "OK";
        case VFS_ERR_IO:    return "I/O error";
        case VFS_ERR_NOTFOUND: return "Not found";
        case VFS_ERR_EXISTS:   return "Already exists";
        case VFS_ERR_NOTDIR:   return "Not a directory";
        case VFS_ERR_NOTEMPTY: return "Directory not empty";
        case VFS_ERR_CONFLICT: return "Conflict";
        case VFS_ERR_FULL:     return "No space left";
        case VFS_ERR_NOMEM:    return "Out of memory";
        default:            return "Unknown error";
    }
}
```

### Debug logging macro (compile-time optional)
```c
/* In platform.h */
#ifndef VFS_DEBUG_ENABLED
  #define VFS_DEBUG(fmt, ...)  ((void)0)
  #define VFS_TRACE(fmt, ...)  ((void)0)
#else
  #define VFS_DEBUG(fmt, ...) fprintf(stderr, "[DEBUG] " fmt "\n", ##__VA_ARGS__)
  #define VFS_TRACE(fmt, ...) fprintf(stderr, "[TRACE] %s:%d " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#endif
```

---

## Deliverables

| File | Purpose |
|------|---------|
| `src/platform.h` | Compiler/arch detection, macros, inline atomics |
| `src/crc32c.c` | CRC32C with hardware dispatch |
| `src/page_buf.h` | Integer read/write, zero-fill, copy (header-only) |
| `src/error.c` | Error string conversion |
| `include/ixsphere_vfs.h` | `vfs_error_t`, `vfs_crc32c`, `vfs_error_string` |
| `include/vfs_internal.h` | Placeholder for Phase 2+ |
| `test/test_runner.c` | Test harness |
| `test/test_crc32c.c` | CRC32C tests with known vectors |
| `test/test_atomics.c` | CAS, concurrent increment, ordering |
| `test/test_page_buf.c` | Read/write helpers, zero-fill, copy |
| `CMakeLists.txt` | Build + test targets |

## Success Criteria
- `cmake --build . && ctest` — all tests pass
- CRC32C returns 0xE3069283 for "123456789" on all platforms
- 4-thread concurrent increment reaches exactly 400,000
- Page buffer read/write round-trips all values correctly
