#ifndef VFS_PAGE_BUF_H
#define VFS_PAGE_BUF_H

#include "ixsphere/vfs.h"
#include "platform.h"
#include <string.h>

#ifndef NDEBUG
    #include <assert.h>
    #define VFS_BOUNDS_CHECK(offset, size) assert((offset) + (size) <= VFS_PAGE_SIZE)
    #define VFS_BOUNDS_CHECK_S(offset, size, ps) assert((offset) + (size) <= (ps))
#else
    #define VFS_BOUNDS_CHECK(offset, size) ((void)0)
    #define VFS_BOUNDS_CHECK_S(offset, size, ps) ((void)0)
#endif

/* ---------------------------------------------------------------------------
 * Integer read/write at byte offsets — VFS_PAGE_SIZE-bounded (legacy).
 *
 * These variants exist for backward compatibility with existing call sites
 * and test code.  New code should prefer the `_s` variants below that
 * accept an explicit `page_size` parameter.
 * --------------------------------------------------------------------------- */

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

/* ---------------------------------------------------------------------------
 * Integer read/write with explicit page_size (primary API).
 *
 * These `_s` variants accept an explicit `page_size` parameter and should
 * be preferred for all new code.  The non-`_s` variants above exist only
 * for legacy call sites that always use VFS_PAGE_SIZE buffers.
 *
 * Call with sb->page_size, ctx->page_size, or pool->sb->page_size
 * for accurate bounds checking.  Use VFS_PAGE_SIZE only when the buffer
 * is guaranteed to be a VFS_PAGE_SIZE (8192) page (e.g., pool slots,
 * header pages).
 * --------------------------------------------------------------------------- */

VFS_INLINE int64_t vfs_rd8_s(const uint8_t* buf, int offset, int64_t ps) {
    VFS_BOUNDS_CHECK_S(offset, 8, ps);
    int64_t val;
    memcpy(&val, buf + offset, 8);
    return val;
}

VFS_INLINE int32_t vfs_rd4_s(const uint8_t* buf, int offset, int64_t ps) {
    VFS_BOUNDS_CHECK_S(offset, 4, ps);
    int32_t val;
    memcpy(&val, buf + offset, 4);
    return val;
}

VFS_INLINE int16_t vfs_rd2_s(const uint8_t* buf, int offset, int64_t ps) {
    VFS_BOUNDS_CHECK_S(offset, 2, ps);
    int16_t val;
    memcpy(&val, buf + offset, 2);
    return val;
}

VFS_INLINE void vfs_wr8_s(uint8_t* buf, int offset, int64_t val, int64_t ps) {
    VFS_BOUNDS_CHECK_S(offset, 8, ps);
    memcpy(buf + offset, &val, 8);
}

VFS_INLINE void vfs_wr4_s(uint8_t* buf, int offset, int32_t val, int64_t ps) {
    VFS_BOUNDS_CHECK_S(offset, 4, ps);
    memcpy(buf + offset, &val, 4);
}

VFS_INLINE void vfs_wr2_s(uint8_t* buf, int offset, int16_t val, int64_t ps) {
    VFS_BOUNDS_CHECK_S(offset, 2, ps);
    memcpy(buf + offset, &val, 2);
}

/* ---------------------------------------------------------------------------
 * Page buffer operations.
 *
 * These accept a `page_size` parameter for StorageBackend compatibility.
 * The VFS layer calls them with VFS_PAGE_SIZE; the StorageBackend calls
 * them with sb->page_size.
 * --------------------------------------------------------------------------- */

VFS_INLINE void vfs_zero_page(uint8_t* buf, int64_t page_size) {
    memset(buf, 0, (size_t)page_size);
}

#if VFS_ARCH_X86_64 && VFS_COMPILER_GCC
    #include <emmintrin.h>  /* SSE2 — _mm_setzero_si128 / _mm_storeu_si128 */

    VFS_INLINE void vfs_zero_page_fast(uint8_t* buf, int64_t page_size) {
        __m128i zero = _mm_setzero_si128();
        int64_t i;
        for (i = 0; i + 16 <= page_size; i += 16) {
            _mm_storeu_si128((__m128i*)(buf + i), zero);
        }
        /* Zero remaining bytes */
        for (; i < page_size; i++) {
            buf[i] = 0;
        }
    }
#else
    #define vfs_zero_page_fast vfs_zero_page
#endif

VFS_INLINE void vfs_copy_page(uint8_t* dst, const uint8_t* src, int64_t page_size) {
    memcpy(dst, src, (size_t)page_size);
}

#endif /* VFS_PAGE_BUF_H */
