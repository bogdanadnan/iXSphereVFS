/* fuse_vfs.h — shared state and declarations for the FUSE filesystem layer.
 *
 * Includes ONLY the public ixsphere/vfs.h header — never vfs_internal.h.
 * This ensures the FUSE layer can be built against only the public API
 * surface, keeping internal struct details encapsulated.
 */

#ifndef VFS_FUSE_VFS_H
#define VFS_FUSE_VFS_H

#include "ixsphere/vfs.h"
#include "fuse_dir_cache.h"
#include <stdbool.h>
#include <pthread.h>

#ifdef FUSE3_FOUND
/* struct fuse_darwin_attr and the Darwin fill_dir typedef are referenced
   by the high-level API callbacks.  Including only the public highlevel
   header here so the public API surface stays limited to the FUSE
   library layer. */
#include <fuse.h>
#endif

/* Per-mount state passed to FUSE callbacks via private_data.
   All fields are owned by the caller; the FUSE layer does not
   allocate or free them. */
typedef struct {
    vfs_t*  vfs;        /* mounted VFS handle */
    char*   vfs_path;   /* backing file path (caller-owned) */
    int64_t epoch;      /* current working epoch (0 = base, odd = snapshot) */
    int64_t page_size;  /* VFS page size in bytes */
    bool    readonly;   /* mount is read-only */

    /* -----------------------------------------------------------------------
     * Control-file protocol (substitute for ioctl on macFUSE).
     *
     * macFUSE 3.18 does not deliver ioctl() calls on a directory fd to
     * the user-space daemon (kernel does not advertise FUSE_CAP_IOCTL_DIR).
     * To keep vfsctl functional without ioctl, the daemon exposes a
     * special hidden file at mount root: ".vfsctl".
     *
     * vfsctl opens this file, writes a text command ("snapshot\n",
     * "commit <epoch>\n", "delete-snapshot <epoch>\n", "gc\n"), then
     * reads the textual response.  On a fresh write the buffer is reset;
     * a read after each write returns the result of the most recent
     * command.  Standard FUSE read/write callbacks handle all of it.
     *
     * The buffer is mutex-protected so concurrent writers/reads from
     * multiple FUSE worker threads cannot interleave their bytes.
     * ----------------------------------------------------------------------- */
    pthread_mutex_t ctl_lock;
    char*   ctl_buf;       /* response bytes, or NULL if no command yet */
    int     ctl_len;       /* valid bytes in ctl_buf */
    int     ctl_cap;       /* allocated capacity of ctl_buf */

    /* -----------------------------------------------------------------------
     * Readdir cache — small LRU of full directory listings.
     *
     * Each entry holds the complete vfs_dirent_t[] for one directory,
     * populated by vfs_readdir (one chain walk).  readdir
     * callbacks serve entries from this cache using the FUSE cursor
     * protocol (offset = position within the listing), avoiding
     * repeated chain walks for large directories.
     *
     * The cache is small (FUSEDIR_CACHE_SIZE = 32) so the lock
     * critical section is short and contention is low.
     * ----------------------------------------------------------------------- */
    FusedirCache dir_cache;
} fuse_vfs_state_t;

/* ---------------------------------------------------------------------------
 * Control-file helpers (see .vfsctl description above).
 *
 *   fuse_vfs_ctl_init(state)    — initialize the lock + buffer (called in init)
 *   fuse_vfs_ctl_destroy(state) — free buffer + destroy lock (called in destroy)
 *
 * Plus the FUSE-facing entry points called from the read/write/open/
 * getattr callbacks in fuse_vfs.c:
 *
 *   fuse_vfs_is_ctl_path(path)          — predicate: is this path "/.vfsctl"?
 *   fuse_vfs_ctl_getattr(stbuf, state)  — fill synthetic stat for the file
 *   fuse_vfs_ctl_write(state, buf, sz, off) — accept a command
 *   fuse_vfs_ctl_read(state, buf, sz, off)  — return next bytes of response
 * --------------------------------------------------------------------------- */
void fuse_vfs_ctl_init(fuse_vfs_state_t* state);
void fuse_vfs_ctl_destroy(fuse_vfs_state_t* state);
int  fuse_vfs_is_ctl_path(const char* path);
void fuse_vfs_ctl_getattr(struct fuse_darwin_attr* stbuf,
                          const fuse_vfs_state_t* state);
int  fuse_vfs_ctl_write(fuse_vfs_state_t* state,
                       const char* buf, size_t size, off_t offset);
int  fuse_vfs_ctl_read(fuse_vfs_state_t* state,
                      char* buf, size_t size, off_t offset);

/* ---------------------------------------------------------------------------
 * Option parsing state — ephemeral, populated by fuse_opt_parse,
 * consumed by fuse_vfs_init.  Not retained after mount.
 * --------------------------------------------------------------------------- */

typedef struct {
    char*   vfs_path;   /* strdup'd path to VFS backing file */
    int64_t epoch;      /* initial working epoch (0 = base) */
    int64_t page_size;  /* VFS page size (default 8192) */
    int     readonly;   /* non-zero for read-only mount */
} fuse_vfs_opts;

/* ---------------------------------------------------------------------------
 * Path resolution — splits a POSIX path ("/a/b/c.txt") into components
 * and walks the VFS tree to resolve the final VirtualPtr.
 * Returns VirtualPtr on success, negative VFS error code on failure.
 * This will be implemented in Phase 4 (src/fuse_path.c).
 * --------------------------------------------------------------------------- */
int64_t resolve_full_path(vfs_t* vfs, int64_t epoch, const char* path);

/* ---------------------------------------------------------------------------
 * Utility — returns non-zero if the VFS node at vp is a directory.
 * --------------------------------------------------------------------------- */
int fuse_is_dir(vfs_t* vfs, int64_t vp);

/* ---------------------------------------------------------------------------
 * Error mapping — converts a VFS error code (vfs_error_t) to a negative
 * errno value suitable for FUSE callback returns.
 * --------------------------------------------------------------------------- */
int vfs_error_to_errno(int vfs_err);

/* Same mapping but to a stable, ASCII string ("OK", "not_found", ...).
   Used by the control-file protocol in src/fuse_vfs_ctl.c. */
const char* vfs_error_to_str(int vfs_err);

/* ---------------------------------------------------------------------------
 * FUSE option parsing callback — processes custom command-line options
 * (--vfs-file, --readonly, --epoch) and populates fuse_vfs_state_t.
 * Implementation in src/fuse_vfs.c (Phase 5).
 * --------------------------------------------------------------------------- */
#ifdef FUSE3_FOUND
int fuse_vfs_opt_proc(void* data, const char* arg, int key,
                      struct fuse_args* outargs);
#endif

#ifdef FUSE3_FOUND
/* Exposed for fuse_main.c: the option specification table and
   option-processing callback used by fuse_opt_parse. */
extern const struct fuse_opt fuse_vfs_opts_spec[];
#endif

/* ---------------------------------------------------------------------------
 * FUSE operations callbacks — each corresponds to a fuse_operations member.
 * These receive fuse_vfs_state_t* via fuse_get_context()->private_data.
 * Implementations in src/fuse_vfs.c (Phase 5).
 * --------------------------------------------------------------------------- */
#ifdef FUSE3_FOUND
typedef uint64_t fuse_ino_t;void* fuse_vfs_init(struct fuse_conn_info* conn, struct fuse_config* cfg);
void  fuse_vfs_destroy(void* private_data);
int   fuse_vfs_getattr(const char* path, struct fuse_darwin_attr* stbuf,
                       struct fuse_file_info* fi);
int   fuse_vfs_readdir(const char* path, void* buf,
                       fuse_darwin_fill_dir_t filler,
                       off_t offset, struct fuse_file_info* fi,
                       enum fuse_readdir_flags flags);
int   fuse_vfs_open(const char* path, struct fuse_file_info* fi);
int   fuse_vfs_read(const char* path, char* buf, size_t size, off_t offset,
                    struct fuse_file_info* fi);
int   fuse_vfs_write(const char* path, const char* buf, size_t size,
                     off_t offset, struct fuse_file_info* fi);
int   fuse_vfs_create(const char* path, mode_t mode,
                      struct fuse_file_info* fi
#ifdef __APPLE__
                      , uint32_t flags
#endif
                      );
int   fuse_vfs_unlink(const char* path);
int   fuse_vfs_mkdir(const char* path, mode_t mode);
int   fuse_vfs_rmdir(const char* path);
int   fuse_vfs_rename(const char* from, const char* to, unsigned int flags);
int   fuse_vfs_truncate(const char* path, off_t size,
                        struct fuse_file_info* fi);
int   fuse_vfs_utimens(const char* path, const struct timespec tv[2],
                       struct fuse_file_info* fi);

/* Phase 6: directory scan callbacks */
int   fuse_vfs_opendir(const char* path, struct fuse_file_info* fi);
int   fuse_vfs_releasedir(const char* path, struct fuse_file_info* fi);

/* Phase 7: release callback */
int   fuse_vfs_release(const char* path, struct fuse_file_info* fi);

/* Phase 9: supplementary operations */
int   fuse_vfs_flush(const char* path, struct fuse_file_info* fi);
int   fuse_vfs_statfs(const char* path,
#ifdef __APPLE__
                      struct statfs* stbuf
#else
                      struct statvfs* stbuf
#endif
                      );
int   fuse_vfs_access(const char* path, int mask);
int   fuse_vfs_chmod(const char* path, mode_t mode, struct fuse_file_info* fi);
int   fuse_vfs_chown(const char* path, uid_t uid, gid_t gid,
                     struct fuse_file_info* fi);
int   fuse_vfs_readlink(const char* path, char* buf, size_t size);
int   fuse_vfs_symlink(const char* from, const char* to);
int   fuse_vfs_link(const char* from, const char* to);

/* Phase 9: extended attributes */
int   fuse_vfs_setxattr(const char* path, const char* name,
                        const char* value, size_t size, int flags);
int   fuse_vfs_getxattr(const char* path, const char* name,
                        char* value, size_t size);
int   fuse_vfs_listxattr(const char* path, char* list, size_t size);
int   fuse_vfs_removexattr(const char* path, const char* name);

int   fuse_vfs_setattr(const char* path, struct fuse_darwin_attr* attr,
                       int to_set, struct fuse_file_info* fi);

/* Required by libfuse: resolve a path to its inode.
   For the root dir, returns 1; for child paths, calls vfs_open at epoch. */
int   fuse_vfs_lookup(fuse_ino_t parent, const char* name);
#endif /* FUSE3_FOUND */



#endif /* VFS_FUSE_VFS_H */
