/*
 * src/crc32c.c — CRC32C (Castagnoli) Implementation
 *
 * Hardware-accelerated CRC where available, with software fallback.
 */
#include "ixsphere_vfs.h"
#include <string.h>

/* Castagnoli polynomial in reversed form */
#define CRC32C_POLY 0x82F63B78

/* Software fallback lookup table */
static uint32_t crc32c_table[256];
static int crc32c_table_built = 0;

static void build_crc32c_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ ((crc & 1) ? CRC32C_POLY : 0);
        }
        crc32c_table[i] = crc;
    }
    crc32c_table_built = 1;
}

/* Software CRC32C - handles any alignment */
static uint32_t crc32c_software(const uint8_t* data, size_t len) {
    if (!crc32c_table_built) {
        build_crc32c_table();
    }
    
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc = crc32c_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

#ifdef VFS_ARCH_X86_64

#include <immintrin.h>

uint32_t vfs_crc32c(const uint8_t* data, size_t len) {
    if (len == 0) return 0x00000000;
    
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t* p = data;
    
    /* Process 8-byte chunks */
    while (len >= 8) {
        crc = _mm_crc32_u64(crc, *((uint64_t*)p));
        p += 8;
        len -= 8;
    }
    
    /* Process 4-byte chunks */
    while (len >= 4) {
        crc = _mm_crc32_u32(crc, *((uint32_t*)p));
        p += 4;
        len -= 4;
    }
    
    /* Process remaining bytes */
    while (len > 0) {
        crc = _mm_crc32_u8(crc, *p);
        p++;
        len--;
    }
    
    return crc ^ 0xFFFFFFFF;
}

#elif defined(VFS_ARCH_AARCH64)

/* ARMv8 CRC32 intrinsics */
#ifdef VFS_OS_MACOS
/* macOS uses different intrinsic names */
uint32_t vfs_crc32c(const uint8_t* data, size_t len) {
    if (len == 0) return 0x00000000;
    
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t* p = data;
    
    /* Process in chunks using aarch64 intrinsics */
    while (len >= 8) {
        __asm__ __volatile__(
            "crc32x  %w0, %w0, %1"
            : "+r"(crc)
            : "r"(*((uint64_t*)p))
        );
        p += 8;
        len -= 8;
    }
    
    while (len >= 4) {
        __asm__ __volatile__(
            "crc32w  %w0, %w0, %1"
            : "+r"(crc)
            : "r"(*((uint32_t*)p))
        );
        p += 4;
        len -= 4;
    }
    
    while (len > 0) {
        __asm__ __volatile__(
            "crc32b  %w0, %w0, %1"
            : "+r"(crc)
            : "r"(*p)
        );
        p++;
        len--;
    }
    
    return crc ^ 0xFFFFFFFF;
}
#else
/* Linux aarch64 */
#include <arm_acle.h>

uint32_t vfs_crc32c(const uint8_t* data, size_t len) {
    if (len == 0) return 0x00000000;
    
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t* p = data;
    
    /* Process in descending chunk sizes */
    while (len >= 8) {
        crc = __crc32cd(crc, *((uint64_t*)p));
        p += 8;
        len -= 8;
    }
    
    while (len >= 4) {
        crc = __crc32cw(crc, *((uint32_t*)p));
        p += 4;
        len -= 4;
    }
    
    while (len > 0) {
        crc = __crc32cb(crc, *p);
        p++;
        len--;
    }
    
    return crc ^ 0xFFFFFFFF;
}
#endif

#else

/* Generic software fallback */
uint32_t vfs_crc32c(const uint8_t* data, size_t len) {
    return crc32c_software(data, len);
}

#endif
