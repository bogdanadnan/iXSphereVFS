cmake_minimum_required(VERSION 3.16)
project(iXSphereVFS VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -march=native -Wall -Wextra -Wpedantic")

# ── Library ────────────────────────────────────────────────

add_library(ixsphere_vfs STATIC
    src/storage.c
    src/crc32c.c
    src/error.c
)

target_include_directories(ixsphere_vfs PUBLIC 
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

# ── Tests ──────────────────────────────────────────────────

enable_testing()
add_executable(vfs_test
    test/test_runner.c
    test/test_platform.c
    test/test_crc32c.c
    test/test_atomics.c
    test/test_page_buf.c
    test/test_storage.c
)
target_include_directories(vfs_test PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(vfs_test ixsphere_vfs)
add_test(NAME vfs_test COMMAND vfs_test)

# ── Benchmarks (Phase 10) ─────────────────────────────────

option(BUILD_BENCH "Build benchmark suite" OFF)
if(BUILD_BENCH)
    add_executable(vfs_bench bench/bench.c)
    target_link_libraries(vfs_bench ixsphere_vfs)
endif()
