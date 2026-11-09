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
    if(APPLE)
        # macOS ld64 has no --gc-sections; -dead_strip is its equivalent.
        target_link_options(runtime_boundary_selftest PRIVATE -Wl,-dead_strip)
    else()
        target_link_options(runtime_boundary_selftest PRIVATE -Wl,--gc-sections)
    endif()
endif()
add_test(NAME runtime_boundary_selftest COMMAND runtime_boundary_selftest)
add_test(NAME runtime_tail_resume_selftest COMMAND runtime_boundary_selftest --tail-resume)
