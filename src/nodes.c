/* Phase 4b–4d: Node type serialization helpers. */
#include "nodes.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * DirNode (Workload 4.1)
 * --------------------------------------------------------------------------- */

void nodes_write_dirnode(uint8_t* slot, uint32_t nodeId, int64_t headPtr) {
    vfs_wr2(slot, DIRNODE_OFF_TYPE, (int16_t)NODE_TYPE_DIR);
    vfs_wr2(slot, DIRNODE_OFF_RSVD, 0);
    vfs_wr4(slot, DIRNODE_OFF_NODEID, (int32_t)nodeId);
    vfs_wr8(slot, DIRNODE_OFF_HEADPTR, headPtr);
    memset(slot + 16, 0, 16);
}

void nodes_read_dirnode(const uint8_t* slot, uint32_t* nodeId, int64_t* headPtr) {
    *nodeId = (uint32_t)vfs_rd4(slot, DIRNODE_OFF_NODEID);
    *headPtr = vfs_rd8(slot, DIRNODE_OFF_HEADPTR);
}

/* ---------------------------------------------------------------------------
 * FileNode (Workload 4.2)
 * --------------------------------------------------------------------------- */

void nodes_write_filenode(uint8_t* slot, uint32_t nodeId, int64_t headPtr,
                          int64_t sizePtr, int64_t createdAt) {
    vfs_wr2(slot, FILENODE_OFF_TYPE, (int16_t)NODE_TYPE_FILE);
    vfs_wr2(slot, FILENODE_OFF_RSVD, 0);
    vfs_wr4(slot, FILENODE_OFF_NODEID, (int32_t)nodeId);
    vfs_wr8(slot, FILENODE_OFF_HEADPTR, headPtr);
    vfs_wr8(slot, FILENODE_OFF_SIZEPTR, sizePtr);
    vfs_wr8(slot, FILENODE_OFF_CTIME, createdAt);
}

void nodes_read_filenode(const uint8_t* slot, uint32_t* nodeId,
                         int64_t* headPtr, int64_t* sizePtr, int64_t* createdAt) {
    *nodeId   = (uint32_t)vfs_rd4(slot, FILENODE_OFF_NODEID);
    *headPtr  = vfs_rd8(slot, FILENODE_OFF_HEADPTR);
    *sizePtr  = vfs_rd8(slot, FILENODE_OFF_SIZEPTR);
    *createdAt = vfs_rd8(slot, FILENODE_OFF_CTIME);
}

int64_t nodes_read_filenode_ctime(const uint8_t* slot) {
    return vfs_rd8(slot, FILENODE_OFF_CTIME);
}

/* ---------------------------------------------------------------------------
 * DirContent (Workload 4.3)
 * --------------------------------------------------------------------------- */

void nodes_write_dircontent(uint8_t* slot, uint32_t childNodeId, uint32_t epoch,
                            int64_t childPtr, int64_t namePtr, int64_t nextPtr) {
    vfs_wr4(slot, DIRCONTENT_OFF_CHILDID, (int32_t)childNodeId);
    vfs_wr4(slot, DIRCONTENT_OFF_EPOCH, (int32_t)epoch);
    vfs_wr8(slot, DIRCONTENT_OFF_CHILDPTR, childPtr);
    vfs_wr8(slot, DIRCONTENT_OFF_NAMEPTR, namePtr);
    vfs_wr8(slot, DIRCONTENT_OFF_NEXTPTR, nextPtr);
}

void nodes_read_dircontent(const uint8_t* slot, uint32_t* childNodeId,
                           uint32_t* epoch, int64_t* childPtr,
                           int64_t* namePtr, int64_t* nextPtr) {
    *childNodeId = (uint32_t)vfs_rd4(slot, DIRCONTENT_OFF_CHILDID);
    *epoch       = (uint32_t)vfs_rd4(slot, DIRCONTENT_OFF_EPOCH);
    *childPtr    = vfs_rd8(slot, DIRCONTENT_OFF_CHILDPTR);
    *namePtr     = vfs_rd8(slot, DIRCONTENT_OFF_NAMEPTR);
    *nextPtr     = vfs_rd8(slot, DIRCONTENT_OFF_NEXTPTR);
}

/* ---------------------------------------------------------------------------
 * FileContent (Workload 4.4)
 * --------------------------------------------------------------------------- */

void nodes_write_filecontent(uint8_t* slot, int64_t pageRootPtr, int64_t nextPtr) {
    vfs_wr8(slot, FILECONTENT_OFF_ROOTPTR, pageRootPtr);
    vfs_wr8(slot, FILECONTENT_OFF_NEXTPTR, nextPtr);
    memset(slot + 16, 0, 16);
}

void nodes_read_filecontent(const uint8_t* slot, int64_t* pageRootPtr, int64_t* nextPtr) {
    *pageRootPtr = vfs_rd8(slot, FILECONTENT_OFF_ROOTPTR);
    *nextPtr     = vfs_rd8(slot, FILECONTENT_OFF_NEXTPTR);
}

/* ---------------------------------------------------------------------------
 * PageNode (Workload 4.5)
 * --------------------------------------------------------------------------- */

void nodes_write_pagenode(uint8_t* slot, int64_t versionRootPtr, int64_t nextPtr) {
    vfs_wr8(slot, PAGENODE_OFF_VERSIONROOT, versionRootPtr);
    vfs_wr8(slot, PAGENODE_OFF_NEXTPTR, nextPtr);
    memset(slot + 16, 0, 16);
}

void nodes_read_pagenode(const uint8_t* slot, int64_t* versionRootPtr, int64_t* nextPtr) {
    *versionRootPtr = vfs_rd8(slot, PAGENODE_OFF_VERSIONROOT);
    *nextPtr        = vfs_rd8(slot, PAGENODE_OFF_NEXTPTR);
}

/* ---------------------------------------------------------------------------
 * VersionPage (Workload 4.6)
 * --------------------------------------------------------------------------- */

void nodes_write_versionpage(uint8_t* slot, uint32_t epoch, int64_t dataPage,
                             int64_t nextPtr) {
    vfs_wr4(slot, VERSIONPAGE_OFF_EPOCH, (int32_t)epoch);
    vfs_wr4(slot, VERSIONPAGE_OFF_RSVD, 0);
    vfs_wr8(slot, VERSIONPAGE_OFF_DATAPAGE, dataPage);
    vfs_wr8(slot, VERSIONPAGE_OFF_NEXTPTR, nextPtr);
    memset(slot + 24, 0, 8);
}

void nodes_read_versionpage(const uint8_t* slot, uint32_t* epoch,
                            int64_t* dataPage, int64_t* nextPtr) {
    *epoch    = (uint32_t)vfs_rd4(slot, VERSIONPAGE_OFF_EPOCH);
    *dataPage = vfs_rd8(slot, VERSIONPAGE_OFF_DATAPAGE);
    *nextPtr  = vfs_rd8(slot, VERSIONPAGE_OFF_NEXTPTR);
}

/* ---------------------------------------------------------------------------
 * FileSize (Workload 4.7)
 * --------------------------------------------------------------------------- */

void nodes_write_filesize(uint8_t* slot, uint32_t epoch, int64_t modifiedAt,
                          int64_t fileSize, int64_t nextPtr) {
    vfs_wr4(slot, FILESIZE_OFF_EPOCH, (int32_t)epoch);
    vfs_wr8(slot, FILESIZE_OFF_MODIFIEDAT, modifiedAt);
    vfs_wr8(slot, FILESIZE_OFF_FILESIZE, fileSize);
    vfs_wr8(slot, FILESIZE_OFF_NEXTPTR, nextPtr);
    vfs_wr4(slot, 28, 0);
}

void nodes_read_filesize(const uint8_t* slot, uint32_t* epoch,
                         int64_t* modifiedAt, int64_t* fileSize, int64_t* nextPtr) {
    *epoch      = (uint32_t)vfs_rd4(slot, FILESIZE_OFF_EPOCH);
    *modifiedAt = vfs_rd8(slot, FILESIZE_OFF_MODIFIEDAT);
    *fileSize   = vfs_rd8(slot, FILESIZE_OFF_FILESIZE);
    *nextPtr    = vfs_rd8(slot, FILESIZE_OFF_NEXTPTR);
}
