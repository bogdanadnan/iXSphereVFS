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
 * DirNode (Workload 4.1; Phase 26 / W1b: childCount → createdAt)
 * --------------------------------------------------------------------------- */

void nodes_write_dirnode(uint8_t* slot, uint32_t nodeId, int64_t headPtr,
                          int64_t indexHeadPtr, int64_t createdAt,
                          int64_t page_size) {
    vfs_wr2_s(slot, DIRNODE_OFF_TYPE, (int16_t)NODE_TYPE_DIR, page_size);
    vfs_wr2_s(slot, DIRNODE_OFF_RSVD, 0, page_size);
    vfs_wr4_s(slot, DIRNODE_OFF_NODEID, (int32_t)nodeId, page_size);
    vfs_wr8_s(slot, DIRNODE_OFF_HEADPTR, headPtr, page_size);
    vfs_wr8_s(slot, DIRNODE_OFF_INDEXHEADPTR, indexHeadPtr, page_size);
    vfs_wr8_s(slot, DIRNODE_OFF_CTIME, createdAt, page_size);
}

void nodes_read_dirnode(const uint8_t* slot, uint32_t* nodeId, int64_t* headPtr,
                         int64_t* indexHeadPtr, int64_t* createdAt,
                         int64_t page_size) {
    *nodeId = (uint32_t)vfs_rd4_s(slot, DIRNODE_OFF_NODEID, page_size);
    *headPtr = vfs_rd8_s(slot, DIRNODE_OFF_HEADPTR, page_size);
    *indexHeadPtr = vfs_rd8_s(slot, DIRNODE_OFF_INDEXHEADPTR, page_size);
    if (createdAt) {
        *createdAt = vfs_rd8_s(slot, DIRNODE_OFF_CTIME, page_size);
    }
}

/* ---------------------------------------------------------------------------
 * DirContentIndex — radix tree internal/leaf node for directory indexing.
 * --------------------------------------------------------------------------- */

void nodes_write_dircontentindex(uint8_t* slot, uint8_t hashNibble,
                                  uint8_t nodeType, int64_t listVP,
                                  int64_t nextVP, int64_t page_size) {
    slot[DIRCONTENTINDEX_OFF_HASHNIBBLE] = hashNibble;
    slot[DIRCONTENTINDEX_OFF_NODETYPE]   = nodeType;
    memset(slot + 2, 0, 6);   /* bytes 2-7: reserved */
    vfs_wr8_s(slot, DIRCONTENTINDEX_OFF_LISTVP, listVP, page_size);
    vfs_wr8_s(slot, DIRCONTENTINDEX_OFF_NEXTVP, nextVP, page_size);
    memset(slot + 24, 0, 8);  /* bytes 24-31: reserved */
}

void nodes_read_dircontentindex(const uint8_t* slot, uint8_t* hashNibble,
                                 uint8_t* nodeType, int64_t* listVP,
                                 int64_t* nextVP, int64_t page_size) {
    *hashNibble = slot[DIRCONTENTINDEX_OFF_HASHNIBBLE];
    *nodeType   = slot[DIRCONTENTINDEX_OFF_NODETYPE];
    *listVP     = vfs_rd8_s(slot, DIRCONTENTINDEX_OFF_LISTVP, page_size);
    *nextVP     = vfs_rd8_s(slot, DIRCONTENTINDEX_OFF_NEXTVP, page_size);
}

/* ---------------------------------------------------------------------------
 * DirContentLink — leaf-list entry pointing to a DirContent in the chain.
 * --------------------------------------------------------------------------- */

void nodes_write_dircontentlink(uint8_t* slot, int64_t dirContentVP,
                                int64_t nextVP, int64_t page_size) {
    memset(slot + 0, 0, 8);   /* bytes 0-7: reserved */
    vfs_wr8_s(slot, DIRCONTENTLINK_OFF_DIRCONTENTVP, dirContentVP, page_size);
    vfs_wr8_s(slot, DIRCONTENTLINK_OFF_NEXTVP, nextVP, page_size);
    memset(slot + 24, 0, 8);  /* bytes 24-31: reserved */
}

void nodes_read_dircontentlink(const uint8_t* slot, int64_t* dirContentVP,
                               int64_t* nextVP, int64_t page_size) {
    *dirContentVP = vfs_rd8_s(slot, DIRCONTENTLINK_OFF_DIRCONTENTVP, page_size);
    *nextVP       = vfs_rd8_s(slot, DIRCONTENTLINK_OFF_NEXTVP, page_size);
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
 * FileContent (Workload 4.4; Phase 26 / W1c: Anchor-aligned layout)
 *
 * The new layout matches the unified Anchor struct: type/flags/segmentId
 * at the front (offsets 0-7), then pageRootPtr (8), nextPtr (16),
 * pageCount (24).  See nodes.h for the full diagram.
 *
 * nodes_write_filecontent / nodes_read_filecontent keep the original
 * 2-int-pointer signature (pageRootPtr + nextPtr) for source-compat
 * with the dozens of call sites; they delegate to nodes_write_anchor
 * / nodes_read_anchor internally with segmentId=0 and pageCount=0.
 * Callers that need to set segmentId or pageCount should call
 * nodes_write_anchor directly.
 * --------------------------------------------------------------------------- */

void nodes_write_filecontent(uint8_t* slot, int64_t pageRootPtr, int64_t nextPtr,
                              int64_t page_size) {
    /* Zero the type/flags/segmentId front (will be set by nodes_write_anchor
       if kind/id are passed; here we keep the convenience wrapper). */
    vfs_wr2_s(slot, FILECONTENT_OFF_TYPE,  (int16_t)ANCHOR_KIND_SEGMENT_FILE, page_size);
    vfs_wr2_s(slot, FILECONTENT_OFF_FLAGS, 0, page_size);
    vfs_wr4_s(slot, FILECONTENT_OFF_SEGMENTID, 0, page_size);
    vfs_wr8_s(slot, FILECONTENT_OFF_ROOTPTR,  pageRootPtr, page_size);
    vfs_wr8_s(slot, FILECONTENT_OFF_NEXTPTR,  nextPtr,     page_size);
    vfs_wr4_s(slot, FILECONTENT_OFF_PAGECOUNT, 0,          page_size);
    vfs_wr4_s(slot, 28, 0, page_size);  /* reserved tail */
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
 * Phase 26 / W1a: Anchor write/read (unified 32-byte layout).
 *
 * Used for the new Segment and ContentUnit types. Field layout matches
 * the ANCHOR_OFF_* macros. The existing FileNode/DirNode/FileContent/
 * PageNode continue to use their type-specific helpers until W3-W4
 * migrate them.
 * --------------------------------------------------------------------------- */

void nodes_write_anchor(uint8_t* slot, AnchorKind kind, uint32_t id,
                        int64_t headPtr, int64_t sibPtr, uint32_t count,
                        int64_t page_size) {
    vfs_wr2_s(slot, ANCHOR_OFF_TYPE, (int16_t)kind, page_size);
    vfs_wr2_s(slot, ANCHOR_OFF_FLAGS, 0, page_size);
    vfs_wr4_s(slot, ANCHOR_OFF_ID, (int32_t)id, page_size);
    vfs_wr8_s(slot, ANCHOR_OFF_HEADPTR, headPtr, page_size);
    vfs_wr8_s(slot, ANCHOR_OFF_SIBPTR, sibPtr, page_size);
    vfs_wr4_s(slot, ANCHOR_OFF_COUNT, (int32_t)count, page_size);
    vfs_wr4_s(slot, ANCHOR_OFF_RESERVED, 0, page_size);
}

void nodes_read_anchor(const uint8_t* slot, AnchorKind* kind, uint32_t* id,
                       int64_t* headPtr, int64_t* sibPtr, uint32_t* count,
                       int64_t page_size) {
    *kind     = (AnchorKind)vfs_rd2_s(slot, ANCHOR_OFF_TYPE, page_size);
    *id       = (uint32_t)vfs_rd4_s(slot, ANCHOR_OFF_ID, page_size);
    *headPtr  = vfs_rd8_s(slot, ANCHOR_OFF_HEADPTR, page_size);
    *sibPtr   = vfs_rd8_s(slot, ANCHOR_OFF_SIBPTR, page_size);
    *count    = (uint32_t)vfs_rd4_s(slot, ANCHOR_OFF_COUNT, page_size);
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
 *
 * Layout: bytes 0-7 are ALWAYS a hash (first slot only), bytes 8-23 are
 * name data.  Readers start at offset 8; chain slots have no hash prefix
 * and are read from offset 0.  The 8-byte overhead per slot is the cost
 * of consistency (no short-name special case).
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
    /* First slot: up to NAMEENTRY_FIRST_SLOT_NAME_MAX (16) bytes of name.
     * Chain slots: up to NAMEENTRY_DATA_SIZE (24) bytes each. */
    int slots_needed;
    if (total_len <= (size_t)NAMEENTRY_FIRST_SLOT_NAME_MAX) {
        slots_needed = 1;
    } else {
        slots_needed = 1 + (int)((total_len - NAMEENTRY_FIRST_SLOT_NAME_MAX
                                  + NAMEENTRY_DATA_SIZE - 1) / NAMEENTRY_DATA_SIZE);
    }

    uint64_t hash = name_hash_compute(utf8_name, (int)total_len);
    int64_t next_vp = 0;

    /* Allocate slots in reverse order (last slot first) */
    for (int i = slots_needed - 1; i >= 0; i--) {
        int64_t vp = pool_alloc(pool);
        if (vp == VFS_VPTR_NULL) {
            *first_slot_vp = VFS_VPTR_NULL;
            return 0;
        }
        /* Phase 25: by-value pool slot, pinned (we write the NameEntry
           content + release persists it to the cache). */
        PoolSlot slot_data = {0};
        pool_acquire(pool, vp, true, &slot_data);
        if (slot_data.vptr == VFS_VPTR_NULL) {
            *first_slot_vp = VFS_VPTR_NULL;
            return 0;
        }

        uint8_t buf[NAMEENTRY_DATA_SIZE] = {0};
        if (i == 0) {
            /* First slot: [8-byte hash][up to 16 bytes of name] */
            memcpy(buf, &hash, sizeof(hash));
            size_t first_chunk = total_len;
            if (first_chunk > NAMEENTRY_FIRST_SLOT_NAME_MAX)
                first_chunk = NAMEENTRY_FIRST_SLOT_NAME_MAX;
            memcpy(buf + 8, utf8_name, first_chunk);
        } else {
            /* Chain slot: [up to 24 bytes of name], starting after first slot's portion */
            size_t offset = NAMEENTRY_FIRST_SLOT_NAME_MAX + (size_t)(i - 1) * NAMEENTRY_DATA_SIZE;
            size_t chunk = total_len - offset;
            if (chunk > NAMEENTRY_DATA_SIZE)
                chunk = NAMEENTRY_DATA_SIZE;
            memcpy(buf, utf8_name + offset, chunk);
        }
        nodes_write_name_entry(slot_data.bytes, buf, next_vp, pool->sb->page_size);
        pool_release(pool, &slot_data);

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
    int slot_idx = 0;

    while (vp != VFS_VPTR_NULL && total < max_len - 1) {
        /* Phase 25: by-value pool slot (read-only).  The name is a
           stack-local copy; cache eviction doesn't affect the loop. */
        PoolSlot slot_data = {0};
        pool_acquire(pool, vp, false, &slot_data);
        if (slot_data.vptr == VFS_VPTR_NULL) break;

        size_t data_off, data_len;
        if (slot_idx == 0) {
            /* First slot: skip 8-byte hash, read up to NAMEENTRY_FIRST_SLOT_NAME_MAX */
            data_off = 8;
            data_len = NAMEENTRY_FIRST_SLOT_NAME_MAX;
        } else {
            /* Chain slot: full NAMEENTRY_DATA_SIZE bytes of name */
            data_off = 0;
            data_len = NAMEENTRY_DATA_SIZE;
        }

        for (size_t i = 0; i < data_len && total < max_len - 1; i++) {
            uint8_t byte = slot_data.bytes[data_off + i];
            if (byte == 0) {
                out_buf[total] = '\0';
                pool_release(pool, &slot_data);
                return total;
            }
            out_buf[total++] = (char)byte;
        }

        vp = vfs_rd8_s(slot_data.bytes, NAMEENTRY_OFF_NEXTPTR, pool->sb->page_size);
        pool_release(pool, &slot_data);
        slot_idx++;
    }

    out_buf[total] = '\0';
    return total;
}

uint64_t nodes_read_name_hash(Pool* pool, int64_t namePtr) {
    if (namePtr == 0) return 0;
    /* Phase 25: by-value pool slot (read-only). */
    PoolSlot slot = {0};
    pool_acquire(pool, namePtr, false, &slot);
    if (slot.vptr == VFS_VPTR_NULL) return 0;
    uint64_t h;
    memcpy(&h, slot.bytes, 8);
    return h;
}

#ifdef VFS_NAME_HASH_TESTING
/* Test-only: write a name with a pre-determined hash value (for collision tests).
 * Otherwise identical to nodes_write_name. */
int nodes_write_name_with_hash(Pool* pool, const char* utf8_name, uint64_t hash,
                                int64_t* first_slot_vp) {
    if (!utf8_name || utf8_name[0] == '\0') {
        *first_slot_vp = VFS_VPTR_NULL;
        return 0;
    }
    size_t total_len = strlen(utf8_name);
    int slots_needed;
    if (total_len <= (size_t)NAMEENTRY_FIRST_SLOT_NAME_MAX)
        slots_needed = 1;
    else
        slots_needed = 1 + (int)((total_len - NAMEENTRY_FIRST_SLOT_NAME_MAX
                                  + NAMEENTRY_DATA_SIZE - 1) / NAMEENTRY_DATA_SIZE);
    int64_t next_vp = 0;
    for (int i = slots_needed - 1; i >= 0; i--) {
        int64_t vp = pool_alloc(pool);
        if (vp == VFS_VPTR_NULL) { *first_slot_vp = VFS_VPTR_NULL; return 0; }
        /* Phase 25: by-value pool slot, pinned. */
        PoolSlot slot_data = {0};
        pool_acquire(pool, vp, true, &slot_data);
        if (slot_data.vptr == VFS_VPTR_NULL) { *first_slot_vp = VFS_VPTR_NULL; return 0; }
        uint8_t buf[NAMEENTRY_DATA_SIZE] = {0};
        if (i == 0) {
            memcpy(buf, &hash, sizeof(hash));
            size_t first_chunk = total_len;
            if (first_chunk > NAMEENTRY_FIRST_SLOT_NAME_MAX)
                first_chunk = NAMEENTRY_FIRST_SLOT_NAME_MAX;
            memcpy(buf + 8, utf8_name, first_chunk);
        } else {
            size_t offset = NAMEENTRY_FIRST_SLOT_NAME_MAX + (size_t)(i - 1) * NAMEENTRY_DATA_SIZE;
            size_t chunk = total_len - offset;
            if (chunk > NAMEENTRY_DATA_SIZE) chunk = NAMEENTRY_DATA_SIZE;
            memcpy(buf, utf8_name + offset, chunk);
        }
        nodes_write_name_entry(slot_data.bytes, buf, next_vp, pool->sb->page_size);
        pool_release(pool, &slot_data);
        if (i == 0) *first_slot_vp = vp;
        next_vp = vp;
    }
    return slots_needed;
}
#endif
