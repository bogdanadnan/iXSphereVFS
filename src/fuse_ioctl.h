/* fuse_ioctl.h — ioctl command definitions for the iXSphereVFS FUSE driver.
 *
 * These commands are sent via ioctl() on a directory fd of the FUSE
 * mountpoint.  They operate on the VFS backing store through the
 * FUSE daemon process.
 *
 * Magic: 'v' (0x76) — iXSphereVFS
 */

#ifndef VFS_FUSE_IOCTL_H
#define VFS_FUSE_IOCTL_H

#ifndef _WIN32
#include <sys/ioctl.h>
#endif

#define VFS_IOC_MAGIC  0x76  /* 'v' */

/* Create a snapshot.  Returns the new snapshot epoch (int64_t). */
#define VFS_IOC_SNAPSHOT       _IOR(VFS_IOC_MAGIC, 1, int64_t)

/* Commit a snapshot.  arg = snapshot epoch (int64_t* in/out). */
#define VFS_IOC_COMMIT         _IOWR(VFS_IOC_MAGIC, 2, int64_t)

/* Soft-delete a snapshot.  arg = snapshot epoch (int64_t). */
#define VFS_IOC_DELETE_SNAP    _IOW(VFS_IOC_MAGIC, 3, int64_t)

/* Run garbage collection.  No argument. */
#define VFS_IOC_GC             _IO(VFS_IOC_MAGIC, 4)

#endif /* VFS_FUSE_IOCTL_H */
