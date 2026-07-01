#include "ixsphere_vfs.h"
#include "platform.h"
#include <string.h>

/* 256-entry lookup table for CRC32C (Castagnoli) - polynomial 0x82F63B78 (reversed) */
static uint32_t s_crc32c_table[256];
static int s_table_ready = 0;

static void init_crc32c_table(void) {
    if (s_table_ready) return;
    
    for (int i = 0; i < 256; i++) {
        uint32_t crc = (uint32_t)i;
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0x82F63B78 : 0);
        }
        s_crc32c_table[i] = crc;
    }
    s_table_ready = 1;
}

uint32_t vfs_crc32c(const uint8_t* data, size_t len) {
    if (len == 0 || data == NULL) return 0x00000000;
    
    init_crc32c_table();
    
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ s_crc32c_table[(crc ^ data[i]) & 0xFF];
    }
    
    return crc ^ 0xFFFFFFFF;
}
