#ifndef VFS_NODES_H
#define VFS_NODES_H

#include "pool.h"
#include "page_buf.h"

/* ---------------------------------------------------------------------------
 * Type discriminator constants (DirNode and FileNode only)
 * --------------------------------------------------------------------------- */

#define NODE_TYPE_DIR   0x01
#define NODE_TYPE_FILE  0x03

/* ---------------------------------------------------------------------------
 * MapperEntry flags
 * --------------------------------------------------------------------------- */

#define MAPPER_FLAG_TRAVERSAL_APPLY  0x0001

/* ---------------------------------------------------------------------------
 * DirNode (32 bytes, 16 used, 16 reserved)
 *
 *   Offset  Size  Field
 *   ──────  ────  ─────
 *     0      2    type        (uint16 — NODE_TYPE_DIR)
 *     2      2    reserved
 *     4      4    nodeId      (uint32)
 *     8      8    headPtr     (VirtualPtr — first DirContent)
 *    16     16    reserved
 * --------------------------------------------------------------------------- */

#define DIRNODE_OFF_TYPE      0
#define DIRNODE_OFF_RSVD      2
#define DIRNODE_OFF_NODEID    4
#define DIRNODE_OFF_HEADPTR   8

void nodes_write_dirnode(uint8_t* slot, uint32_t nodeId, int64_t headPtr, int64_t page_size);
void nodes_read_dirnode(const uint8_t* slot, uint32_t* nodeId, int64_t* headPtr, int64_t page_size);

/* ---------------------------------------------------------------------------
 * FileNode (32 bytes, fully packed)
 *
 *   Offset  Size  Field
 *   ──────  ────  ─────
 *     0      2    type        (uint16 — NODE_TYPE_FILE)
 *     2      2    reserved
 *     4      4    nodeId      (uint32)
 *     8      8    headPtr     (VirtualPtr — first FileContent)
 *    16      8    sizePtr     (VirtualPtr — first FileSize, 0 if none)
 *    24      8    createdAt   (int64 — Unix timestamp, immutable)
 * --------------------------------------------------------------------------- */

#define FILENODE_OFF_TYPE      0
#define FILENODE_OFF_RSVD      2
#define FILENODE_OFF_NODEID    4
#define FILENODE_OFF_HEADPTR   8
#define FILENODE_OFF_SIZEPTR  16
#define FILENODE_OFF_CTIME    24

void nodes_write_filenode(uint8_t* slot, uint32_t nodeId, int64_t headPtr,
                          int64_t sizePtr, int64_t createdAt, int64_t page_size);
void nodes_read_filenode(const uint8_t* slot, uint32_t* nodeId,
                         int64_t* headPtr, int64_t* sizePtr, int64_t* createdAt, int64_t page_size);
int64_t nodes_read_filenode_ctime(const uint8_t* slot, int64_t page_size);

/* ---------------------------------------------------------------------------
 * DirContent (32 bytes, fully packed)
 *
 *   Offset  Size  Field
 *   ──────  ────  ─────
 *     0      4    childNodeId  (uint32)
 *     4      4    epoch        (uint32)
 *     8      8    childPtr     (VirtualPtr — DirNode or FileNode)
 *    16      8    namePtr      (VirtualPtr — first NameEntry; 0 = tombstone)
 *    24      8    nextPtr      (VirtualPtr — next DirContent, 0 = end)
 * --------------------------------------------------------------------------- */

#define DIRCONTENT_OFF_CHILDID     0
#define DIRCONTENT_OFF_EPOCH         4
#define DIRCONTENT_OFF_CHILDPTR      8
#define DIRCONTENT_OFF_NAMEPTR      16
#define DIRCONTENT_OFF_NEXTPTR      24

void nodes_write_dircontent(uint8_t* slot, uint32_t childNodeId, uint32_t epoch,
                            int64_t childPtr, int64_t namePtr, int64_t nextPtr, int64_t page_size);
void nodes_read_dircontent(const uint8_t* slot, uint32_t* childNodeId,
                           uint32_t* epoch, int64_t* childPtr,
                           int64_t* namePtr, int64_t* nextPtr, int64_t page_size);

/* ---------------------------------------------------------------------------
 * FileContent (32 bytes, 16 used, 16 reserved)
 *
 *   Offset  Size  Field
 *   ──────  ────  ─────
 *     0      8    pageRootPtr  (VirtualPtr — first PageNode in segment)
 *     8      8    nextPtr      (VirtualPtr — next FileContent, 0 = end)
 *    16      4    pageCount    (uint32 — number of PageNodes in this segment)
 *    20     12    reserved
 * --------------------------------------------------------------------------- */

#define FILECONTENT_OFF_ROOTPTR     0
#define FILECONTENT_OFF_NEXTPTR       8
#define FILECONTENT_OFF_PAGECOUNT    16

void nodes_write_filecontent(uint8_t* slot, int64_t pageRootPtr, int64_t nextPtr, int64_t page_size);
void nodes_read_filecontent(const uint8_t* slot, int64_t* pageRootPtr, int64_t* nextPtr, int64_t page_size);

/* ---------------------------------------------------------------------------
 * PageNode (32 bytes, 20 used, 12 reserved)
 *
 *   Offset  Size  Field
 *   ──────  ────  ─────
 *     0      8    versionRoot  (VirtualPtr — first VersionPage, 0 = unwritten)
 *     8      8    nextPtr      (VirtualPtr — next PageNode, 0 = end)
 *    16      4    pageIndex    (uint32 — logical page index within segment)
 *    20     12    reserved
 * --------------------------------------------------------------------------- */

#define PAGENODE_OFF_VERSIONROOT    0
#define PAGENODE_OFF_NEXTPTR          8
#define PAGENODE_OFF_PAGEINDEX       16
#define PAGENODE_OFF_RESERVED2       20

void nodes_write_pagenode(uint8_t* slot, int64_t versionRootPtr, int64_t nextPtr,
                          uint32_t page_index, int64_t page_size);
void nodes_read_pagenode(const uint8_t* slot, int64_t* versionRootPtr,
                         int64_t* nextPtr, uint32_t* page_index, int64_t page_size);

/* ---------------------------------------------------------------------------
 * VersionPage (32 bytes, 20 used, 12 reserved)
 *
 *   Offset  Size  Field
 *   ──────  ────  ─────
 *     0      4    epoch      (uint32)
 *     4      4    reserved
 *     8      8    dataPage   (int64 — logical page index of data page)
 *    16      8    nextPtr    (VirtualPtr — next older VersionPage, 0 = base)
 *    24      8    reserved
 * --------------------------------------------------------------------------- */

#define VERSIONPAGE_OFF_EPOCH      0
#define VERSIONPAGE_OFF_RSVD       4
#define VERSIONPAGE_OFF_DATAPAGE   8
#define VERSIONPAGE_OFF_NEXTPTR   16

void nodes_write_versionpage(uint8_t* slot, uint32_t epoch, int64_t dataPage,
                             int64_t nextPtr, int64_t page_size);
void nodes_read_versionpage(const uint8_t* slot, uint32_t* epoch,
                            int64_t* dataPage, int64_t* nextPtr, int64_t page_size);

/* ---------------------------------------------------------------------------
 * FileSize (32 bytes, 24 used, 8 reserved)
 *
 *   Offset  Size  Field
 *   ──────  ────  ─────
 *     0      4    epoch       (uint32)
 *     4      8    modifiedAt  (int64 — Unix timestamp)
 *    12      8    fileSize    (int64 — file size in bytes)
 *    20      8    nextPtr     (VirtualPtr — next FileSize, 0 = end)
 *    28      4    reserved
 * --------------------------------------------------------------------------- */

#define FILESIZE_OFF_EPOCH       0
#define FILESIZE_OFF_MODIFIEDAT  4
#define FILESIZE_OFF_FILESIZE   12
#define FILESIZE_OFF_NEXTPTR    20

void nodes_write_filesize(uint8_t* slot, uint32_t epoch, int64_t modifiedAt,
                          int64_t fileSize, int64_t nextPtr, int64_t page_size);
void nodes_read_filesize(const uint8_t* slot, uint32_t* epoch,
                         int64_t* modifiedAt, int64_t* fileSize, int64_t* nextPtr, int64_t page_size);

/* ---------------------------------------------------------------------------
 * NameEntry (32 bytes per slot, chains for names > 16 bytes)
 *
 * The first slot holds 8 bytes of hash + 16 bytes of name.  Chain slots
 * (for names > 16 bytes) hold 24 bytes of name data with no hash prefix.
 *
 * Planned layout:
 *   Offset  Size  Field
 *   ──────  ────  ─────
 *     0      8    nameHash  (uint64_t — FNV-1a 64-bit hash of the full name)
 *     8     16    nameData  (UTF-8 bytes, zero-padded if < 16)
 *    24      8    nextPtr   (VirtualPtr — next NameEntry slot, 0 = end)
 *
 * Current layout (will be migrated in a future task):
 *   Offset  Size  Field
 *   ──────  ────  ─────
 *     0     24    data     (UTF-8 bytes, zero-padded if < 24)
 *    24      8    nextPtr  (VirtualPtr — next NameEntry slot, 0 = end)
 * --------------------------------------------------------------------------- */

#define NAMEENTRY_OFF_HASH     0   /* uint64_t — FNV-1a hash of full name */
#define NAMEENTRY_OFF_NAME_DATA 8  /* 16 bytes of name data in first slot */
#define NAMEENTRY_OFF_NEXTPTR  24
#define NAMEENTRY_DATA_SIZE    24   /* bytes of name data per slot */

/* Internal helper: write a single NameEntry slot (used by nodes_write_name). */
void nodes_write_name_entry(uint8_t* slot, const uint8_t* data_24, int64_t nextPtr, int64_t page_size);

/* Write a name chain.  Returns number of slots written (1 or more).
   Empty name (len=0): returns 0 and sets *first_slot_vp = VFS_VPTR_NULL. */
int  nodes_write_name(Pool* pool, const char* utf8_name, int64_t* first_slot_vp);

/* Read a name chain into out_buf (null-terminated).  Returns name length
   (excluding null terminator), or 0 if first_slot_vp is null. */
int  nodes_read_name(Pool* pool, int64_t first_slot_vp, char* out_buf, int max_len);

/* ---------------------------------------------------------------------------
 * TouchedFile (32 bytes, 16 used, 16 reserved)
 *
 *   Offset  Size  Field
 *   ──────  ────  ─────
 *     0      4    epoch    (uint32 — snapshot epoch)
 *     4      4    nodeId   (uint32 — modified file's nodeId)
 *     8      8    nextPtr  (VirtualPtr — next TouchedFile, 0 = end)
 *    16     16    reserved
 * --------------------------------------------------------------------------- */

#define TOUCHEDFILE_OFF_EPOCH    0
#define TOUCHEDFILE_OFF_NODEID   4
#define TOUCHEDFILE_OFF_NEXTPTR  8

void nodes_write_touchedfile(uint8_t* slot, uint32_t epoch, uint32_t nodeId,
                             int64_t nextPtr, int64_t page_size);
void nodes_read_touchedfile(const uint8_t* slot, uint32_t* epoch,
                            uint32_t* nodeId, int64_t* nextPtr, int64_t page_size);

/* ---------------------------------------------------------------------------
 * MapperEntry (32 bytes, 16 used, 16 reserved)
 *
 *   Offset  Size  Field
 *   ──────  ────  ─────
 *     0      4    fromEpoch  (uint32)
 *     4      4    toEpoch    (uint32)
 *     8      2    flags      (uint16 — bit 0 = traversalApply)
 *    10      6    reserved
 *    16      8    nextPtr    (VirtualPtr — next MapperEntry, 0 = end)
 *    24      8    reserved
 * --------------------------------------------------------------------------- */

#define MAPPER_OFF_FROMEPOCH  0
#define MAPPER_OFF_TOEPOCH    4
#define MAPPER_OFF_FLAGS      8
#define MAPPER_OFF_NEXTPTR   16

void nodes_write_mapperentry(uint8_t* slot, uint32_t fromEpoch, uint32_t toEpoch,
                             uint16_t flags, int64_t nextPtr, int64_t page_size);
void nodes_read_mapperentry(const uint8_t* slot, uint32_t* fromEpoch,
                            uint32_t* toEpoch, uint16_t* flags, int64_t* nextPtr, int64_t page_size);

#endif /* VFS_NODES_H */
