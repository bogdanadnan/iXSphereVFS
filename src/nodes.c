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
