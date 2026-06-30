/*
 * src/page_buf.h — Page Buffer Helpers
 *
 * Inline functions for reading and writing integers at arbitrary
 * byte offsets within an 8KB page buffer.
 */
#ifndef VFS_PAGE_BUF_H
#define VFS_PAGE_BUF_H

#include "src/platform.h"
#include <stdint.h>
#include <string.h>

/* Include for SIMD on x86_64 */
#if defined(VFS_ARCH_X86_64)
#include <emmintrin.h>
#endif

/* Read 8-byte integer at offset within buf */
static inline int64_t vfs_rd8(const uint8_t* buf, size_t offset) {
    int64_t val;
    memcpy(&val, buf + offset, sizeof(val));
    return val;
}

/* Read 4-byte integer at offset within buf */
static inline int32_t vfs_rd4(const uint8_t* buf, size_t offset) {
    int32_t val;
    memcpy(&val, buf + offset, sizeof(val));
    return val;
}

/* Read 2-byte integer at offset within buf */
static inline int16_t vfs_rd2(const uint8_t* buf, size_t offset) {
    int16_t val;
    memcpy(&val, buf + offset, sizeof(val));
    return val;
}

/* Write 8-byte integer at offset within buf */
static inline void vfs_wr8(uint8_t* buf, size_t offset, int64_t val) {
    memcpy(buf + offset, &val, sizeof(val));
}

/* Write 4-byte integer at offset within buf */
static inline void vfs_wr4(uint8_t* buf, size_t offset, int32_t val) {
    memcpy(buf + offset, &val, sizeof(val));
}

/* Write 2-byte integer at offset within buf */
static inline void vfs_wr2(uint8_t* buf, size_t offset, int16_t val) {
    memcpy(buf + offset, &val, sizeof(val));
}

/* Zero-fill the entire page buffer */
static inline void vfs_zero_page(uint8_t* buf) {
    memset(buf, 0, VFS_PAGE_SIZE);
}

/* Fast zero-fill using SIMD on x86_64 */
static inline void vfs_zero_page_fast(uint8_t* buf) {
#if defined(VFS_ARCH_X86_64)
    __m128i zero = _mm_setzero_si128();
    for (int i = 0; i < VFS_PAGE_SIZE; i += 16) {
        _mm_store_si128((__m128i*)(buf + i), zero);
    }
#else
    memset(buf, 0, VFS_PAGE_SIZE);
#endif
}

/* Copy VFS_PAGE_SIZE bytes from src to dst */
static inline void vfs_copy_page(uint8_t* dst, const uint8_t* src) {
    memcpy(dst, src, VFS_PAGE_SIZE);
}

#endif /* VFS_PAGE_BUF_H */
