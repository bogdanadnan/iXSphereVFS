#ifndef VFS_PAGE_BUF_H
#define VFS_PAGE_BUF_H

#include "ixsphere_vfs.h"
#include "platform.h"
#include <string.h>

#ifndef NDEBUG
    #include <assert.h>
    #define VFS_BOUNDS_CHECK(offset, size) assert((offset) + (size) <= VFS_PAGE_SIZE)
#else
    #define VFS_BOUNDS_CHECK(offset, size) ((void)0)
#endif

VFS_INLINE int64_t vfs_rd8(const uint8_t* buf, int offset) {
    VFS_BOUNDS_CHECK(offset, 8);
    int64_t val;
    memcpy(&val, buf + offset, 8);
    return val;
}

VFS_INLINE int32_t vfs_rd4(const uint8_t* buf, int offset) {
    VFS_BOUNDS_CHECK(offset, 4);
    int32_t val;
    memcpy(&val, buf + offset, 4);
    return val;
}

VFS_INLINE int16_t vfs_rd2(const uint8_t* buf, int offset) {
    VFS_BOUNDS_CHECK(offset, 2);
    int16_t val;
    memcpy(&val, buf + offset, 2);
    return val;
}

VFS_INLINE void vfs_wr8(uint8_t* buf, int offset, int64_t val) {
    VFS_BOUNDS_CHECK(offset, 8);
    memcpy(buf + offset, &val, 8);
}

VFS_INLINE void vfs_wr4(uint8_t* buf, int offset, int32_t val) {
    VFS_BOUNDS_CHECK(offset, 4);
    memcpy(buf + offset, &val, 4);
}

VFS_INLINE void vfs_wr2(uint8_t* buf, int offset, int16_t val) {
    VFS_BOUNDS_CHECK(offset, 2);
    memcpy(buf + offset, &val, 2);
}

VFS_INLINE void vfs_zero_page(uint8_t* buf) {
    memset(buf, 0, VFS_PAGE_SIZE);
}

#if VFS_ARCH_X86_64 && VFS_COMPILER_GCC
    #include <immintrin.h>

    VFS_INLINE void vfs_zero_page_fast(uint8_t* buf) {
        __m128i zero = _mm_setzero_si128();
        for (int i = 0; i < VFS_PAGE_SIZE; i += 16) {
            _mm_storeu_si128((__m128i*)(buf + i), zero);
        }
    }
#else
    #define vfs_zero_page_fast vfs_zero_page
#endif

VFS_INLINE void vfs_copy_page(uint8_t* dst, const uint8_t* src) {
    memcpy(dst, src, VFS_PAGE_SIZE);
}

#endif /* VFS_PAGE_BUF_H */
