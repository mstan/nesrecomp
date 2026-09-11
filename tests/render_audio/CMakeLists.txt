cmake_minimum_required(VERSION 3.16)
project(NESRecompRenderAudioSelftest C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(apu_period_cache_selftest
    apu_period_cache_selftest.c
    ../../runner/src/apu.c
)
target_include_directories(apu_period_cache_selftest PRIVATE
    ../../runner/include
    ../../runner/src
)

add_executable(ppu_renderer_sidecar_selftest
    ppu_renderer_sidecar_selftest.c
    ../../runner/src/ppu_renderer.c
)
target_include_directories(ppu_renderer_sidecar_selftest PRIVATE
    ../../runner/include
)

add_executable(voxel_grouping_selftest
    voxel_grouping_selftest.c
    ../../runner/src/voxel_renderer.c
)
target_include_directories(voxel_grouping_selftest PRIVATE ../../runner/include)

if(NOT MSVC)
    target_link_libraries(apu_period_cache_selftest m)
    target_link_libraries(ppu_renderer_sidecar_selftest m)
    target_link_libraries(voxel_grouping_selftest m)
endif()

enable_testing()
add_test(NAME apu_period_cache_selftest COMMAND apu_period_cache_selftest)
add_test(NAME ppu_renderer_sidecar_selftest COMMAND ppu_renderer_sidecar_selftest)
add_test(NAME voxel_grouping_selftest COMMAND voxel_grouping_selftest)
