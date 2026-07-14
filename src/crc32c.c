#include "ixsphere/vfs.h"
#include "platform.h"
#include <string.h>

#if VFS_ARCH_X86_64 && VFS_COMPILER_GCC
    #include <nmmintrin.h>  /* SSE4.2 — _mm_crc32_u64 / _mm_crc32_u32 / _mm_crc32_u8 */
#endif

#if VFS_ARCH_AARCH64
    #include <arm_acle.h>   /* ARMv8 — __crc32cd / __crc32cw / __crc32cb */
#endif

/* ---------------------------------------------------------------------------
 * Lookup table (256 entries, Castagnoli polynomial 0x82F63B78)
 *
 * Initialized at library load time via __attribute__((constructor)) on
 * GCC/Clang, or on first call (double-checked with an atomic flag) on MSVC.
 * --------------------------------------------------------------------------- */

static uint32_t s_crc32c_table[256];

static void vfs_crc32c_init_table(void) {
    for (int i = 0; i < 256; i++) {
        uint32_t crc = (uint32_t)i;
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0x82F63B78u : 0u);
        }
        s_crc32c_table[i] = crc;
    }
}

#if VFS_COMPILER_GCC
    /* Run once before main() — no threading concern.
     * NOTE: on x86_64 the HW CRC32C path is taken unconditionally
     * (see crc32c() below), so this table is computed but never
     * used.  It's harmless (~1KB of .rodata + 1024-byte init at
     * load time) and keeps the fallback path ready if the HW
     * path is ever disabled at runtime. */
    __attribute__((constructor))
    static void vfs_crc32c_ctor(void) {
        vfs_crc32c_init_table();
    }
#elif VFS_COMPILER_MSVC
    /* On MSVC, run on first call via a static flag (acceptable —
       the constructor pattern has no MSVC equivalent that works
       reliably across DLL/static-lib boundaries). */
    static volatile LONG s_table_ready = 0;
    static void vfs_crc32c_ensure_table(void) {
        if (InterlockedCompareExchange(&s_table_ready, 1, 0) == 0) {
            vfs_crc32c_init_table();
            InterlockedExchange(&s_table_ready, 2);
        }
        while (s_table_ready != 2) { YieldProcessor(); }
    }
#endif

/* ---------------------------------------------------------------------------
 * x86_64 hardware path  (SSE4.2 CRC32C intrinsics)
 *
 * __attribute__((target("sse4.2"))) lets only this function use SSE4.2
 * instructions without raising the ISA baseline for the whole library.
 * --------------------------------------------------------------------------- */

#if VFS_ARCH_X86_64 && VFS_COMPILER_GCC

__attribute__((target("sse4.2")))
static uint32_t vfs_crc32c_hw_x86(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    size_t i = 0;

    for (; i + 8 <= len; i += 8) {
        uint64_t chunk;
        memcpy(&chunk, data + i, 8);
        crc = (uint32_t)_mm_crc32_u64(crc, chunk);
    }
    for (; i + 4 <= len; i += 4) {
        uint32_t chunk;
        memcpy(&chunk, data + i, 4);
        crc = _mm_crc32_u32(crc, chunk);
    }
    for (; i < len; i++) {
        crc = _mm_crc32_u8(crc, data[i]);
    }
    return crc ^ 0xFFFFFFFFu;
}

#endif /* x86_64 */

/* ---------------------------------------------------------------------------
 * aarch64 hardware path  (ARMv8 CRC32 intrinsics)
 * --------------------------------------------------------------------------- */

#if VFS_ARCH_AARCH64

static uint32_t vfs_crc32c_hw_arm(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    size_t i = 0;

    for (; i + 8 <= len; i += 8) {
        uint64_t chunk;
        memcpy(&chunk, data + i, 8);
        crc = __crc32cd(crc, chunk);
    }
    for (; i + 4 <= len; i += 4) {
        uint32_t chunk;
        memcpy(&chunk, data + i, 4);
        crc = __crc32cw(crc, chunk);
    }
    for (; i < len; i++) {
        crc = __crc32cb(crc, data[i]);
    }
    return crc ^ 0xFFFFFFFFu;
}

#endif /* aarch64 */

/* ---------------------------------------------------------------------------
 * Software fallback  — only compiled when no hardware path is active,
 * or when targeting MSVC (which uses software table init as well).
 * --------------------------------------------------------------------------- */

#if !(VFS_ARCH_X86_64 && VFS_COMPILER_GCC) && !VFS_ARCH_AARCH64

static uint32_t vfs_crc32c_sw(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ s_crc32c_table[(crc ^ data[i]) & 0xFFu];
    }
    return crc ^ 0xFFFFFFFFu;
}

#endif

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

uint32_t vfs_crc32c(const uint8_t* data, size_t len) {
    if (len == 0 || data == NULL) return 0x00000000u;

#if VFS_COMPILER_MSVC
    vfs_crc32c_ensure_table();
#endif

#if VFS_ARCH_X86_64 && VFS_COMPILER_GCC
    return vfs_crc32c_hw_x86(data, len);
#elif VFS_ARCH_AARCH64
    return vfs_crc32c_hw_arm(data, len);
#else
    return vfs_crc32c_sw(data, len);
#endif
}
