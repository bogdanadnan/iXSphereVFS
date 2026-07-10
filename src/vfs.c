#include "ixsphere/vfs.h"
#include "platform.h"
#include "ixsphere/vfs_internal.h"
#include "tree.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ---------------------------------------------------------------------------
 * Per-file locking subsystem
 *
 * Hash table with fixed 256 buckets, chained linked list.
 * Each entry keyed by file nodeId only.  Global lock (epoch=0) is tracked
 * via a boolean flag; per-epoch locks use a counter.  A condition variable
 * coordinates global vs per-epoch: when global is held, per-epoch lockers
 * wait; when per-epoch locks are active, global lockers wait until they
 * drain.
 * --------------------------------------------------------------------------- */

#define LOCK_BUCKETS 256

typedef struct FileLockState {
    int64_t           nodeId;       /* file nodeId (lock key) */
    pthread_mutex_t   mtx;
    pthread_cond_t    cv;
    pthread_t         global_owner; /* thread holding the global lock, 0 if none */
    int               global_depth; /* recursive count for global lock */
    int               global_held;  /* 1 if global lock is active */
    int               global_pending; /* threads waiting for global lock */
    int               epoch_count;  /* number of active per-epoch locks */
    int               refcount;     /* number of references to this entry */
    struct FileLockState* next;     /* hash chain next pointer */
} FileLockState;

static FileLockState* lock_table[LOCK_BUCKETS];
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;

/* Hash: file nodeId */
static int lock_hash(int64_t nodeId) {
    uint64_t h = (uint64_t)nodeId;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    return (int)(h % LOCK_BUCKETS);
}

/* Find or create a FileLockState for nodeId.  Caller must hold table_lock. */
static FileLockState* filelock_find_or_create(int64_t nodeId) {
    int bkt = lock_hash(nodeId);
    FileLockState* e = lock_table[bkt];
    while (e) {
        if (e->nodeId == nodeId) {
            e->refcount++;
            return e;
        }
        e = e->next;
    }
    e = (FileLockState*)calloc(1, sizeof(FileLockState));
    if (!e) return NULL;
    e->nodeId = nodeId;
    pthread_mutex_init(&e->mtx, NULL);
    pthread_cond_init(&e->cv, NULL);
    e->refcount = 1;
    e->global_owner = 0;
    e->global_depth = 0;
    e->global_held = 0;
    e->global_pending = 0;
    e->epoch_count = 0;
    e->next = lock_table[bkt];
    lock_table[bkt] = e;
    return e;
}

static void filelock_release(FileLockState* e, int bkt) {
    e->refcount--;
    if (e->refcount <= 0) {
        FileLockState** pp = &lock_table[bkt];
        while (*pp) {
            if (*pp == e) {
                *pp = e->next;
                break;
            }
            pp = &(*pp)->next;
        }
        pthread_mutex_destroy(&e->mtx);
        pthread_cond_destroy(&e->cv);
        free(e);
    }
}

int vfs_lock(vfs_t* vfs, int64_t file, int64_t epoch) {
    if (!vfs || !vfs->ctx) return VFS_ERR_IO;

    pthread_mutex_lock(&table_lock);
    FileLockState* fls = filelock_find_or_create(file);
    if (!fls) {
        pthread_mutex_unlock(&table_lock);
        vfs->ctx->last_error = VFS_ERR_NOMEM;
        return VFS_ERR_NOMEM;
    }
    pthread_mutex_unlock(&table_lock);

    pthread_t self = pthread_self();

    if (epoch == 0) {
        /* Global lock: announce intent, wait until per-epoch locks drain */
        pthread_mutex_lock(&fls->mtx);
        fls->global_pending = 1;

        if (fls->global_depth > 0 && pthread_equal(fls->global_owner, self)) {
            fls->global_depth++;
            fls->global_pending = 0;
            pthread_mutex_unlock(&fls->mtx);
            return VFS_OK;
        }

        while (fls->epoch_count > 0)
            pthread_cond_wait(&fls->cv, &fls->mtx);

        fls->global_owner = self;
        fls->global_depth = 1;
        fls->global_held = 1;
        fls->global_pending = 0;
        pthread_mutex_unlock(&fls->mtx);
        return VFS_OK;
    }

    /* Per-epoch lock: wait until global lock is released and no global
       lock is pending (another thread may be waiting to acquire it). */
    pthread_mutex_lock(&fls->mtx);

    while (fls->global_held || fls->global_pending)
        pthread_cond_wait(&fls->cv, &fls->mtx);

    fls->epoch_count++;
    pthread_mutex_unlock(&fls->mtx);
    return VFS_OK;
}

int vfs_unlock(vfs_t* vfs, int64_t file, int64_t epoch) {
    if (!vfs || !vfs->ctx) return VFS_ERR_IO;

    int bkt = lock_hash(file);

    pthread_mutex_lock(&table_lock);
    FileLockState* fls = lock_table[bkt];
    while (fls) {
        if (fls->nodeId == file) break;
        fls = fls->next;
    }
    if (!fls) {
        pthread_mutex_unlock(&table_lock);
        vfs->ctx->last_error = VFS_ERR_IO;
        return VFS_ERR_IO;
    }

    pthread_t self = pthread_self();

    if (epoch == 0) {
        /* Global unlock */
        pthread_mutex_lock(&fls->mtx);
        if (fls->global_depth == 0 || !pthread_equal(fls->global_owner, self)) {
            pthread_mutex_unlock(&fls->mtx);
            pthread_mutex_unlock(&table_lock);
            vfs->ctx->last_error = VFS_ERR_IO;
            return VFS_ERR_IO;
        }
        fls->global_depth--;
        if (fls->global_depth == 0) {
            fls->global_owner = 0;
            fls->global_held = 0;
            pthread_cond_broadcast(&fls->cv);
        }
        pthread_mutex_unlock(&fls->mtx);
    } else {
        /* Per-epoch unlock */
        pthread_mutex_lock(&fls->mtx);
        if (fls->epoch_count == 0) {
            pthread_mutex_unlock(&fls->mtx);
            pthread_mutex_unlock(&table_lock);
            vfs->ctx->last_error = VFS_ERR_IO;
            return VFS_ERR_IO;
        }
        fls->epoch_count--;
        if (fls->epoch_count == 0 && fls->global_pending > 0)
            pthread_cond_signal(&fls->cv);
        pthread_mutex_unlock(&fls->mtx);
    }

    filelock_release(fls, bkt);
    pthread_mutex_unlock(&table_lock);
    return VFS_OK;
}

const char* vfs_error_string(vfs_error_t err) {
    switch (err) {
        case VFS_OK:            return "OK";
        case VFS_ERR_IO:        return "I/O error";
        case VFS_ERR_NOTFOUND:  return "Not found";
        case VFS_ERR_EXISTS:    return "Already exists";
        case VFS_ERR_NOTDIR:    return "Not a directory";
        case VFS_ERR_NOTEMPTY:  return "Directory not empty";
        case VFS_ERR_CONFLICT:  return "Conflict";
        case VFS_ERR_FULL:      return "No space left";
        case VFS_ERR_NOMEM:     return "Out of memory";
        case VFS_ERR_EPOCH:     return "Epoch not writable";
        default:                return "Unknown error";
    }
}

vfs_t* vfs_mount(const char* path, int64_t page_size) {
    vfs_t* vfs = (vfs_t*)calloc(1, sizeof(vfs_t));
    if (!vfs) return NULL;

    TreeContext* ctx = (TreeContext*)calloc(1, sizeof(TreeContext));
    if (!ctx) {
        free(vfs);
        return NULL;
    }

    ctx->sb = storage_open(path, page_size);
    if (!ctx->sb) {
        free(ctx);
        free(vfs);
        return NULL;
    }

    ctx->page_size = ctx->sb->page_size;

    /* Initialize pool allocator — list_head points into TreeContext */
    pool_init(&ctx->pool, ctx->sb, &ctx->pool_list_head_value);

    /* Initialize epoch mapper */
    mapper_init(&ctx->mapper, &ctx->pool, &ctx->epochMapperPtr);

    /* Bootstrap or reinitialize the tree.
       tree_bootstrap_superblock handles both fresh and reopen internally. */
    int err = tree_bootstrap_superblock(ctx);
    if (err != VFS_OK) {
        storage_close(ctx->sb);
        free(ctx);
        free(vfs);
        return NULL;
    }

    /* Initialize the in-memory mapper table snapshot */
    err = mapper_table_init(&ctx->mapper_table, &ctx->pool, &ctx->epochMapperPtr);
    if (err != VFS_OK) {
        storage_close(ctx->sb);
        free(ctx);
        free(vfs);
        return NULL;
    }

    vfs->ctx = ctx;
    return vfs;
}

void vfs_unmount(vfs_t* vfs) {
    if (!vfs) return;
    if (vfs->ctx) {
        /* Flush superblock to persist any pending changes */
        tree_superblock_write(vfs->ctx);
        /* Destroy page array cache if built */
        /* Free in-memory mapper table entries */
        mapper_table_destroy(&vfs->ctx->mapper_table);
        storage_close(vfs->ctx->sb);
        free(vfs->ctx);
    }
    free(vfs);
}

int vfs_flush(vfs_t* vfs) {
    if (!vfs || !vfs->ctx) return VFS_ERR_IO;
    storage_flush(vfs->ctx->sb, -1);
    return VFS_OK;
}

vfs_error_t vfs_last_error(vfs_t* vfs) {
    if (!vfs || !vfs->ctx) return VFS_ERR_IO;
    return vfs->ctx->last_error;
}

int64_t vfs_root(vfs_t* vfs) {
    if (!vfs || !vfs->ctx) return 0;
    return vfs->ctx->rootNodeOffset;
}

int64_t vfs_current_epoch(vfs_t* vfs) {
    if (!vfs || !vfs->ctx) return 0;
    return vfs->ctx->currentEpoch;
}

int vfs_node_type(vfs_t* vfs, int64_t vp) {
    if (!vfs || !vfs->ctx || vp <= 0) return 0;
    uint8_t* slot = pool_resolve_ro(&vfs->ctx->pool, vp);
    if (!slot) return 0;
    int16_t type = (int16_t)vfs_rd2_s(slot, 0, vfs->ctx->page_size);
    if (type == (int16_t)NODE_TYPE_DIR)  return 0x01;
    if (type == (int16_t)NODE_TYPE_FILE) return 0x03;
    return 0;
}

/* ---------------------------------------------------------------------------
 * vfs_readdir_alloc — heap-allocated variant of vfs_readdir.
 *
 * Walks the DirContent chain exactly once and produces a vfs_dirent_t[]
 * sized to the actual entry count (no cap, no doubling, no caller
 * guess).  Used by FUSE dir caching where the full directory listing
 * must be retrieved to support cursor-based readdir with offset.
 *
 * On success returns VFS_OK and sets *out_entries / *out_count.
 * On error returns negative error code and sets them to NULL / 0.
 * --------------------------------------------------------------------------- */

int vfs_readdir_alloc(vfs_t* vfs, int64_t dir,
                      vfs_dirent_t** out_entries, int* out_count,
                      int64_t epoch) {
    if (!vfs || !vfs->ctx || !out_entries || !out_count) return VFS_ERR_IO;
    return dirchain_list_all(vfs->ctx, dir, epoch, out_entries, out_count);
}

/* ---------------------------------------------------------------------------
 * vfs_free_dirents — free a buffer returned by vfs_readdir_alloc.
 *
 * Wrapper around free() so the call site reads self-documentingly
 * (vfs_free_dirents vs plain free) and so future changes (e.g.,
 * pool allocator for VFS-internal memory) don't break callers.
 * Safe on NULL.
 * --------------------------------------------------------------------------- */

void vfs_free_dirents(vfs_dirent_t* entries) {
    free(entries);
}
