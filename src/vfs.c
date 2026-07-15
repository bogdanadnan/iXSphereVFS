#include "ixsphere/vfs.h"
#include "platform.h"
#include "ixsphere/vfs_internal.h"
#include "tree.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ---------------------------------------------------------------------------
 * Per-file locking subsystem (Phase 26 / W0)
 *
 * Per-vfs_t lock table (hash, 256 buckets, chained list).  Each entry
 * keyed by an opaque int64_t (typically a VirtualPtr, but nodeId
 * still works for legacy callers).  Global lock (epoch=0) is tracked
 * via a flag; per-epoch locks are tracked per-epoch (small array of
 * {epoch, count} pairs, max 64 distinct epochs per key).
 *
 * Semantics:
 *   - global (epoch=0) is exclusive: drains all per-epoch holders.
 *   - per-epoch (epoch!=0) is per-epoch-keyed: different-epoch
 *     holders proceed in parallel; same-epoch second acquirer
 *     blocks until the first releases.
 *
 * Lock contract:
 *   - Key is an opaque int64_t (callers pick a stable identifier;
 *     vfs_lock does not interpret the value).
 *   - Per-epoch mode (epoch!=0) provides same-epoch exclusion
 *     but allows different-epoch writes to proceed in parallel.
 *   - Global mode (epoch==0) is fully exclusive.
 * --------------------------------------------------------------------------- */

#define LOCK_BUCKETS 256
#define MAX_PER_EPOCH_HOLDERS 64   /* max distinct epochs per key (sanity bound) */

typedef struct FileLockState {
    int64_t           nodeId;       /* lock key (opaque int64_t to the lock subsystem) */
    pthread_mutex_t   mtx;
    pthread_cond_t    cv;
    pthread_t         global_owner; /* thread holding the global lock, 0 if none */
    int               global_depth; /* recursive count for global lock */
    int               global_held;  /* 1 if global lock is active */
    int               global_pending; /* threads waiting for global lock */
    /* Per-epoch holder tracking (R1 fix) */
    int64_t           epoch_keys[MAX_PER_EPOCH_HOLDERS];
    int               epoch_counts[MAX_PER_EPOCH_HOLDERS];
    int               num_epochs;          /* number of distinct epochs with holders */
    int               total_epoch_holders; /* sum of all epoch_counts (for the drain check) */
    int               refcount;     /* number of references to this entry */
    struct FileLockState* next;     /* hash chain next pointer */
} FileLockState;

typedef struct LockTable {
    FileLockState* buckets[LOCK_BUCKETS];
    pthread_mutex_t table_lock;
} LockTable;

static LockTable* lock_table_create(void) {
    LockTable* t = (LockTable*)calloc(1, sizeof(LockTable));
    if (!t) return NULL;
    if (pthread_mutex_init(&t->table_lock, NULL) != 0) {
        free(t);
        return NULL;
    }
    return t;
}

static void lock_table_destroy(LockTable* t) {
    if (!t) return;
    for (int i = 0; i < LOCK_BUCKETS; i++) {
        FileLockState* e = t->buckets[i];
        while (e) {
            FileLockState* next = e->next;
            pthread_mutex_destroy(&e->mtx);
            pthread_cond_destroy(&e->cv);
            free(e);
            e = next;
        }
    }
    pthread_mutex_destroy(&t->table_lock);
    free(t);
}

void vfs_lock_destroy(vfs_t* vfs) {
    if (!vfs) return;
    if (vfs->lock_table) {
        lock_table_destroy(vfs->lock_table);
        vfs->lock_table = NULL;
    }
}

/* Hash: opaque key int64_t */
static int lock_hash(int64_t key) {
    uint64_t h = (uint64_t)key;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    return (int)(h % LOCK_BUCKETS);
}

/* Find or create a FileLockState for `key` in the given LockTable.
   Caller must hold t->table_lock. */
static FileLockState* filelock_find_or_create(LockTable* t, int64_t key) {
    int bkt = lock_hash(key);
    FileLockState* e = t->buckets[bkt];
    while (e) {
        if (e->nodeId == key) {
            e->refcount++;
            return e;
        }
        e = e->next;
    }
    e = (FileLockState*)calloc(1, sizeof(FileLockState));
    if (!e) return NULL;
    e->nodeId = key;
    pthread_mutex_init(&e->mtx, NULL);
    pthread_cond_init(&e->cv, NULL);
    e->refcount = 1;
    e->global_owner = 0;
    e->global_depth = 0;
    e->global_held = 0;
    e->global_pending = 0;
    e->num_epochs = 0;
    e->total_epoch_holders = 0;
    e->next = t->buckets[bkt];
    t->buckets[bkt] = e;
    return e;
}

/* Look up a FileLockState (no create).  Caller must hold t->table_lock. */
static FileLockState* filelock_lookup(LockTable* t, int64_t key) {
    int bkt = lock_hash(key);
    FileLockState* e = t->buckets[bkt];
    while (e) {
        if (e->nodeId == key) return e;
        e = e->next;
    }
    return NULL;
}

static void filelock_release(LockTable* t, FileLockState* e, int bkt) {
    e->refcount--;
    if (e->refcount <= 0) {
        FileLockState** pp = &t->buckets[bkt];
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

/* Per-epoch helpers (called with fls->mtx held).  Returns 0 on success,
   -1 on overflow. */
static int per_epoch_add(FileLockState* fls, int64_t epoch) {
    /* If epoch already has holders, increment. */
    for (int i = 0; i < fls->num_epochs; i++) {
        if (fls->epoch_keys[i] == epoch) {
            fls->epoch_counts[i]++;
            fls->total_epoch_holders++;
            return 0;
        }
    }
    /* New epoch; needs a slot. */
    if (fls->num_epochs >= MAX_PER_EPOCH_HOLDERS) {
        return -1;  /* overflow — caller treats as error */
    }
    fls->epoch_keys[fls->num_epochs] = epoch;
    fls->epoch_counts[fls->num_epochs] = 1;
    fls->num_epochs++;
    fls->total_epoch_holders++;
    return 0;
}

static int per_epoch_remove(FileLockState* fls, int64_t epoch) {
    for (int i = 0; i < fls->num_epochs; i++) {
        if (fls->epoch_keys[i] == epoch) {
            fls->epoch_counts[i]--;
            fls->total_epoch_holders--;
            if (fls->epoch_counts[i] == 0) {
                /* Compact: shift left. */
                for (int j = i; j < fls->num_epochs - 1; j++) {
                    fls->epoch_keys[j] = fls->epoch_keys[j+1];
                    fls->epoch_counts[j] = fls->epoch_counts[j+1];
                }
                fls->num_epochs--;
            }
            return 0;
        }
    }
    return -1;  /* not held — caller treats as error */
}

static int per_epoch_has_holders(FileLockState* fls, int64_t epoch) {
    for (int i = 0; i < fls->num_epochs; i++) {
        if (fls->epoch_keys[i] == epoch) return 1;
    }
    return 0;
}

int vfs_lock(vfs_t* vfs, int64_t file, int64_t epoch) {
    if (!vfs || !vfs->ctx) return VFS_ERR_IO;
    if (!vfs->lock_table) {
        vfs->ctx->last_error = VFS_ERR_IO;
        return VFS_ERR_IO;
    }

    LockTable* t = vfs->lock_table;

    pthread_mutex_lock(&t->table_lock);
    FileLockState* fls = filelock_find_or_create(t, file);
    if (!fls) {
        pthread_mutex_unlock(&t->table_lock);
        vfs->ctx->last_error = VFS_ERR_NOMEM;
        return VFS_ERR_NOMEM;
    }
    pthread_mutex_unlock(&t->table_lock);

    pthread_t self = pthread_self();

    if (epoch == 0) {
        /* Global lock: announce intent, wait until per-epoch holders drain */
        pthread_mutex_lock(&fls->mtx);
        fls->global_pending = 1;

        if (fls->global_depth > 0 && pthread_equal(fls->global_owner, self)) {
            fls->global_depth++;
            fls->global_pending = 0;
            pthread_mutex_unlock(&fls->mtx);
            return VFS_OK;
        }

        /* Wait for BOTH: any per-epoch holders to drain AND any
         * existing global holder to release.  The original code only
         * checked total_epoch_holders, missing the global_held check
         * — two concurrent epoch==0 callers would both pass through,
         * defeating the exclusivity the global lock is supposed to
         * provide. */
        while (fls->global_held || fls->total_epoch_holders > 0)
            pthread_cond_wait(&fls->cv, &fls->mtx);

        fls->global_owner = self;
        fls->global_depth = 1;
        fls->global_held = 1;
        fls->global_pending = 0;
        pthread_mutex_unlock(&fls->mtx);
        return VFS_OK;
    }

    /* Per-epoch lock: wait for global to be released, then for any
       same-epoch holders to release.  Different-epoch holders don't
       block us. */
    pthread_mutex_lock(&fls->mtx);

    while (fls->global_held || fls->global_pending)
        pthread_cond_wait(&fls->cv, &fls->mtx);

    while (per_epoch_has_holders(fls, epoch))
        pthread_cond_wait(&fls->cv, &fls->mtx);

    if (per_epoch_add(fls, epoch) != 0) {
        /* Overflow — too many distinct epochs at this key. */
        pthread_mutex_unlock(&fls->mtx);
        vfs->ctx->last_error = VFS_ERR_NOMEM;
        return VFS_ERR_NOMEM;
    }
    pthread_mutex_unlock(&fls->mtx);
    return VFS_OK;
}

int vfs_unlock(vfs_t* vfs, int64_t file, int64_t epoch) {
    if (!vfs || !vfs->ctx) return VFS_ERR_IO;
    if (!vfs->lock_table) {
        vfs->ctx->last_error = VFS_ERR_IO;
        return VFS_ERR_IO;
    }

    LockTable* t = vfs->lock_table;
    int bkt = lock_hash(file);

    pthread_mutex_lock(&t->table_lock);
    FileLockState* fls = filelock_lookup(t, file);
    if (!fls) {
        pthread_mutex_unlock(&t->table_lock);
        vfs->ctx->last_error = VFS_ERR_IO;
        return VFS_ERR_IO;
    }

    pthread_t self = pthread_self();

    if (epoch == 0) {
        /* Global unlock */
        pthread_mutex_lock(&fls->mtx);
        if (fls->global_depth == 0 || !pthread_equal(fls->global_owner, self)) {
            pthread_mutex_unlock(&fls->mtx);
            pthread_mutex_unlock(&t->table_lock);
            vfs->ctx->last_error = VFS_ERR_IO;
            return VFS_ERR_IO;
        }
        fls->global_depth--;
        if (fls->global_depth == 0) {
            fls->global_owner = 0;
            fls->global_held = 0;
            /* Wake any per-epoch waiters; they re-check their conditions. */
            pthread_cond_broadcast(&fls->cv);
        }
        pthread_mutex_unlock(&fls->mtx);
    } else {
        /* Per-epoch unlock */
        pthread_mutex_lock(&fls->mtx);
        if (per_epoch_remove(fls, epoch) != 0) {
            pthread_mutex_unlock(&fls->mtx);
            pthread_mutex_unlock(&t->table_lock);
            vfs->ctx->last_error = VFS_ERR_IO;
            return VFS_ERR_IO;
        }
        /* Wake any waiters: per-epoch waiters re-check same-epoch,
           global waiters re-check total_epoch_holders. */
        pthread_cond_broadcast(&fls->cv);
        pthread_mutex_unlock(&fls->mtx);
    }

    filelock_release(t, fls, bkt);
    pthread_mutex_unlock(&t->table_lock);
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
    vfs->lock_table = lock_table_create();
    if (!vfs->lock_table) {
        mapper_table_destroy(&ctx->mapper_table);
        storage_close(ctx->sb);
        free(ctx);
        free(vfs);
        return NULL;
    }
    return vfs;
}

void vfs_unmount(vfs_t* vfs) {
    if (!vfs) return;
    /* Phase 26 / W0: destroy the per-vfs_t lock table (fixes H4) */
    vfs_lock_destroy(vfs);
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
    /* Phase 25: by-value pool slot (read-only). */
    PoolSlot slot = {0};
    pool_acquire(&vfs->ctx->pool, vp, false, &slot);
    if (slot.vptr == VFS_VPTR_NULL) return 0;
    int16_t type = (int16_t)vfs_rd2_s(slot.bytes, 0, vfs->ctx->page_size);
    if (type == (int16_t)NODE_TYPE_DIR)  return 0x01;
    if (type == (int16_t)NODE_TYPE_FILE) return 0x03;
    return 0;
}

/* ---------------------------------------------------------------------------
 * vfs_readdir — list directory contents.
 *
 * Walks the DirContent chain exactly once and produces a vfs_dirent_t[]
 * sized to the actual entry count (no cap, no doubling, no caller
 * guess).  Used by FUSE dir caching where the full directory listing
 * must be retrieved to support cursor-based readdir with offset.
 *
 * On success returns VFS_OK and sets *out_entries / *out_count.
 * On error returns negative error code and sets them to NULL / 0.
 * --------------------------------------------------------------------------- */

int vfs_readdir(vfs_t* vfs, int64_t dir,
                vfs_dirent_t** out_entries, int* out_count,
                int64_t epoch) {
    if (!vfs || !vfs->ctx || !out_entries || !out_count) return VFS_ERR_IO;
    return dirchain_list(vfs->ctx, dir, epoch, out_entries, out_count);
}

/* ---------------------------------------------------------------------------
 * vfs_free_dirents — free a buffer returned by vfs_readdir.
 *
 * Wrapper around free() so the call site reads self-documentingly
 * (vfs_free_dirents vs plain free) and so future changes (e.g.,
 * pool allocator for VFS-internal memory) don't break callers.
 * Safe on NULL.
 * --------------------------------------------------------------------------- */

void vfs_free_dirents(vfs_dirent_t* entries) {
    free(entries);
}
