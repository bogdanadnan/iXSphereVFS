cmake_minimum_required(VERSION 3.16)
project(iXSphereVFS VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic")

add_library(ixsphere_vfs STATIC
    src/vfs.c
    src/crc32c.c
    src/storage.c
    src/indirection.c
    src/lazy_mirror.c
    src/page_cache.c
    src/pool.c
    src/nodes.c
    src/epoch.c
    src/tree.c
    src/page_array.c
    src/dentry_cache.c
    src/mapper.c
    src/touched.c
    src/gc.c
    src/gc_map.c
    src/var_array.c
)

set_source_files_properties(src/var_array.c PROPERTIES COMPILE_FLAGS "-DVFS_VAR_ARRAY_TESTING -Wno-pedantic -fno-strict-aliasing")
set_source_files_properties(src/tree.c PROPERTIES COMPILE_FLAGS "-DVFS_NAME_HASH_TESTING")
set_source_files_properties(src/nodes.c PROPERTIES COMPILE_FLAGS "-DVFS_NAME_HASH_TESTING")

target_include_directories(ixsphere_vfs PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

enable_testing()

add_executable(vfs_test test/test_main.c)
target_link_libraries(vfs_test ixsphere_vfs pthread)
add_test(NAME vfs_test COMMAND vfs_test)

add_executable(test_storage test/test_storage.c)
target_link_libraries(test_storage ixsphere_vfs pthread)
add_test(NAME test_storage COMMAND test_storage)

add_executable(test_pool test/test_pool.c)
target_link_libraries(test_pool ixsphere_vfs pthread)
add_test(NAME test_pool COMMAND test_pool)

add_executable(test_nodes test/test_nodes.c)
target_link_libraries(test_nodes ixsphere_vfs pthread)
add_test(NAME test_nodes COMMAND test_nodes)

add_executable(test_tree test/test_tree.c)
target_link_libraries(test_tree ixsphere_vfs pthread)
add_test(NAME test_tree COMMAND test_tree)

add_executable(test_mapper test/test_mapper.c)
target_link_libraries(test_mapper ixsphere_vfs pthread)
add_test(NAME test_mapper COMMAND test_mapper)

add_executable(test_epoch test/test_epoch.c)
target_link_libraries(test_epoch ixsphere_vfs pthread)
add_test(NAME test_epoch COMMAND test_epoch)

add_executable(test_gc test/test_gc.c)
target_link_libraries(test_gc ixsphere_vfs pthread)
add_test(NAME test_gc COMMAND test_gc)

add_executable(test_var_array test/test_var_array.c)
target_link_libraries(test_var_array ixsphere_vfs pthread)
add_test(NAME test_var_array COMMAND test_var_array)
set_source_files_properties(test/test_tree.c PROPERTIES COMPILE_FLAGS "-DVFS_NAME_HASH_TESTING")
set_source_files_properties(test/test_var_array.c PROPERTIES COMPILE_FLAGS "-DVFS_VAR_ARRAY_TESTING -Wno-pedantic -Wno-incompatible-pointer-types -fno-strict-aliasing")

add_executable(test_crash test/test_crash.c)
target_link_libraries(test_crash ixsphere_vfs pthread)
if(NOT WIN32)
add_test(NAME test_crash COMMAND test_crash)
else()
# test_crash uses fork/kill/waitpid — skip on Windows
endif()

add_executable(vfs_bench bench/bench.c)
target_link_libraries(vfs_bench ixsphere_vfs pthread)
