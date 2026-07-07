/* FUSE-based filesystem interface for iXSphereVFS.
 * Conditionally built when FUSE3 is available.
 * Exposes a VFS mount as a FUSE filesystem.
 */

#include "ixsphere/vfs.h"
#include "ixsphere/vfs_internal.h"

#ifdef FUSE3_FOUND
#include <fuse3/fuse.h>
#include <fuse3/fuse_lowlevel.h>
#endif

/* ---------------------------------------------------------------------------
 * FUSE operations — populated when FUSE3_FOUND is defined.
 * When FUSE3 is not available, this file compiles to an empty translation
 * unit (all code behind #ifdef FUSE3_FOUND).
 * --------------------------------------------------------------------------- */

/* Placeholder for FUSE operations implementation */
