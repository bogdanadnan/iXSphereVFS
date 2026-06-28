cmake_minimum_required(VERSION 3.16)
project(iXSphereVFS VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── Library ────────────────────────────────────────────────

add_library(ixsphere_vfs STATIC
    src/page_header.c
    src/free_bitmap.c
    src/storage_backend.c
    src/superblock.c
    src/node.c
    src/pool.c
    src/epoch.c
    src/tree.c
    src/gc.c
)

target_include_directories(ixsphere_vfs PUBLIC include)

# ── Tests ──────────────────────────────────────────────────

option(BUILD_TESTS "Build test suite" ON)
if(BUILD_TESTS)
    enable_testing()
    add_executable(vfs_test test/test_main.c)
    target_link_libraries(vfs_test ixsphere_vfs)
    add_test(NAME vfs_test COMMAND vfs_test)
endif()
