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
 * Each entry keyed by (file, epoch).  Supports reference-counted
 * recursive locking within the same thread.
 *
 * Two-phase locking rule:
 *   Global lock (epoch=0) must be acquired before any per-epoch lock
 *   on the same file.  Per-epoch locks block other per-epoch locks
 *   for the same (file, epoch), but NOT for different epochs.
 * --------------------------------------------------------------------------- */

#define LOCK_BUCKETS 256

typedef struct LockEntry {
    int64_t           key;          /* (file << 32) | epoch  */
    pthread_mutex_t   mtx;
    pthread_t         owner;        /* current owner thread id */
    int               depth;        /* 0 = unlocked, >0 = locked N times */
    int               refcount;     /* number of references to this entry */
    struct LockEntry* next;         /* hash chain next pointer */
} LockEntry;

static LockEntry* lock_table[LOCK_BUCKETS];
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;

/* Hash: combine file and epoch into a bucket index */
static int lock_hash(int64_t file, int64_t epoch) {
    uint64_t h = (uint64_t)(file ^ (epoch << 5));
    return (int)(h % LOCK_BUCKETS);
}

/* Find or create a LockEntry for (file, epoch).  Returns with table_lock held. */
static LockEntry* lock_find_or_create(int64_t file, int64_t epoch) {
    int bkt = lock_hash(file, epoch);
    int64_t key = (file << 32) | (epoch & 0xFFFFFFFFLL);
    LockEntry* e = lock_table[bkt];
    while (e) {
        if (e->key == key) {
            e->refcount++;
            return e;
        }
        e = e->next;
    }
    /* Not found — allocate new entry */
    e = (LockEntry*)calloc(1, sizeof(LockEntry));
    if (!e) return NULL;
    e->key = key;
    pthread_mutex_init(&e->mtx, NULL);
    e->depth = 0;
    e->refcount = 1;
    e->owner = 0;
    /* Prepend to bucket */
    e->next = lock_table[bkt];
    lock_table[bkt] = e;
    return e;
}

static void lock_release_entry(LockEntry* e, int bkt) {
    e->refcount--;
    if (e->refcount <= 0) {
        /* Remove from chain */
        LockEntry** pp = &lock_table[bkt];
        while (*pp) {
            if (*pp == e) {
                *pp = e->next;
                break;
            }
            pp = &(*pp)->next;
        }
        pthread_mutex_destroy(&e->mtx);
        free(e);
    }
}

int vfs_lock(vfs_t* vfs, int64_t file, int64_t epoch) {
    if (!vfs || !vfs->ctx) return VFS_ERR_IO;

    pthread_mutex_lock(&table_lock);
    LockEntry* e = lock_find_or_create(file, epoch);
    if (!e) { pthread_mutex_unlock(&table_lock); return VFS_ERR_NOMEM; }
    pthread_mutex_unlock(&table_lock);

    pthread_t self = pthread_self();

    if (epoch == 0) {
        /* Global lock: acquire mutex, block until owned */
        if (e->depth > 0 && pthread_equal(e->owner, self)) {
            /* Recursive lock */
            e->depth++;
            return VFS_OK;
        }
        pthread_mutex_lock(&e->mtx);
        e->owner = self;
        e->depth = 1;
        return VFS_OK;
    }

    /* Per-epoch lock */
    if (e->depth > 0 && pthread_equal(e->owner, self)) {
        /* Recursive lock */
        e->depth++;
        return VFS_OK;
    }
    pthread_mutex_lock(&e->mtx);
    e->owner = self;
    e->depth = 1;
    return VFS_OK;
}

int vfs_unlock(vfs_t* vfs, int64_t file, int64_t epoch) {
    if (!vfs || !vfs->ctx) return VFS_ERR_IO;

    int bkt = lock_hash(file, epoch);
    int64_t key = (file << 32) | (epoch & 0xFFFFFFFFLL);

    pthread_mutex_lock(&table_lock);
    LockEntry* e = lock_table[bkt];
    while (e) {
        if (e->key == key) break;
        e = e->next;
    }
    if (!e) { pthread_mutex_unlock(&table_lock); return VFS_ERR_IO; }

    pthread_t self = pthread_self();
    if (e->depth == 0 || !pthread_equal(e->owner, self)) {
        pthread_mutex_unlock(&table_lock);
        return VFS_ERR_IO;  /* not locked by this thread */
    }

    e->depth--;
    if (e->depth == 0) {
        e->owner = 0;
        pthread_mutex_unlock(&e->mtx);
    }

    lock_release_entry(e, bkt);
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

vfs_t* vfs_open(const char* path, int64_t page_size) {
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

    vfs->ctx = ctx;
    return vfs;
}

void vfs_close(vfs_t* vfs) {
    if (!vfs) return;
    if (vfs->ctx) {
        /* Flush superblock to persist any pending changes */
        tree_superblock_write(vfs->ctx);
        /* Destroy page array cache if built */
        if (vfs->ctx->seg_array_fc_vp != 0)
            segment_array_destroy(&vfs->ctx->seg_array_cache);
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
