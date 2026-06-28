# iXSphereVFS

Standalone C implementation of the Spec 30c epoch-versioned unified tree VFS.

## Build

```
mkdir build && cd build
cmake ..
make
make test
```

## Structure

```
include/         Public API header
src/             Library source
test/            Test suite
```

## Spec

See `docs/30c-vfs-v2-epoch-tree.md` in the main iXSpherePlatform repository.
