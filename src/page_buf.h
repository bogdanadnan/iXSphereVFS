#ifndef VFS_PAGE_BUF_H
#define VFS_PAGE_BUF_H

#include "ixsphere_vfs.h"
#include "platform.h"
#include <string.h>
#include <assert.h>

VFS_INLINE int64_t vfs_rd8(const uint8_t* buf, int offset) {
    int64_t val;
    memcpy(&val, buf + offset, 8);
    return val;
}

VFS_INLINE int32_t vfs_rd4(const uint8_t* buf, int offset) {
    int32_t val;
    memcpy(&val, buf + offset, 4);
    return val;
}

VFS_INLINE int16_t vfs_rd2(const uint8_t* buf, int offset) {
    int16_t val;
    memcpy(&val, buf + offset, 2);
    return val;
}

VFS_INLINE void vfs_wr8(uint8_t* buf, int offset, int64_t val) {
    memcpy(buf + offset, &val, 8);
}

VFS_INLINE void vfs_wr4(uint8_t* buf, int offset, int32_t val) {
    memcpy(buf + offset, &val, 4);
}

VFS_INLINE void vfs_wr2(uint8_t* buf, int offset, int16_t val) {
    memcpy(buf + offset, &val, 2);
}

VFS_INLINE void vfs_zero_page(uint8_t* buf) {
    memset(buf, 0, VFS_PAGE_SIZE);
}

VFS_INLINE void vfs_copy_page(uint8_t* dst, const uint8_t* src) {
    memcpy(dst, src, VFS_PAGE_SIZE);
}

#endif /* VFS_PAGE_BUF_H */
