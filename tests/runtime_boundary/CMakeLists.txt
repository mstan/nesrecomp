cmake_minimum_required(VERSION 3.16)
project(NESRecompRuntimeBoundarySelftest C)

set(CMAKE_C_STANDARD 11)
enable_testing()

add_executable(runtime_boundary_selftest
    runtime_boundary_selftest.c
    ../../runner/src/runtime.c
    ../../runner/src/crc32.c
)
target_include_directories(runtime_boundary_selftest PRIVATE ../../runner/include)
if(NOT MSVC)
    target_compile_options(runtime_boundary_selftest PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(runtime_boundary_selftest PRIVATE -Wl,--gc-sections)
endif()
add_test(NAME runtime_boundary_selftest COMMAND runtime_boundary_selftest)
