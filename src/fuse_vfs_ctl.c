/* ---------------------------------------------------------------------------
 * fuse_vfs_ctl.c — control-file protocol for vfsctl
 *
 * macFUSE 3.18 does not deliver ioctl() calls on a directory fd to the
 * user-space daemon (the kernel does not advertise FUSE_CAP_IOCTL_DIR,
 * so even setting conn->want |= FUSE_CAP_IOCTL_DIR has no effect, and
 * ioctl() on a FUSE fd returns ENOTTY without forwarding to the daemon).
 *
 * To keep vfsctl functional, the daemon exposes a special hidden file
 * at the mount root called ".vfsctl".  The client opens it, writes a
 * text command ("snapshot\n", "commit <epoch>\n", "delete-snapshot
 * <epoch>\n", "gc\n"), then reads the textual response.
 *
 * Each write resets the response buffer; each read returns the
 * response of the most recent command (or EOF if no command has been
 * issued yet on this open file).  The buffer is mutex-protected so
 * concurrent FUSE worker threads cannot interleave.
 * --------------------------------------------------------------------------- */

#include "fuse_vfs.h"
#include "ixsphere/vfs.h"
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define VFS_CTL_PATH        "/.vfsctl"
#define VFS_CTL_PATH_LEN    9   /* strlen("/.vfsctl") */
#define VFS_CTL_BUF_INIT    64
#define VFS_CTL_LINE_MAX    128 /* max command line we'll read */

void fuse_vfs_ctl_init(fuse_vfs_state_t* state) {
    pthread_mutex_init(&state->ctl_lock, NULL);
    state->ctl_buf  = NULL;
    state->ctl_len  = 0;
    state->ctl_cap  = 0;
}

void fuse_vfs_ctl_destroy(fuse_vfs_state_t* state) {
    pthread_mutex_destroy(&state->ctl_lock);
    free(state->ctl_buf);
    state->ctl_buf  = NULL;
    state->ctl_len  = 0;
    state->ctl_cap  = 0;
}

/* -----------------------------------------------------------------------
 * Internal helpers
 * ----------------------------------------------------------------------- */

/* Replace the response buffer with the given bytes (no NUL terminator).
   Returns 0 on success, -errno on failure.  Mutex must be held by caller. */
static int ctl_set_buf(fuse_vfs_state_t* state, const char* data, int len) {
    if (len + 1 > state->ctl_cap) {
        int new_cap = state->ctl_cap ? state->ctl_cap * 2 : VFS_CTL_BUF_INIT;
        while (new_cap < len + 1) new_cap *= 2;
        char* nb = realloc(state->ctl_buf, (size_t)new_cap);
        if (!nb) return -ENOMEM;
        state->ctl_buf = nb;
        state->ctl_cap = new_cap;
    }
    if (len > 0) memcpy(state->ctl_buf, data, (size_t)len);
    state->ctl_len = len;
    return 0;
}

/* Append formatted text to the response buffer.  Mutex must be held. */
static int ctl_appendf(fuse_vfs_state_t* state, const char* fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return -EINVAL;
    if (n >= (int)sizeof(tmp)) n = (int)sizeof(tmp) - 1;

    int old = state->ctl_len;
    if ((old + n + 1) > state->ctl_cap) {
        int new_cap = state->ctl_cap ? state->ctl_cap * 2 : VFS_CTL_BUF_INIT;
        while (new_cap < old + n + 1) new_cap *= 2;
        char* nb = realloc(state->ctl_buf, (size_t)new_cap);
        if (!nb) return -ENOMEM;
        state->ctl_buf = nb;
        state->ctl_cap = new_cap;
    }
    memcpy(state->ctl_buf + old, tmp, (size_t)n);
    state->ctl_len = old + n;
    return 0;
}

/* -----------------------------------------------------------------------
 * Command dispatch
 *
 * Mutex held on entry.  The command has been trimmed of leading and
 * trailing whitespace and a single trailing newline.  Returns 0 on
 * success, -errno on failure (failure also aborts the response buffer).
 * ----------------------------------------------------------------------- */
static int ctl_dispatch(fuse_vfs_state_t* state, const char* cmd) {
    /* Always start with an empty response */
    state->ctl_len = 0;

    vfs_t* vfs = state->vfs;
    int rc = 0;

    if (strcmp(cmd, "snapshot") == 0) {
        int64_t epoch = vfs_snapshot(vfs);
        if (epoch < 0) {
            rc = ctl_appendf(state, "ERR %s\n",
                              vfs_error_string(vfs_last_error(vfs)));
        } else {
            rc = ctl_appendf(state, "%lld\n", (long long)epoch);
        }
    } else if (strncmp(cmd, "commit ", 7) == 0) {
        long long epoch = atoll(cmd + 7);
        if (epoch <= 0) {
            rc = ctl_appendf(state, "ERR invalid epoch\n");
        } else {
            int r = vfs_commit(vfs, (int64_t)epoch);
            if (r == 0)              rc = ctl_appendf(state, "ok\n");
            else if (r == VFS_ERR_CONFLICT) rc = ctl_appendf(state, "conflict\n");
            else                     rc = ctl_appendf(state, "ERR %s\n",
                                                   vfs_error_string((vfs_error_t)r));
        }
    } else if (strncmp(cmd, "delete-snapshot ", 16) == 0) {
        long long epoch = atoll(cmd + 16);
        if (epoch <= 0) {
            rc = ctl_appendf(state, "ERR invalid epoch\n");
        } else {
            int r = vfs_delete_snapshot(vfs, (int64_t)epoch);
            if (r == 0) rc = ctl_appendf(state, "ok\n");
            else        rc = ctl_appendf(state, "ERR %s\n",
                                         vfs_error_string((vfs_error_t)r));
        }
    } else if (strcmp(cmd, "gc") == 0) {
        int r = vfs_gc(vfs);
        if (r == 0) rc = ctl_appendf(state, "ok\n");
        else        rc = ctl_appendf(state, "ERR %s\n",
                                     vfs_error_string((vfs_error_t)r));
    } else {
        rc = ctl_appendf(state, "ERR unknown command: '%s'\n", cmd);
    }
    return rc;
}

/* -----------------------------------------------------------------------
 * Public FUSE-facing entry points (called from fuse_vfs.c callbacks)
 * ----------------------------------------------------------------------- */

/* Returns 1 if the path is the control file, 0 otherwise. */
int fuse_vfs_is_ctl_path(const char* path) {
    if (!path) return 0;
    /* macFUSE may pass path="/" with nullpath_ok=1; we want the
       file under root, so compare with the leading "/" form. */
    return (strcmp(path, VFS_CTL_PATH) == 0 ||
            strcmp(path,  "." VFS_CTL_PATH + 1) == 0);
}

/* Build the synthetic stat for ".vfsctl".  Mode 0644 (read+write,
   no exec).  Size is the length of the latest response, or 0 if no
   command has been issued. */
void fuse_vfs_ctl_getattr(struct fuse_darwin_attr* stbuf,
                          const fuse_vfs_state_t* state) {
    memset(stbuf, 0, sizeof(*stbuf));
    stbuf->mode = S_IFREG | 0644;
    stbuf->nlink = 1;
    /* Snapshot the current length under the mutex */
    pthread_mutex_lock((pthread_mutex_t*)&state->ctl_lock);
    stbuf->size = (off_t)state->ctl_len;
    pthread_mutex_unlock((pthread_mutex_t*)&state->ctl_lock);
    time_t now = time(NULL);
    stbuf->mtimespec.tv_sec = now;
    stbuf->ctimespec.tv_sec = now;
    stbuf->atimespec.tv_sec = now;
    stbuf->uid = getuid();
    stbuf->gid = getgid();
}

/* Handler invoked from the FUSE write() callback.  Returns the number
   of bytes "consumed" (always the full size — writes are atomic from
   the caller's POV).  -errno on failure. */
int fuse_vfs_ctl_write(fuse_vfs_state_t* state,
                      const char* buf, size_t size, off_t offset) {
    if (offset != 0) return -EINVAL;     /* only pwrite at offset 0 supported */

    /* Buffer the command locally (VFS_CTL_LINE_MAX).  Anything longer is
       truncated — there is no command we accept that long. */
    char line[VFS_CTL_LINE_MAX + 1];
    int n = (int)(size > VFS_CTL_LINE_MAX ? VFS_CTL_LINE_MAX : size);
    memcpy(line, buf, (size_t)n);
    line[n] = '\0';

    /* Strip trailing whitespace/newline */
    while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r' ||
                     line[n-1] == ' '  || line[n-1] == '\t')) {
        line[--n] = '\0';
    }
    /* Strip leading whitespace */
    char* p = line;
    while (*p == ' ' || *p == '\t') p++;

    pthread_mutex_lock(&state->ctl_lock);
    int rc = ctl_dispatch(state, p);
    pthread_mutex_unlock(&state->ctl_lock);

    return rc < 0 ? rc : (int)size;
}

/* Handler invoked from the FUSE read() callback.  Returns bytes copied
   (>= 0) or 0 at EOF (when ctl_len <= offset).  -errno on failure. */
int fuse_vfs_ctl_read(fuse_vfs_state_t* state,
                     char* buf, size_t size, off_t offset) {
    pthread_mutex_lock(&state->ctl_lock);

    if ((int)offset >= state->ctl_len) {
        pthread_mutex_unlock(&state->ctl_lock);
        return 0;   /* EOF */
    }
    int avail = state->ctl_len - (int)offset;
    int n = (avail < (int)size) ? avail : (int)size;
    memcpy(buf, state->ctl_buf + offset, (size_t)n);

    pthread_mutex_unlock(&state->ctl_lock);
    return n;
}