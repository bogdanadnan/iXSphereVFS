/* Phase 4b–4d: Node type serialization helpers. */
#include "nodes.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * splitmix64_hash — FNV-1a accumulation + splitmix64 finalizer.
 *
 * FNV-1a provides good dispersion for short inputs (file names are
 * typically < 256 bytes).  The splitmix64 finalizer adds strong
 * avalanche — a single-bit change in the input flips ~32 output bits
 * on average, even for 1- or 2-byte names.
 * --------------------------------------------------------------------------- */
static uint64_t splitmix64_hash(const uint8_t* data, size_t len) {
    uint64_t h = 14695981039346656037ULL;  /* FNV-1a offset basis */
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 1099511628211ULL;  /* FNV-1a prime */
    }
    /* splitmix64 finalizer */
    h += 0x9e3779b97f4a7c15ULL;
    h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ULL;
    h = (h ^ (h >> 27)) * 0x94d049bb133111ebULL;
    return h ^ (h >> 31);
}

uint64_t name_hash_compute(const char* name, int len) {
    return splitmix64_hash((const uint8_t*)name, (size_t)len);
}

/* ---------------------------------------------------------------------------
 * DirNode (Workload 4.1)
 * --------------------------------------------------------------------------- */

void nodes_write_dirnode(uint8_t* slot, uint32_t nodeId, int64_t headPtr,
                          int64_t page_size) {
    vfs_wr2_s(slot, DIRNODE_OFF_TYPE, (int16_t)NODE_TYPE_DIR, page_size);
    vfs_wr2_s(slot, DIRNODE_OFF_RSVD, 0, page_size);
    vfs_wr4_s(slot, DIRNODE_OFF_NODEID, (int32_t)nodeId, page_size);
    vfs_wr8_s(slot, DIRNODE_OFF_HEADPTR, headPtr, page_size);
    memset(slot + 16, 0, 16);
}

void nodes_read_dirnode(const uint8_t* slot, uint32_t* nodeId, int64_t* headPtr,
                         int64_t page_size) {
    *nodeId = (uint32_t)vfs_rd4_s(slot, DIRNODE_OFF_NODEID, page_size);
    *headPtr = vfs_rd8_s(slot, DIRNODE_OFF_HEADPTR, page_size);
}

/* ---------------------------------------------------------------------------
 * FileNode (Workload 4.2)
 * --------------------------------------------------------------------------- */

void nodes_write_filenode(uint8_t* slot, uint32_t nodeId, int64_t headPtr,
                          int64_t sizePtr, int64_t createdAt, int64_t page_size) {
    vfs_wr2_s(slot, FILENODE_OFF_TYPE, (int16_t)NODE_TYPE_FILE, page_size);
    vfs_wr2_s(slot, FILENODE_OFF_RSVD, 0, page_size);
    vfs_wr4_s(slot, FILENODE_OFF_NODEID, (int32_t)nodeId, page_size);
    vfs_wr8_s(slot, FILENODE_OFF_HEADPTR, headPtr, page_size);
    vfs_wr8_s(slot, FILENODE_OFF_SIZEPTR, sizePtr, page_size);
    vfs_wr8_s(slot, FILENODE_OFF_CTIME, createdAt, page_size);
}

void nodes_read_filenode(const uint8_t* slot, uint32_t* nodeId,
                         int64_t* headPtr, int64_t* sizePtr, int64_t* createdAt,
                         int64_t page_size) {
    *nodeId   = (uint32_t)vfs_rd4_s(slot, FILENODE_OFF_NODEID, page_size);
    *headPtr  = vfs_rd8_s(slot, FILENODE_OFF_HEADPTR, page_size);
    *sizePtr  = vfs_rd8_s(slot, FILENODE_OFF_SIZEPTR, page_size);
    *createdAt = vfs_rd8_s(slot, FILENODE_OFF_CTIME, page_size);
}

int64_t nodes_read_filenode_ctime(const uint8_t* slot, int64_t page_size) {
    return vfs_rd8_s(slot, FILENODE_OFF_CTIME, page_size);
}

/* ---------------------------------------------------------------------------
 * DirContent (Workload 4.3)
 * --------------------------------------------------------------------------- */

void nodes_write_dircontent(uint8_t* slot, uint32_t childNodeId, uint32_t epoch,
                            int64_t childPtr, int64_t namePtr, int64_t nextPtr,
                            int64_t page_size) {
    vfs_wr4_s(slot, DIRCONTENT_OFF_CHILDID, (int32_t)childNodeId, page_size);
    vfs_wr4_s(slot, DIRCONTENT_OFF_EPOCH, (int32_t)epoch, page_size);
    vfs_wr8_s(slot, DIRCONTENT_OFF_CHILDPTR, childPtr, page_size);
    vfs_wr8_s(slot, DIRCONTENT_OFF_NAMEPTR, namePtr, page_size);
    vfs_wr8_s(slot, DIRCONTENT_OFF_NEXTPTR, nextPtr, page_size);
}

void nodes_read_dircontent(const uint8_t* slot, uint32_t* childNodeId,
                           uint32_t* epoch, int64_t* childPtr,
                           int64_t* namePtr, int64_t* nextPtr,
                           int64_t page_size) {
    *childNodeId = (uint32_t)vfs_rd4_s(slot, DIRCONTENT_OFF_CHILDID, page_size);
    *epoch       = (uint32_t)vfs_rd4_s(slot, DIRCONTENT_OFF_EPOCH, page_size);
    *childPtr    = vfs_rd8_s(slot, DIRCONTENT_OFF_CHILDPTR, page_size);
    *namePtr     = vfs_rd8_s(slot, DIRCONTENT_OFF_NAMEPTR, page_size);
    *nextPtr     = vfs_rd8_s(slot, DIRCONTENT_OFF_NEXTPTR, page_size);
}

/* ---------------------------------------------------------------------------
 * FileContent (Workload 4.4)
 * --------------------------------------------------------------------------- */

void nodes_write_filecontent(uint8_t* slot, int64_t pageRootPtr, int64_t nextPtr,
                              int64_t page_size) {
    vfs_wr8_s(slot, FILECONTENT_OFF_ROOTPTR, pageRootPtr, page_size);
    vfs_wr8_s(slot, FILECONTENT_OFF_NEXTPTR, nextPtr, page_size);
    memset(slot + 16, 0, 16);
}

void nodes_read_filecontent(const uint8_t* slot, int64_t* pageRootPtr, int64_t* nextPtr,
                             int64_t page_size) {
    *pageRootPtr = vfs_rd8_s(slot, FILECONTENT_OFF_ROOTPTR, page_size);
    *nextPtr     = vfs_rd8_s(slot, FILECONTENT_OFF_NEXTPTR, page_size);
}

/* ---------------------------------------------------------------------------
 * PageNode (Workload 4.5)
 * --------------------------------------------------------------------------- */

void nodes_write_pagenode(uint8_t* slot, int64_t versionRootPtr, int64_t nextPtr,
                           uint32_t page_index, int64_t page_size) {
    vfs_wr8_s(slot, PAGENODE_OFF_VERSIONROOT, versionRootPtr, page_size);
    vfs_wr8_s(slot, PAGENODE_OFF_NEXTPTR, nextPtr, page_size);
    vfs_wr4_s(slot, PAGENODE_OFF_PAGEINDEX, (int32_t)page_index, page_size);
    memset(slot + 20, 0, 12);
}

void nodes_read_pagenode(const uint8_t* slot, int64_t* versionRootPtr,
                         int64_t* nextPtr, uint32_t* page_index, int64_t page_size) {
    *versionRootPtr = vfs_rd8_s(slot, PAGENODE_OFF_VERSIONROOT, page_size);
    *nextPtr        = vfs_rd8_s(slot, PAGENODE_OFF_NEXTPTR, page_size);
    if (page_index) *page_index = (uint32_t)vfs_rd4_s(slot, PAGENODE_OFF_PAGEINDEX, page_size);
}

/* ---------------------------------------------------------------------------
 * VersionPage (Workload 4.6)
 * --------------------------------------------------------------------------- */

void nodes_write_versionpage(uint8_t* slot, uint32_t epoch, int64_t dataPage,
                             int64_t nextPtr, int64_t page_size) {
    vfs_wr4_s(slot, VERSIONPAGE_OFF_EPOCH, (int32_t)epoch, page_size);
    vfs_wr4_s(slot, VERSIONPAGE_OFF_RSVD, 0, page_size);
    vfs_wr8_s(slot, VERSIONPAGE_OFF_DATAPAGE, dataPage, page_size);
    vfs_wr8_s(slot, VERSIONPAGE_OFF_NEXTPTR, nextPtr, page_size);
    memset(slot + 24, 0, 8);
}

void nodes_read_versionpage(const uint8_t* slot, uint32_t* epoch,
                            int64_t* dataPage, int64_t* nextPtr,
                            int64_t page_size) {
    *epoch    = (uint32_t)vfs_rd4_s(slot, VERSIONPAGE_OFF_EPOCH, page_size);
    *dataPage = vfs_rd8_s(slot, VERSIONPAGE_OFF_DATAPAGE, page_size);
    *nextPtr  = vfs_rd8_s(slot, VERSIONPAGE_OFF_NEXTPTR, page_size);
}

/* ---------------------------------------------------------------------------
 * FileSize (Workload 4.7)
 * --------------------------------------------------------------------------- */

void nodes_write_filesize(uint8_t* slot, uint32_t epoch, int64_t modifiedAt,
                          int64_t fileSize, int64_t nextPtr,
                          int64_t page_size) {
    vfs_wr4_s(slot, FILESIZE_OFF_EPOCH, (int32_t)epoch, page_size);
    vfs_wr8_s(slot, FILESIZE_OFF_MODIFIEDAT, modifiedAt, page_size);
    vfs_wr8_s(slot, FILESIZE_OFF_FILESIZE, fileSize, page_size);
    vfs_wr8_s(slot, FILESIZE_OFF_NEXTPTR, nextPtr, page_size);
    vfs_wr4_s(slot, 28, 0, page_size);
}

void nodes_read_filesize(const uint8_t* slot, uint32_t* epoch,
                         int64_t* modifiedAt, int64_t* fileSize, int64_t* nextPtr,
                         int64_t page_size) {
    *epoch      = (uint32_t)vfs_rd4_s(slot, FILESIZE_OFF_EPOCH, page_size);
    *modifiedAt = vfs_rd8_s(slot, FILESIZE_OFF_MODIFIEDAT, page_size);
    *fileSize   = vfs_rd8_s(slot, FILESIZE_OFF_FILESIZE, page_size);
    *nextPtr    = vfs_rd8_s(slot, FILESIZE_OFF_NEXTPTR, page_size);
}

/* ---------------------------------------------------------------------------
 * TouchedFile (Workload 4.9)
 * --------------------------------------------------------------------------- */

void nodes_write_touchedfile(uint8_t* slot, uint32_t epoch, uint32_t nodeId,
                             int64_t nextPtr, int64_t page_size) {
    vfs_wr4_s(slot, TOUCHEDFILE_OFF_EPOCH, (int32_t)epoch, page_size);
    vfs_wr4_s(slot, TOUCHEDFILE_OFF_NODEID, (int32_t)nodeId, page_size);
    vfs_wr8_s(slot, TOUCHEDFILE_OFF_NEXTPTR, nextPtr, page_size);
    memset(slot + 16, 0, 16);
}

void nodes_read_touchedfile(const uint8_t* slot, uint32_t* epoch,
                            uint32_t* nodeId, int64_t* nextPtr,
                            int64_t page_size) {
    *epoch  = (uint32_t)vfs_rd4_s(slot, TOUCHEDFILE_OFF_EPOCH, page_size);
    *nodeId = (uint32_t)vfs_rd4_s(slot, TOUCHEDFILE_OFF_NODEID, page_size);
    *nextPtr = vfs_rd8_s(slot, TOUCHEDFILE_OFF_NEXTPTR, page_size);
}

/* ---------------------------------------------------------------------------
 * MapperEntry (Workload 4.10)
 * --------------------------------------------------------------------------- */

void nodes_write_mapperentry(uint8_t* slot, uint32_t fromEpoch, uint32_t toEpoch,
                             uint16_t flags, int64_t nextPtr,
                             int64_t page_size) {
    vfs_wr4_s(slot, MAPPER_OFF_FROMEPOCH, (int32_t)fromEpoch, page_size);
    vfs_wr4_s(slot, MAPPER_OFF_TOEPOCH, (int32_t)toEpoch, page_size);
    vfs_wr2_s(slot, MAPPER_OFF_FLAGS, (int16_t)flags, page_size);
    memset(slot + 10, 0, 6);
    vfs_wr8_s(slot, MAPPER_OFF_NEXTPTR, nextPtr, page_size);
    memset(slot + 24, 0, 8);
}

void nodes_read_mapperentry(const uint8_t* slot, uint32_t* fromEpoch,
                            uint32_t* toEpoch, uint16_t* flags, int64_t* nextPtr,
                            int64_t page_size) {
    *fromEpoch = (uint32_t)vfs_rd4_s(slot, MAPPER_OFF_FROMEPOCH, page_size);
    *toEpoch   = (uint32_t)vfs_rd4_s(slot, MAPPER_OFF_TOEPOCH, page_size);
    *flags     = (uint16_t)vfs_rd2_s(slot, MAPPER_OFF_FLAGS, page_size);
    *nextPtr   = vfs_rd8_s(slot, MAPPER_OFF_NEXTPTR, page_size);
}

/* ---------------------------------------------------------------------------
 * NameEntry (Workload 4.8)
 * --------------------------------------------------------------------------- */

void nodes_write_name_entry(uint8_t* slot, const uint8_t* data_24, int64_t nextPtr,
                             int64_t page_size) {
    memcpy(slot, data_24, NAMEENTRY_DATA_SIZE);
    vfs_wr8_s(slot, NAMEENTRY_OFF_NEXTPTR, nextPtr, page_size);
}

int nodes_write_name(Pool* pool, const char* utf8_name, int64_t* first_slot_vp) {
    if (!utf8_name || utf8_name[0] == '\0') {
        *first_slot_vp = VFS_VPTR_NULL;
        return 0;
    }

    size_t total_len = strlen(utf8_name);
    int slots_needed = (int)((total_len + NAMEENTRY_DATA_SIZE - 1) / NAMEENTRY_DATA_SIZE);
    int64_t next_vp = 0;

    /* Allocate slots in reverse order (last slot first) */
    for (int i = slots_needed - 1; i >= 0; i--) {
        int64_t vp = pool_alloc(pool);
        if (vp == VFS_VPTR_NULL) {
            /* Allocation failure — partial chain may exist */
            *first_slot_vp = VFS_VPTR_NULL;
            return 0;
        }

        uint8_t* slot_data = pool_resolve(pool, vp);
        if (!slot_data) {
            *first_slot_vp = VFS_VPTR_NULL;
            return 0;
        }

        size_t offset = (size_t)i * NAMEENTRY_DATA_SIZE;
        size_t chunk = total_len - offset;
        if (chunk > NAMEENTRY_DATA_SIZE)
            chunk = NAMEENTRY_DATA_SIZE;

        uint8_t buf[NAMEENTRY_DATA_SIZE] = {0};
        memcpy(buf, utf8_name + offset, chunk);
        nodes_write_name_entry(slot_data, buf, next_vp, pool->sb->page_size);

        if (i == 0)
            *first_slot_vp = vp;

        next_vp = vp;
    }

    return slots_needed;
}

int nodes_read_name(Pool* pool, int64_t first_slot_vp, char* out_buf, int max_len) {
    if (first_slot_vp == VFS_VPTR_NULL || max_len <= 0) {
        if (max_len > 0) out_buf[0] = '\0';
        return 0;
    }

    int total = 0;
    int64_t vp = first_slot_vp;

    while (vp != VFS_VPTR_NULL && total < max_len - 1) {
        uint8_t* slot_data = pool_resolve(pool, vp);
        if (!slot_data) break;

        /* Copy up to NAMEENTRY_DATA_SIZE bytes, stopping at first null */
        for (int i = 0; i < NAMEENTRY_DATA_SIZE && total < max_len - 1; i++) {
            uint8_t byte = slot_data[i];
            if (byte == 0) {
                out_buf[total] = '\0';
                return total;
            }
            out_buf[total++] = (char)byte;
        }

        vp = vfs_rd8_s(slot_data, NAMEENTRY_OFF_NEXTPTR, pool->sb->page_size);
    }

    out_buf[total] = '\0';
    return total;
}
