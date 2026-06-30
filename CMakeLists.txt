cmake_minimum_required(VERSION 3.16)
project(iXSphereVFS VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic")

add_library(ixsphere_vfs STATIC
    src/vfs.c
)

target_include_directories(ixsphere_vfs PUBLIC include)

enable_testing()
add_executable(vfs_test
    test/test_main.c
)
target_link_libraries(vfs_test ixsphere_vfs pthread)
add_test(NAME vfs_test COMMAND vfs_test)
