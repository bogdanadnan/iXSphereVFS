#ifndef VFS_PLATFORM_H
#define VFS_PLATFORM_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Compiler detection */
#if defined(__GNUC__) || defined(__clang__)
    #define VFS_COMPILER_GCC 1
    #define VFS_COMPILER_MSVC 0
#elif defined(_MSC_VER)
    #define VFS_COMPILER_GCC 0
    #define VFS_COMPILER_MSVC 1
#else
    #error "Unsupported compiler"
#endif

/* Architecture detection */
#if defined(__x86_64__) || defined(_M_X64)
    #define VFS_ARCH_X86_64 1
    #define VFS_ARCH_AARCH64 0
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define VFS_ARCH_X86_64 0
    #define VFS_ARCH_AARCH64 1
#else
    #error "Unsupported architecture"
#endif

/* OS detection */
#if defined(__linux__)
    #define VFS_OS_LINUX 1
    #define VFS_OS_MACOS 0
    #define VFS_OS_WINDOWS 0
#elif defined(__APPLE__) && defined(__MACH__)
    #define VFS_OS_LINUX 0
    #define VFS_OS_MACOS 1
    #define VFS_OS_WINDOWS 0
#elif defined(_WIN32)
    #define VFS_OS_LINUX 0
    #define VFS_OS_MACOS 0
    #define VFS_OS_WINDOWS 1
#else
    #error "Unsupported OS"
#endif

/* Cache line size: 64 on x86_64, 128 on Apple Silicon (aarch64 macOS), 64 on Linux aarch64 */
#if VFS_ARCH_AARCH64 && VFS_OS_MACOS
    #define VFS_CACHELINE 128
#else
    #define VFS_CACHELINE 64
#endif

/* Inline hint */
#define VFS_INLINE static inline

/* Branch prediction hints */
#if VFS_COMPILER_GCC
    #define VFS_LIKELY(x)   __builtin_expect(!!(x), 1)
    #define VFS_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    #define VFS_LIKELY(x)   (x)
    #define VFS_UNLIKELY(x) (x)
#endif

/* Aliasing hint */
#if VFS_COMPILER_GCC
    #define VFS_RESTRICT __restrict__
#elif VFS_COMPILER_MSVC
    #define VFS_RESTRICT __restrict
#else
    #define VFS_RESTRICT
#endif

/* Default page size */
#define VFS_PAGE_SIZE 8192

/* ============================================================================
 * Atomics & Memory Ordering (Workload 1.4)
 * ============================================================================ */

#if VFS_COMPILER_GCC

/* int64_t atomics */
VFS_INLINE int64_t vfs_atomic_load_i64(const int64_t* ptr) {
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

VFS_INLINE void vfs_atomic_store_i64(int64_t* ptr, int64_t val) {
    __atomic_store_n(ptr, val, __ATOMIC_RELEASE);
}

VFS_INLINE int64_t vfs_atomic_add_i64(int64_t* ptr, int64_t delta) {
    return __atomic_add_fetch(ptr, delta, __ATOMIC_RELAXED);
}

VFS_INLINE int64_t vfs_cas_i64(int64_t* ptr, int64_t expected, int64_t desired) {
    __atomic_compare_exchange_n(ptr, &expected, desired, 0, 
                                 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return expected;
}

/* int32_t atomics */
VFS_INLINE int32_t vfs_atomic_load_i32(const int32_t* ptr) {
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

VFS_INLINE void vfs_atomic_store_i32(int32_t* ptr, int32_t val) {
    __atomic_store_n(ptr, val, __ATOMIC_RELEASE);
}

VFS_INLINE int32_t vfs_atomic_add_i32(int32_t* ptr, int32_t delta) {
    return __atomic_add_fetch(ptr, delta, __ATOMIC_RELAXED);
}

VFS_INLINE int32_t vfs_cas_i32(int32_t* ptr, int32_t expected, int32_t val) {
    __atomic_compare_exchange_n(ptr, &expected, val, 0, 
                                 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return expected;
}

/* void* atomics */
VFS_INLINE void* vfs_atomic_load_ptr(const void* const* ptr) {
    return (void*)__atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

VFS_INLINE void vfs_atomic_store_ptr(void** ptr, void* val) {
    __atomic_store_n(ptr, val, __ATOMIC_RELEASE);
}

VFS_INLINE void* vfs_atomic_add_ptr(void** ptr, intptr_t delta) {
    return (void*)__atomic_add_fetch(ptr, delta, __ATOMIC_RELAXED);
}

VFS_INLINE void* vfs_cas_ptr(void** ptr, void* expected, void* desired) {
    __atomic_compare_exchange_n(ptr, &expected, desired, 0, 
                                 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return expected;
}

/* Memory barriers */
VFS_INLINE void vfs_mb_acquire(void) {
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
}

VFS_INLINE void vfs_mb_release(void) {
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

VFS_INLINE void vfs_mb_full(void) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

#elif VFS_COMPILER_MSVC

#include <intrin.h>

/* int64_t atomics */
VFS_INLINE int64_t vfs_atomic_load_i64(const int64_t* ptr) {
    return InterlockedCompareExchange64((volatile int64_t*)ptr, 0, 0);
}

VFS_INLINE void vfs_atomic_store_i64(int64_t* ptr, int64_t val) {
    InterlockedExchange64((volatile int64_t*)ptr, val);
}

VFS_INLINE int64_t vfs_atomic_add_i64(int64_t* ptr, int64_t delta) {
    return InterlockedAdd64((volatile int64_t*)ptr, delta);
}

VFS_INLINE int64_t vfs_cas_i64(int64_t* ptr, int64_t expected, int64_t desired) {
    return InterlockedCompareExchange64((volatile int64_t*)ptr, desired, expected);
}

/* int32_t atomics */
VFS_INLINE int32_t vfs_atomic_load_i32(const int32_t* ptr) {
    return InterlockedCompareExchange((volatile int32_t*)ptr, 0, 0);
}

VFS_INLINE void vfs_atomic_store_i32(int32_t* ptr, int32_t val) {
    InterlockedExchange((volatile int32_t*)ptr, val);
}

VFS_INLINE int32_t vfs_atomic_add_i32(int32_t* ptr, int32_t delta) {
    return InterlockedAdd((volatile int32_t*)ptr, delta);
}

VFS_INLINE int32_t vfs_cas_i32(int32_t* ptr, int32_t expected, int32_t val) {
    return InterlockedCompareExchange((volatile int32_t*)ptr, val, expected);
}

/* void* atomics */
VFS_INLINE void* vfs_atomic_load_ptr(const void* const* ptr) {
    return (void*)InterlockedCompareExchangePointer((volatile void* const*)ptr, NULL, NULL);
}

VFS_INLINE void vfs_atomic_store_ptr(void** ptr, void* val) {
    InterlockedExchangePointer((volatile void**)ptr, val);
}

VFS_INLINE void* vfs_atomic_add_ptr(void** ptr, intptr_t delta) {
    void* expected = (void*)InterlockedCompareExchangePointer((volatile void**)ptr, NULL, NULL);
    void* desired;
    void* actual;
    do {
        desired = (void*)((char*)expected + delta);
        actual = (void*)InterlockedCompareExchangePointer((volatile void**)ptr, desired, expected);
        if (actual == expected) break;
        expected = actual;   /* re-read on failure */
    } while (1);
    return desired;
}

VFS_INLINE void* vfs_cas_ptr(void** ptr, void* expected, void* desired) {
    return (void*)InterlockedCompareExchangePointer((volatile void**)ptr, desired, expected);
}

/* Memory barriers */
VFS_INLINE void vfs_mb_acquire(void) {
    _ReadWriteBarrier();
}

VFS_INLINE void vfs_mb_release(void) {
    _ReadWriteBarrier();
}

VFS_INLINE void vfs_mb_full(void) {
    __faststorefence();
}

#endif /* atomics */

#endif /* VFS_PLATFORM_H */
