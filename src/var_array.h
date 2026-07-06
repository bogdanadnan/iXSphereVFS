#ifndef VFS_VAR_ARRAY_H
#define VFS_VAR_ARRAY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Chunk size defines — the branching factor at every level of the tree.
 * DEFAULT: 256 entries per chunk (good cacheline trade-off, ~2KB)
 * MIN:     16  entries per chunk (tight minimum, ~128B)
 * MAX:     4096 entries per chunk (large-dataset ceiling, ~32KB) */
#define VFS_VAR_ARRAY_DEFAULT_CHUNK_SIZE  256
#define VFS_VAR_ARRAY_MIN_CHUNK_SIZE       16
#define VFS_VAR_ARRAY_MAX_CHUNK_SIZE     4096

#endif /* VFS_VAR_ARRAY_H */
