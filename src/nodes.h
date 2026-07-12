#ifndef VFS_NODES_H
#define VFS_NODES_H

#include "pool.h"
#include "page_buf.h"

/* ---------------------------------------------------------------------------
 * Phase 26 / W1: AnchorKind enum + unified Anchor layout.
 *
 * The Anchor is the canonical 32-byte layout used at all three
 * non-leaf levels (Node, Segment, ContentUnit). It is identified
 * by the `type` field at offset 0:
 *
 *   ROOT_FILE  / ROOT_DIR   — file/dir root (Node)
 *   SEGMENT_FILE / SEGMENT_DIR — mid-level group of 64 Anchors
 *   UNIT_PAGE / UNIT_SLOT  — per-content anchor (per-page or per-child)
 *
 * The legacy `NODE_TYPE_DIR` / `NODE_TYPE_FILE` (0x01 / 0x03)
 * values are still used on disk for FileNode/DirNode (their layout
 * matches the Anchor shape but with the original type values; W3+
 * migrates to ROOT_FILE / ROOT_DIR). The new values 0x10+ are
 * reserved for Segment and ContentUnit, allocated by future
 * workloads.
 * --------------------------------------------------------------------------- */

#define ANCHOR_KIND_ROOT_FILE     0x10
#define ANCHOR_KIND_ROOT_DIR      0x11
#define ANCHOR_KIND_SEGMENT_FILE  0x20
#define ANCHOR_KIND_SEGMENT_DIR   0x21
#define ANCHOR_KIND_UNIT_PAGE     0x30
#define ANCHOR_KIND_UNIT_SLOT     0x31

typedef uint16_t AnchorKind;

/* Number of Anchors per Segment.  Initial value matches the existing
   `segment_size` in `storage.c:149`.  Made a macro so future
   profiling can experiment with different values (per-type split
   is a future option; see Open question #2 in the spec). */
#define ANCHOR_UNITS_PER_SEGMENT 1024

/* Anchor offset macros (offset 0..31 within the 32-byte slot). */
#define ANCHOR_OFF_TYPE      0   /* uint16 AnchorKind */
#define ANCHOR_OFF_FLAGS     2   /* uint16 reserved */
#define ANCHOR_OFF_ID        4   /* uint32: nodeId/segmentId/pageIndex/slotId */
#define ANCHOR_OFF_HEADPTR   8   /* int64  VirtualPtr */
#define ANCHOR_OFF_SIBPTR   16   /* int64  VirtualPtr (sibling at same level) */
#define ANCHOR_OFF_COUNT    24   /* uint32 (unit count for Segment; 0 otherwise) */
#define ANCHOR_OFF_RESERVED 28   /* uint32 reserved */

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
 /* ---------------------------------------------------------------------------
 * DirNode (32 bytes, fully packed) — Phase 26 / W1b
 *
 *   Offset  Size  Field
 *   ──────  ────  ─────
 *     0      2    type         (uint16 — NODE_TYPE_DIR)
 *     2      2    reserved
 *     4      4    nodeId       (uint32)
 *     8      8    headPtr      (VirtualPtr — first DirContent)
 *    16     8    indexHeadPtr (VirtualPtr — first DirContentIndex at level 0)
 *    24     8    createdAt    (int64 — Unix timestamp, immutable)
 *
 * W1b: dropped childCount (was 4 bytes at offset 24).  The field was
 * a monotonically-incrementing upper bound on DirContent inserts
 * (live + tombstones) used to size the dedup hash_map in
 * dirchain_list.  W5 removes the dedup entirely (per-ContentUnit
 * chains are dedup'd at the structure level), so the field has no
 * remaining purpose.  Replaced with createdAt to bring DirNode in
 * line with FileNode (which has had createdAt at offset 24 since
 * its original definition).
 * --------------------------------------------------------------------------- */

#define DIRNODE_OFF_TYPE            0
#define DIRNODE_OFF_RSVD            2
#define DIRNODE_OFF_NODEID          4
#define DIRNODE_OFF_HEADPTR         8
#define DIRNODE_OFF_INDEXHEADPTR   16
#define DIRNODE_OFF_CTIME          24

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
                          int64_t indexHeadPtr, int64_t createdAt,
                          int64_t page_size);
void nodes_read_dirnode(const uint8_t* slot, uint32_t* nodeId, int64_t* headPtr,
                         int64_t* indexHeadPtr, int64_t* createdAt,
                         int64_t page_size);

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
 * FileContent (32 bytes) — Phase 26 / W1c: now matches the Anchor shape.
 *
 *   Offset  Size  Field
 *   ──────  ────  ─────
 *     0      2    type        (uint16 — ANCHOR_KIND_SEGMENT_FILE = 0x20)
 *     2      2    flags       (uint16 reserved)
 *     4      4    segmentId   (uint32 — unique per VFS instance)
 *     8      8    pageRootPtr (VirtualPtr — first PageNode in segment)
 *    16      8    nextPtr     (VirtualPtr — next FileContent, 0 = end)
 *    24      4    pageCount   (uint32 — number of PageNodes in this segment)
 *    28      4    reserved
 *
 * W1c: layout aligned with the unified Anchor struct (see ANCHOR_OFF_*
 * macros).  pageRootPtr moved from offset 0 → 8, nextPtr from 8 → 16,
 * pageCount from 16 → 24.  The legacy FILECONTENT_OFF_* macros below
 * are kept as aliases for now to minimize churn at the call sites;
 * they map to the new offsets.  The functions nodes_write_filecontent
 * / nodes_read_filecontent now write/read the new layout (and zero the
 * new type/flags/segmentId fields — call sites that need a real
 * segmentId should use nodes_write_anchor directly with
 * ANCHOR_KIND_SEGMENT_FILE).
 * --------------------------------------------------------------------------- */

#define FILECONTENT_OFF_TYPE         0  /* was reserved; now ANCHOR_KIND_SEGMENT_FILE */
#define FILECONTENT_OFF_FLAGS        2  /* was reserved; now 0 */
#define FILECONTENT_OFF_SEGMENTID    4  /* new in W1c */
#define FILECONTENT_OFF_ROOTPTR      8  /* was 0 */
#define FILECONTENT_OFF_NEXTPTR     16  /* was 8 */
#define FILECONTENT_OFF_PAGECOUNT   24  /* was 16 */

void nodes_write_filecontent(uint8_t* slot, int64_t pageRootPtr, int64_t nextPtr, int64_t page_size);
void nodes_read_filecontent(const uint8_t* slot, int64_t* pageRootPtr, int64_t* nextPtr, int64_t page_size);

/* ---------------------------------------------------------------------------
 * PageNode (32 bytes) — Phase 26 / W1d: now matches the Anchor shape.
 * (Renamed conceptually to "ContentUnit" in W2; the struct/helpers keep
 * the PageNode name for source-compat through W5.)
 *
 *   Offset  Size  Field
 *   ──────  ────  ─────
 *     0      2    type        (uint16 — ANCHOR_KIND_UNIT_PAGE = 0x30)
 *     2      2    flags       (uint16 reserved)
 *     4      4    pageIndex   (uint32 — logical page index within segment)
 *     8      8    versionRoot (VirtualPtr — first VersionPage, 0 = unwritten)
 *    16      8    nextPtr     (VirtualPtr — next PageNode, 0 = end)
 *    24      4    reserved
 *    28      4    reserved
 *
 * W1d: layout aligned with the unified Anchor struct.  pageIndex
 * moved from offset 16 → 4 (the Anchor `id` field), versionRoot from
 * 0 → 8 (Anchor `headPtr`), nextPtr from 8 → 16 (Anchor `sibPtr`).
 * The legacy PAGENODE_OFF_* macros below are kept as aliases for now
 * to minimize churn at the call sites; they map to the new offsets.
 *
 * Note: at the ContentUnit level, the `id` field stores the pageIndex
 * (an integer that uniquely identifies the page within a segment).
 * The dir-side equivalent (W1e) uses the same `id` field to store
 * the slotId, giving a uniform 32-byte ContentUnit layout for both
 * file and dir chains.
 * --------------------------------------------------------------------------- */

#define PAGENODE_OFF_TYPE         0  /* new in W1d; ANCHOR_KIND_UNIT_PAGE */
#define PAGENODE_OFF_FLAGS        2  /* new in W1d; reserved */
#define PAGENODE_OFF_PAGEINDEX    4  /* was 16 */
#define PAGENODE_OFF_VERSIONROOT  8  /* was 0 */
#define PAGENODE_OFF_NEXTPTR     16  /* was 8 */

void nodes_write_pagenode(uint8_t* slot, int64_t versionRootPtr, int64_t nextPtr,
                          uint32_t page_index, int64_t page_size);
void nodes_read_pagenode(const uint8_t* slot, int64_t* versionRootPtr,
                         int64_t* nextPtr, uint32_t* page_index, int64_t page_size);

/* ---------------------------------------------------------------------------
 * Phase 26 / W1e: DirSegment and SlotNode — dir-side mirror of Segment
 * (FileContent) and ContentUnit (PageNode).
 *
 * Both types share the unified 32-byte Anchor layout (type/flags/id at
 * 0-7, headPtr at 8, sibPtr at 16, count at 24, reserved at 28), so
 * they use the same write/read helpers as the file-side types — just
 * with different AnchorKind values.  No new write/read functions are
 * needed; call sites use nodes_write_anchor / nodes_read_anchor
 * directly with the right kind.
 *
 *   DirSegment:  ANCHOR_KIND_SEGMENT_DIR (0x21)
 *     - id       = segmentId (per-VFS unique)
 *     - headPtr  = first SlotNode in the segment
 *     - sibPtr   = next DirSegment in the chain
 *     - count    = number of live SlotNode entries in this segment
 *
 *   SlotNode:    ANCHOR_KIND_UNIT_SLOT (0x31)
 *     - id       = childNodeId of the entry (per-ContentUnit id source
 *                  is childNodeId directly — no separate slot-ID space;
 *                  see spec §4.4 and the W1 design notes)
 *     - headPtr  = first DirContent in this slot's chain
 *     - sibPtr   = next SlotNode in the same segment (sibling slot)
 *     - count    = 0 (per-slot entry count is tracked on the
 *                  DirContent chain itself, not here)
 *
 * Allocation sites for these types are added in W5 (dir segment
 * population); this W1e header work just establishes the discriminator
 * values and the per-type offset macros so call sites in W5 can be
 * written without further header changes.
 *
 * The offset macros below are aliases of the ANCHOR_OFF_* macros
 * (the layout is identical across all Anchor-typed slots).  They're
 * provided for symmetry with the file-side PAGENODE_OFF_* / FILECONTENT_OFF_*
 * macros — making the W5 dir-chain code read the same as the W3 file
 * code, which is the central goal of W1.
 * --------------------------------------------------------------------------- */

#define DIRSEGMENT_OFF_TYPE         ANCHOR_OFF_TYPE         /* ANCHOR_KIND_SEGMENT_DIR */
#define DIRSEGMENT_OFF_FLAGS        ANCHOR_OFF_FLAGS
#define DIRSEGMENT_OFF_SEGMENTID    ANCHOR_OFF_ID
#define DIRSEGMENT_OFF_HEADPTR      ANCHOR_OFF_HEADPTR      /* first SlotNode in segment */
#define DIRSEGMENT_OFF_NEXTPTR      ANCHOR_OFF_SIBPTR       /* next DirSegment */
#define DIRSEGMENT_OFF_SLOTCOUNT    ANCHOR_OFF_COUNT        /* live SlotNode entries */

#define SLOTNODE_OFF_TYPE           ANCHOR_OFF_TYPE         /* ANCHOR_KIND_UNIT_SLOT */
#define SLOTNODE_OFF_FLAGS          ANCHOR_OFF_FLAGS
#define SLOTNODE_OFF_CHILDNODEID    ANCHOR_OFF_ID           /* id = childNodeId */
#define SLOTNODE_OFF_HEADPTR        ANCHOR_OFF_HEADPTR      /* first DirContent in slot */
#define SLOTNODE_OFF_NEXTPTR        ANCHOR_OFF_SIBPTR       /* next SlotNode in segment */

/* ---------------------------------------------------------------------------
 * Phase 26 / W1a: Anchor write/read helpers (additive).
 *
 * These helpers operate on the unified 32-byte Anchor layout. Used for
 * the new Segment and ContentUnit types allocated in W1d; the existing
 * FileNode/DirNode/FileContent/PageNode continue to use their
 * type-specific helpers until W3-W4 migrate them.
 * --------------------------------------------------------------------------- */

void nodes_write_anchor(uint8_t* slot, AnchorKind kind, uint32_t id,
                        int64_t headPtr, int64_t sibPtr, uint32_t count,
                        int64_t page_size);
void nodes_read_anchor(const uint8_t* slot, AnchorKind* kind, uint32_t* id,
                       int64_t* headPtr, int64_t* sibPtr, uint32_t* count,
                       int64_t page_size);

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
