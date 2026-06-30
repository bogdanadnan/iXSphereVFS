/*
 * src/platform.h — Platform Detection & Primitives
 *
 * Detects compiler, architecture, and OS at compile time. Provides
 * consistent macros used by the rest of the codebase.
 */
#ifndef VFS_PLATFORM_H
#define VFS_PLATFORM_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* ── Compiler Detection ─────────────────────────────────── */

#if defined(__clang__)
    #define VFS_COMPILER_CLANG 1
    #define VFS_COMPILER_GCC 1
#elif defined(__GNUC__)
    #define VFS_COMPILER_GCC 1
#elif defined(_MSC_VER)
    #define VFS_COMPILER_MSVC 1
#else
    #error "Unsupported compiler: need GCC, Clang, or MSVC"
#endif

/* ── Architecture Detection ──────────────────────────────── */

#if defined(__x86_64__) || defined(_M_X64)
    #define VFS_ARCH_X86_64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define VFS_ARCH_AARCH64 1
#else
    #error "Unsupported architecture: need x86_64 or aarch64"
#endif

/* ── OS Detection ──────────────────────────────────────── */

#if defined(__linux__)
    #define VFS_OS_LINUX 1
#elif defined(__APPLE__) && defined(__MACH__)
    #define VFS_OS_MACOS 1
#elif defined(_WIN32) || defined(_WIN64)
    #define VFS_OS_WINDOWS 1
#else
    #error "Unsupported OS: need Linux, macOS, or Windows"
#endif

/* ── Utility Macros ────────────────────────────────────── */

#define VFS_INLINE static inline
#define VFS_LIKELY(x)   __builtin_expect(!!(x), 1)
#define VFS_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define VFS_RESTRICT __restrict

/* ── Atomic Bit Operations (GCC/Clang) ─────────────────────── */

#if defined(VFS_COMPILER_GCC) || defined(VFS_COMPILER_CLANG)

VFS_INLINE int vfs_atomic_bit_test_and_set(uint8_t* ptr, int bit) {
    uint8_t mask = 1 << (bit & 7);
    uint8_t* byte_ptr = ptr + (bit >> 3);
    uint8_t old = __atomic_fetch_or(byte_ptr, mask, __ATOMIC_ACQ_REL);
    return (old & mask) != 0;
}

VFS_INLINE int vfs_atomic_bit_test_and_reset(uint8_t* ptr, int bit) {
    uint8_t mask = 1 << (bit & 7);
    uint8_t* byte_ptr = ptr + (bit >> 3);
    uint8_t old = __atomic_fetch_and(byte_ptr, ~mask, __ATOMIC_ACQ_REL);
    return (old & mask) != 0;
}

VFS_INLINE int vfs_atomic_bit_test(const uint8_t* ptr, int bit) {
    uint8_t mask = 1 << (bit & 7);
    uint8_t* byte_ptr = (uint8_t*)ptr + (bit >> 3);
    return (__atomic_load_n(byte_ptr, __ATOMIC_ACQUIRE) & mask) != 0;
}

/* MSVC atomics for AArch64 */
#elif defined(VFS_COMPILER_MSVC)

VFS_INLINE int vfs_atomic_bit_test_and_set(uint8_t* ptr, int bit) {
    return _bittestandset((volatile char*)ptr, bit);
}

VFS_INLINE int vfs_atomic_bit_test_and_reset(uint8_t* ptr, int bit) {
    return _bittestandreset((volatile char*)ptr, bit);
}

VFS_INLINE int vfs_atomic_bit_test(const uint8_t* ptr, int bit) {
    return _bittest((volatile char*)ptr, bit);
}

#endif

/* ── Platform Constants ─────────────────────────────────── */

#ifdef VFS_ARCH_X86_64
    #define VFS_CACHELINE 64
#endif

#ifdef VFS_ARCH_AARCH64
    #ifdef VFS_OS_MACOS
        #define VFS_CACHELINE 128
    #else
        #define VFS_CACHELINE 64
    #endif
#endif

#ifndef VFS_CACHELINE
    #define VFS_CACHELINE 64
#endif

/* VFS uses a fixed 8KB page size */
#define VFS_PAGE_SIZE 8192

/* ── Debug Logging ─────────────────────────────────────── */

#ifdef VFS_DEBUG_ENABLED
    #define VFS_DEBUG(fmt, ...) \
        fprintf(stderr, "DEBUG %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
    #define VFS_DEBUG(fmt, ...) ((void)0)
#endif

#ifdef VFS_TRACE_ENABLED
    #define VFS_TRACE(fmt, ...) \
        fprintf(stderr, "TRACE %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
    #define VFS_TRACE(fmt, ...) ((void)0)
#endif

/* ── Atomics & Memory Ordering (GCC/Clang) ───────────────── */

#if defined(VFS_COMPILER_GCC) || defined(VFS_COMPILER_CLANG)

VFS_INLINE void vfs_mb_acquire(void) {
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
}

VFS_INLINE void vfs_mb_release(void) {
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

VFS_INLINE void vfs_mb_full(void) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

VFS_INLINE int32_t vfs_atomic_load_i32(const int32_t* ptr) {
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

VFS_INLINE void vfs_atomic_store_i32(int32_t* ptr, int32_t val) {
    __atomic_store_n(ptr, val, __ATOMIC_RELEASE);
}

VFS_INLINE int32_t vfs_atomic_add_i32(int32_t* ptr, int32_t delta) {
    return __atomic_add_fetch(ptr, delta, __ATOMIC_ACQ_REL);
}

VFS_INLINE int32_t vfs_cas_i32(int32_t* ptr, int32_t expected, int32_t desired) {
    __atomic_compare_exchange_n(ptr, &expected, desired, 0,
                                __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
    return expected;
}

VFS_INLINE int64_t vfs_atomic_load_i64(const int64_t* ptr) {
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

VFS_INLINE void vfs_atomic_store_i64(int64_t* ptr, int64_t val) {
    __atomic_store_n(ptr, val, __ATOMIC_RELEASE);
}

VFS_INLINE int64_t vfs_atomic_add_i64(int64_t* ptr, int64_t delta) {
    return __atomic_add_fetch(ptr, delta, __ATOMIC_ACQ_REL);
}

VFS_INLINE int64_t vfs_cas_i64(int64_t* ptr, int64_t expected, int64_t desired) {
    __atomic_compare_exchange_n(ptr, &expected, desired, 0,
                                __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
    return expected;
}

VFS_INLINE void* vfs_atomic_load_ptr(const void* volatile* ptr) {
    return (void*)__atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

VFS_INLINE void vfs_atomic_store_ptr(void* volatile* ptr, void* val) {
    __atomic_store_n(ptr, val, __ATOMIC_RELEASE);
}

VFS_INLINE void* vfs_atomic_add_ptr(void* volatile* ptr, ptrdiff_t delta) {
    return (void*)__atomic_add_fetch((int64_t*)ptr, delta, __ATOMIC_ACQ_REL);
}

VFS_INLINE void* vfs_cas_ptr(void* volatile* ptr, void* expected, void* desired) {
    __atomic_compare_exchange_n(ptr, &expected, desired, 0,
                                __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
    return expected;
}

/* ── Atomics & Memory Ordering (MSVC) ────────────────────── */

#elif defined(VFS_COMPILER_MSVC)

#include <intrin.h>
#include <windows.h>

#pragma intrinsic(_mm_mfence)
#pragma intrinsic(_ReadWriteBarrier)

VFS_INLINE void vfs_mb_acquire(void) {
    _ReadWriteBarrier();  /* Compiler barrier; x86 has implicit acquire semantics */
}

VFS_INLINE void vfs_mb_release(void) {
    _ReadWriteBarrier();  /* Compiler barrier; x86 has implicit release semantics */
}

VFS_INLINE void vfs_mb_full(void) {
    _mm_mfence();
}

/* MSVC 32-bit atomics */
#if defined(_M_X64)
#define VFS_ATOMIC_64BIT 1
#endif

VFS_INLINE int32_t vfs_atomic_load_i32(const int32_t* ptr) {
    return InterlockedOr((volatile LONG*)ptr, 0);
}

VFS_INLINE void vfs_atomic_store_i32(int32_t* ptr, int32_t val) {
    InterlockedExchange((volatile LONG*)ptr, val);
}

VFS_INLINE int32_t vfs_atomic_add_i32(int32_t* ptr, int32_t delta) {
    return InterlockedAdd((volatile LONG*)ptr, delta);
}

VFS_INLINE int32_t vfs_cas_i32(int32_t* ptr, int32_t expected, int32_t desired) {
    return InterlockedCompareExchange((volatile LONG*)ptr, desired, expected);
}

VFS_INLINE int64_t vfs_atomic_load_i64(const int64_t* ptr) {
    return InterlockedOr64((volatile LONG64*)ptr, 0);
}

VFS_INLINE void vfs_atomic_store_i64(int64_t* ptr, int64_t val) {
    InterlockedExchange64((volatile LONG64*)ptr, val);
}

VFS_INLINE int64_t vfs_atomic_add_i64(int64_t* ptr, int64_t delta) {
    return InterlockedAdd64((volatile LONG64*)ptr, delta);
}

VFS_INLINE int64_t vfs_cas_i64(int64_t* ptr, int64_t expected, int64_t desired) {
    return InterlockedCompareExchange64((volatile LONG64*)ptr, desired, expected);
}

VFS_INLINE void* vfs_atomic_load_ptr(const void* const* ptr) {
    return (void*)vfs_atomic_load_i64((const int64_t*)ptr);
}

VFS_INLINE void vfs_atomic_store_ptr(void* volatile* ptr, void* val) {
    vfs_atomic_store_i64((int64_t*)ptr, (int64_t)val);
}

VFS_INLINE void* vfs_atomic_add_ptr(void* volatile* ptr, ptrdiff_t delta) {
    return (void*)vfs_atomic_add_i64((int64_t*)ptr, (int64_t)delta);
}

VFS_INLINE void* vfs_cas_ptr(void* volatile* ptr, void* expected, void* desired) {
    int64_t old = (int64_t)expected;
    expected = (void*)InterlockedCompareExchange64((volatile LONG64*)ptr, (LONG64)desired, old);
    return expected;
}

#endif /* Compiler */

#endif /* VFS_PLATFORM_H */
