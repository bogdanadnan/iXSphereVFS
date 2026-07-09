#ifndef VFS_NODES_H
#define VFS_NODES_H

#include "pool.h"
#include "page_buf.h"

/* ---------------------------------------------------------------------------
 * Type discriminator constants (DirNode and FileNode only)
 * --------------------------------------------------------------------------- */

#define NODE_TYPE_DIR   0x01
#define NODE_TYPE_FILE  0x03

/* DirContentIndex node type values (stored at DIRCONTENTINDEX_OFF_NODETYPE) */
#define NODE_TYPE_INDEX_INTERNAL 0x02  /* internal node — navigates to children */
#define NODE_TYPE_INDEX_LEAF     0x03  /* leaf node — holds DirContentLink list */

/* Radix tree shape — 4-bit branching × 16 levels covers a 64-bit hash. */
#define RADIX_TREE_BRANCHING   16
#define RADIX_TREE_MAX_LEVELS  16

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
#define DIRNODE_OFF_INDEXHEADPTR 16  /* int64_t — VirtualPtr to first DirContentIndex at level 0 */

/* DirContentIndex — radix-tree node for directory indexing.  INTERNAL type
   (nodeType=0x02) navigates to children; LEAF type (nodeType=0x03) holds
   a list of DirContentLink entries.  32 bytes per slot. */
#define DIRCONTENTINDEX_OFF_HASHNIBBLE 0  /* uint8 — which nibble of the name hash (0..15) */
#define DIRCONTENTINDEX_OFF_NODETYPE 1    /* uint8 — 0x02=INTERNAL, 0x03=LEAF */
#define DIRCONTENTINDEX_OFF_LISTVP 8      /* int64 — VP of first entry in this level's list */
#define DIRCONTENTINDEX_OFF_NEXTVP 16     /* int64 — VP of next sibling at the same level */

/* DirContentLink — leaf-list entry pointing to a DirContent in the chain.
   32 bytes per slot. */
#define DIRCONTENTLINK_OFF_DIRCONTENTVP 8  /* int64 — VP of the actual DirContent slot */
#define DIRCONTENTLINK_OFF_NEXTVP 16       /* int64 — VP of next link in the leaf's list */

void nodes_write_dirnode(uint8_t* slot, uint32_t nodeId, int64_t headPtr,
                          int64_t indexHeadPtr, int64_t page_size);
void nodes_read_dirnode(const uint8_t* slot, uint32_t* nodeId, int64_t* headPtr, int64_t page_size);

void nodes_write_dircontentindex(uint8_t* slot, uint8_t hashNibble, uint8_t nodeType,
                                  int64_t listVP, int64_t nextVP, int64_t page_size);
void nodes_read_dircontentindex(const uint8_t* slot, uint8_t* hashNibble,
                                 uint8_t* nodeType, int64_t* listVP,
                                 int64_t* nextVP, int64_t page_size);

void nodes_write_dircontentlink(uint8_t* slot, int64_t dirContentVP,
                                int64_t nextVP, int64_t page_size);
void nodes_read_dircontentlink(const uint8_t* slot, int64_t* dirContentVP,
                               int64_t* nextVP, int64_t page_size);

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
 * NameEntry (32 bytes per slot, consisting of 24 bytes of data region
 * followed by an 8-byte nextPtr at offset 24)
 *
 * Layout:
 *   Offset  Size  Field
 *   ──────  ────  ─────
 *     0      8    nameHash  (uint64_t — hash of the full name)
 *     8     16    nameData  (UTF-8 bytes, zero-padded if < 16)
 *
 * The first slot holds 8 bytes of hash + 16 bytes of name.  Chain slots
 * (for names > 16 bytes) hold 24 bytes of name data with no hash prefix.
 * --------------------------------------------------------------------------- */

#define NAMEENTRY_OFF_HASH     0   /* uint64_t — FNV-1a hash of full name */
#define NAMEENTRY_OFF_NAME_DATA 8  /* 16 bytes of name data in first slot */
/* The first slot has 8 bytes of hash overhead, leaving 16 bytes for name
 * data.  Chain slots use the full NAMEENTRY_DATA_SIZE (24 bytes). */
#define NAMEENTRY_FIRST_SLOT_NAME_MAX 16
#define NAMEENTRY_OFF_NEXTPTR  24
/* Used for chain slots.  The first slot only holds NAMEENTRY_FIRST_SLOT_NAME_MAX
 * (16) bytes of name due to the 8-byte hash prefix. */
#define NAMEENTRY_DATA_SIZE    24   /* bytes of name data per slot */

/* Internal helper: write a single NameEntry slot (used by nodes_write_name).
 * The 24-byte data_24 buffer is memcpy'd to slot[0..23] verbatim.
 * Callers must assemble the correct layout:
 *   first slot  = [8 bytes hash][16 bytes name]
 *   chain slots = [24 bytes name] */
void nodes_write_name_entry(uint8_t* slot, const uint8_t* data_24, int64_t nextPtr, int64_t page_size);

/* Write a name chain.  Returns number of slots written (1 or more).
   Empty name (len=0): returns 0 and sets *first_slot_vp = VFS_VPTR_NULL. */
int  nodes_write_name(Pool* pool, const char* utf8_name, int64_t* first_slot_vp);

/* Read a name chain into out_buf (null-terminated).  Returns name length
   (excluding null terminator), or 0 if first_slot_vp is null. */
int  nodes_read_name(Pool* pool, int64_t first_slot_vp, char* out_buf, int max_len);

/* Read just the 8-byte FNV-1a hash from the first NameEntry slot.
 * Returns 0 if the slot cannot be resolved.  NOTE: a real hash can
 * theoretically be 0; the 0-on-error is a minor ambiguity accepted
 * for simplicity. */
uint64_t nodes_read_name_hash(Pool* pool, int64_t namePtr);

#ifdef VFS_NAME_HASH_TESTING
/* Test-only: write a name with a pre-determined hash. */
int nodes_write_name_with_hash(Pool* pool, const char* utf8_name, uint64_t hash,
                                int64_t* first_slot_vp);
#endif

/* Compute the FNV-1a 64-bit hash of a name.  This is the same hash that
 * nodes_write_name stores in the first NameEntry slot at offset 0. */
uint64_t name_hash_compute(const char* name, int len);

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
