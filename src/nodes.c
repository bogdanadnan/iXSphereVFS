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
