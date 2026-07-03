/* Phase 4b–4d: Node type serialization helpers. */
#include "nodes.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * DirNode (Workload 4.1)
 * --------------------------------------------------------------------------- */

void nodes_write_dirnode(uint8_t* slot, uint32_t nodeId, int64_t headPtr) {
    vfs_wr2_s(slot, DIRNODE_OFF_TYPE, (int16_t)NODE_TYPE_DIR, VFS_PAGE_SIZE);
    vfs_wr2_s(slot, DIRNODE_OFF_RSVD, 0, VFS_PAGE_SIZE);
    vfs_wr4_s(slot, DIRNODE_OFF_NODEID, (int32_t)nodeId, VFS_PAGE_SIZE);
    vfs_wr8_s(slot, DIRNODE_OFF_HEADPTR, headPtr, VFS_PAGE_SIZE);
    memset(slot + 16, 0, 16);
}

void nodes_read_dirnode(const uint8_t* slot, uint32_t* nodeId, int64_t* headPtr) {
    *nodeId = (uint32_t)vfs_rd4_s(slot, DIRNODE_OFF_NODEID, VFS_PAGE_SIZE);
    *headPtr = vfs_rd8_s(slot, DIRNODE_OFF_HEADPTR, VFS_PAGE_SIZE);
}

/* ---------------------------------------------------------------------------
 * FileNode (Workload 4.2)
 * --------------------------------------------------------------------------- */

void nodes_write_filenode(uint8_t* slot, uint32_t nodeId, int64_t headPtr,
                          int64_t sizePtr, int64_t createdAt) {
    vfs_wr2_s(slot, FILENODE_OFF_TYPE, (int16_t)NODE_TYPE_FILE, VFS_PAGE_SIZE);
    vfs_wr2_s(slot, FILENODE_OFF_RSVD, 0, VFS_PAGE_SIZE);
    vfs_wr4_s(slot, FILENODE_OFF_NODEID, (int32_t)nodeId, VFS_PAGE_SIZE);
    vfs_wr8_s(slot, FILENODE_OFF_HEADPTR, headPtr, VFS_PAGE_SIZE);
    vfs_wr8_s(slot, FILENODE_OFF_SIZEPTR, sizePtr, VFS_PAGE_SIZE);
    vfs_wr8_s(slot, FILENODE_OFF_CTIME, createdAt, VFS_PAGE_SIZE);
}

void nodes_read_filenode(const uint8_t* slot, uint32_t* nodeId,
                         int64_t* headPtr, int64_t* sizePtr, int64_t* createdAt) {
    *nodeId   = (uint32_t)vfs_rd4_s(slot, FILENODE_OFF_NODEID, VFS_PAGE_SIZE);
    *headPtr  = vfs_rd8_s(slot, FILENODE_OFF_HEADPTR, VFS_PAGE_SIZE);
    *sizePtr  = vfs_rd8_s(slot, FILENODE_OFF_SIZEPTR, VFS_PAGE_SIZE);
    *createdAt = vfs_rd8_s(slot, FILENODE_OFF_CTIME, VFS_PAGE_SIZE);
}

int64_t nodes_read_filenode_ctime(const uint8_t* slot) {
    return vfs_rd8_s(slot, FILENODE_OFF_CTIME, VFS_PAGE_SIZE);
}

/* ---------------------------------------------------------------------------
 * DirContent (Workload 4.3)
 * --------------------------------------------------------------------------- */

void nodes_write_dircontent(uint8_t* slot, uint32_t childNodeId, uint32_t epoch,
                            int64_t childPtr, int64_t namePtr, int64_t nextPtr) {
    vfs_wr4_s(slot, DIRCONTENT_OFF_CHILDID, (int32_t)childNodeId, VFS_PAGE_SIZE);
    vfs_wr4_s(slot, DIRCONTENT_OFF_EPOCH, (int32_t)epoch, VFS_PAGE_SIZE);
    vfs_wr8_s(slot, DIRCONTENT_OFF_CHILDPTR, childPtr, VFS_PAGE_SIZE);
    vfs_wr8_s(slot, DIRCONTENT_OFF_NAMEPTR, namePtr, VFS_PAGE_SIZE);
    vfs_wr8_s(slot, DIRCONTENT_OFF_NEXTPTR, nextPtr, VFS_PAGE_SIZE);
}

void nodes_read_dircontent(const uint8_t* slot, uint32_t* childNodeId,
                           uint32_t* epoch, int64_t* childPtr,
                           int64_t* namePtr, int64_t* nextPtr) {
    *childNodeId = (uint32_t)vfs_rd4_s(slot, DIRCONTENT_OFF_CHILDID, VFS_PAGE_SIZE);
    *epoch       = (uint32_t)vfs_rd4_s(slot, DIRCONTENT_OFF_EPOCH, VFS_PAGE_SIZE);
    *childPtr    = vfs_rd8_s(slot, DIRCONTENT_OFF_CHILDPTR, VFS_PAGE_SIZE);
    *namePtr     = vfs_rd8_s(slot, DIRCONTENT_OFF_NAMEPTR, VFS_PAGE_SIZE);
    *nextPtr     = vfs_rd8_s(slot, DIRCONTENT_OFF_NEXTPTR, VFS_PAGE_SIZE);
}

/* ---------------------------------------------------------------------------
 * FileContent (Workload 4.4)
 * --------------------------------------------------------------------------- */

void nodes_write_filecontent(uint8_t* slot, int64_t pageRootPtr, int64_t nextPtr) {
    vfs_wr8_s(slot, FILECONTENT_OFF_ROOTPTR, pageRootPtr, VFS_PAGE_SIZE);
    vfs_wr8_s(slot, FILECONTENT_OFF_NEXTPTR, nextPtr, VFS_PAGE_SIZE);
    memset(slot + 16, 0, 16);
}

void nodes_read_filecontent(const uint8_t* slot, int64_t* pageRootPtr, int64_t* nextPtr) {
    *pageRootPtr = vfs_rd8_s(slot, FILECONTENT_OFF_ROOTPTR, VFS_PAGE_SIZE);
    *nextPtr     = vfs_rd8_s(slot, FILECONTENT_OFF_NEXTPTR, VFS_PAGE_SIZE);
}

/* ---------------------------------------------------------------------------
 * PageNode (Workload 4.5)
 * --------------------------------------------------------------------------- */

void nodes_write_pagenode(uint8_t* slot, int64_t versionRootPtr, int64_t nextPtr) {
    vfs_wr8_s(slot, PAGENODE_OFF_VERSIONROOT, versionRootPtr, VFS_PAGE_SIZE);
    vfs_wr8_s(slot, PAGENODE_OFF_NEXTPTR, nextPtr, VFS_PAGE_SIZE);
    memset(slot + 16, 0, 16);
}

void nodes_read_pagenode(const uint8_t* slot, int64_t* versionRootPtr, int64_t* nextPtr) {
    *versionRootPtr = vfs_rd8_s(slot, PAGENODE_OFF_VERSIONROOT, VFS_PAGE_SIZE);
    *nextPtr        = vfs_rd8_s(slot, PAGENODE_OFF_NEXTPTR, VFS_PAGE_SIZE);
}

/* ---------------------------------------------------------------------------
 * VersionPage (Workload 4.6)
 * --------------------------------------------------------------------------- */

void nodes_write_versionpage(uint8_t* slot, uint32_t epoch, int64_t dataPage,
                             int64_t nextPtr) {
    vfs_wr4_s(slot, VERSIONPAGE_OFF_EPOCH, (int32_t)epoch, VFS_PAGE_SIZE);
    vfs_wr4_s(slot, VERSIONPAGE_OFF_RSVD, 0, VFS_PAGE_SIZE);
    vfs_wr8_s(slot, VERSIONPAGE_OFF_DATAPAGE, dataPage, VFS_PAGE_SIZE);
    vfs_wr8_s(slot, VERSIONPAGE_OFF_NEXTPTR, nextPtr, VFS_PAGE_SIZE);
    memset(slot + 24, 0, 8);
}

void nodes_read_versionpage(const uint8_t* slot, uint32_t* epoch,
                            int64_t* dataPage, int64_t* nextPtr) {
    *epoch    = (uint32_t)vfs_rd4_s(slot, VERSIONPAGE_OFF_EPOCH, VFS_PAGE_SIZE);
    *dataPage = vfs_rd8_s(slot, VERSIONPAGE_OFF_DATAPAGE, VFS_PAGE_SIZE);
    *nextPtr  = vfs_rd8_s(slot, VERSIONPAGE_OFF_NEXTPTR, VFS_PAGE_SIZE);
}

/* ---------------------------------------------------------------------------
 * FileSize (Workload 4.7)
 * --------------------------------------------------------------------------- */

void nodes_write_filesize(uint8_t* slot, uint32_t epoch, int64_t modifiedAt,
                          int64_t fileSize, int64_t nextPtr) {
    vfs_wr4_s(slot, FILESIZE_OFF_EPOCH, (int32_t)epoch, VFS_PAGE_SIZE);
    vfs_wr8_s(slot, FILESIZE_OFF_MODIFIEDAT, modifiedAt, VFS_PAGE_SIZE);
    vfs_wr8_s(slot, FILESIZE_OFF_FILESIZE, fileSize, VFS_PAGE_SIZE);
    vfs_wr8_s(slot, FILESIZE_OFF_NEXTPTR, nextPtr, VFS_PAGE_SIZE);
    vfs_wr4_s(slot, 28, 0, VFS_PAGE_SIZE);
}

void nodes_read_filesize(const uint8_t* slot, uint32_t* epoch,
                         int64_t* modifiedAt, int64_t* fileSize, int64_t* nextPtr) {
    *epoch      = (uint32_t)vfs_rd4_s(slot, FILESIZE_OFF_EPOCH, VFS_PAGE_SIZE);
    *modifiedAt = vfs_rd8_s(slot, FILESIZE_OFF_MODIFIEDAT, VFS_PAGE_SIZE);
    *fileSize   = vfs_rd8_s(slot, FILESIZE_OFF_FILESIZE, VFS_PAGE_SIZE);
    *nextPtr    = vfs_rd8_s(slot, FILESIZE_OFF_NEXTPTR, VFS_PAGE_SIZE);
}

/* ---------------------------------------------------------------------------
 * TouchedFile (Workload 4.9)
 * --------------------------------------------------------------------------- */

void nodes_write_touchedfile(uint8_t* slot, uint32_t epoch, uint32_t nodeId,
                             int64_t nextPtr) {
    vfs_wr4_s(slot, TOUCHEDFILE_OFF_EPOCH, (int32_t)epoch, VFS_PAGE_SIZE);
    vfs_wr4_s(slot, TOUCHEDFILE_OFF_NODEID, (int32_t)nodeId, VFS_PAGE_SIZE);
    vfs_wr8_s(slot, TOUCHEDFILE_OFF_NEXTPTR, nextPtr, VFS_PAGE_SIZE);
    memset(slot + 16, 0, 16);
}

void nodes_read_touchedfile(const uint8_t* slot, uint32_t* epoch,
                            uint32_t* nodeId, int64_t* nextPtr) {
    *epoch  = (uint32_t)vfs_rd4_s(slot, TOUCHEDFILE_OFF_EPOCH, VFS_PAGE_SIZE);
    *nodeId = (uint32_t)vfs_rd4_s(slot, TOUCHEDFILE_OFF_NODEID, VFS_PAGE_SIZE);
    *nextPtr = vfs_rd8_s(slot, TOUCHEDFILE_OFF_NEXTPTR, VFS_PAGE_SIZE);
}

/* ---------------------------------------------------------------------------
 * MapperEntry (Workload 4.10)
 * --------------------------------------------------------------------------- */

void nodes_write_mapperentry(uint8_t* slot, uint32_t fromEpoch, uint32_t toEpoch,
                             uint16_t flags, int64_t nextPtr) {
    vfs_wr4_s(slot, MAPPER_OFF_FROMEPOCH, (int32_t)fromEpoch, VFS_PAGE_SIZE);
    vfs_wr4_s(slot, MAPPER_OFF_TOEPOCH, (int32_t)toEpoch, VFS_PAGE_SIZE);
    vfs_wr2_s(slot, MAPPER_OFF_FLAGS, (int16_t)flags, VFS_PAGE_SIZE);
    memset(slot + 10, 0, 6);
    vfs_wr8_s(slot, MAPPER_OFF_NEXTPTR, nextPtr, VFS_PAGE_SIZE);
    memset(slot + 24, 0, 8);
}

void nodes_read_mapperentry(const uint8_t* slot, uint32_t* fromEpoch,
                            uint32_t* toEpoch, uint16_t* flags, int64_t* nextPtr) {
    *fromEpoch = (uint32_t)vfs_rd4_s(slot, MAPPER_OFF_FROMEPOCH, VFS_PAGE_SIZE);
    *toEpoch   = (uint32_t)vfs_rd4_s(slot, MAPPER_OFF_TOEPOCH, VFS_PAGE_SIZE);
    *flags     = (uint16_t)vfs_rd2_s(slot, MAPPER_OFF_FLAGS, VFS_PAGE_SIZE);
    *nextPtr   = vfs_rd8_s(slot, MAPPER_OFF_NEXTPTR, VFS_PAGE_SIZE);
}

/* ---------------------------------------------------------------------------
 * NameEntry (Workload 4.8)
 * --------------------------------------------------------------------------- */

void nodes_write_name_entry(uint8_t* slot, const uint8_t* data_24, int64_t nextPtr) {
    memcpy(slot, data_24, NAMEENTRY_DATA_SIZE);
    vfs_wr8_s(slot, NAMEENTRY_OFF_NEXTPTR, nextPtr, VFS_PAGE_SIZE);
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
        nodes_write_name_entry(slot_data, buf, next_vp);

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

        vp = vfs_rd8_s(slot_data, NAMEENTRY_OFF_NEXTPTR, VFS_PAGE_SIZE);
    }

    out_buf[total] = '\0';
    return total;
}
