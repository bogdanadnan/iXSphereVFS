/* fuse_vfs.h — shared state and declarations for the FUSE filesystem layer.
 *
 * Includes ONLY the public ixsphere/vfs.h header — never vfs_internal.h.
 * This ensures the FUSE layer can be built against only the public API
 * surface, keeping internal struct details encapsulated.
 */

#ifndef VFS_FUSE_VFS_H
#define VFS_FUSE_VFS_H

#include "ixsphere/vfs.h"
#include <stdbool.h>

/* Per-mount state passed to FUSE callbacks via private_data.
   All fields are owned by the caller; the FUSE layer does not
   allocate or free them. */
typedef struct {
    vfs_t*  vfs;        /* mounted VFS handle */
    char*   vfs_path;   /* backing file path (caller-owned) */
    int64_t epoch;      /* current working epoch (0 = base, odd = snapshot) */
    int64_t page_size;  /* VFS page size in bytes */
    bool    readonly;   /* mount is read-only */
} fuse_vfs_state_t;

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

/* ---------------------------------------------------------------------------
 * FUSE option parsing callback — processes custom command-line options
 * (--vfs-file, --readonly, --epoch) and populates fuse_vfs_state_t.
 * Implementation in src/fuse_vfs.c (Phase 5).
 * --------------------------------------------------------------------------- */
#ifdef FUSE3_FOUND
int fuse_vfs_opt_proc(void* data, const char* arg, int key,
                      struct fuse_args* outargs);
#endif

/* Exposed for fuse_main.c: the option specification table and
   option-processing callback used by fuse_opt_parse. */
extern const struct fuse_opt fuse_vfs_opts_spec[];

/* ---------------------------------------------------------------------------
 * ioctl handler — dispatches VFS_IOC_SNAPSHOT, VFS_IOC_COMMIT,
 * VFS_IOC_DELETE_SNAP, and VFS_IOC_GC to the underlying VFS.
 * Implementation in src/fuse_vfs.c (Phase 5).
 * --------------------------------------------------------------------------- */
int fuse_vfs_ioctl(vfs_t* vfs, unsigned long request, void* arg);

/* ---------------------------------------------------------------------------
 * FUSE operations callbacks — each corresponds to a fuse_operations member.
 * These receive fuse_vfs_state_t* via fuse_get_context()->private_data.
 * Implementations in src/fuse_vfs.c (Phase 5).
 * --------------------------------------------------------------------------- */
#ifdef FUSE3_FOUND
void* fuse_vfs_init(struct fuse_conn_info* conn, struct fuse_config* cfg);
void  fuse_vfs_destroy(void* private_data);
int   fuse_vfs_getattr(const char* path, struct stat* stbuf,
                       struct fuse_file_info* fi);
int   fuse_vfs_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info* fi,
                       enum fuse_readdir_flags flags);
int   fuse_vfs_open(const char* path, struct fuse_file_info* fi);
int   fuse_vfs_read(const char* path, char* buf, size_t size, off_t offset,
                    struct fuse_file_info* fi);
int   fuse_vfs_write(const char* path, const char* buf, size_t size,
                     off_t offset, struct fuse_file_info* fi);
int   fuse_vfs_create(const char* path, mode_t mode,
                      struct fuse_file_info* fi);
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
int   fuse_vfs_statfs(const char* path, struct statvfs* stbuf);
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

/* Phase 10: ioctl callback (FUSE-level, distinct from fuse_vfs_ioctl helper) */
int   fuse_vfs_ioctl_cb(fuse_ino_t ino, int cmd, void* arg,
                        struct fuse_file_info* fi, unsigned int flags,
                        void* data);
#endif /* FUSE3_FOUND */

#endif /* VFS_FUSE_VFS_H */
