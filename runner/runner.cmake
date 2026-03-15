# runner.cmake — Source list for NESRecomp game projects.
#
# Usage in a game project CMakeLists.txt:
#   set(NESRECOMP_ROOT ${CMAKE_SOURCE_DIR}/nesrecomp)
#   include(${NESRECOMP_ROOT}/runner/runner.cmake)
#   add_executable(MyGame ${NESRECOMP_RUNNER_SOURCES} extras.c generated/game_full.c ...)
#   target_include_directories(MyGame PRIVATE ${NESRECOMP_RUNNER_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR})
#   target_link_libraries(MyGame SDL2::SDL2)

set(NESRECOMP_RUNNER_ROOT ${CMAKE_CURRENT_LIST_DIR})

set(NESRECOMP_RUNNER_SOURCES
    ${NESRECOMP_RUNNER_ROOT}/src/main_runner.c
    ${NESRECOMP_RUNNER_ROOT}/src/runtime.c
    ${NESRECOMP_RUNNER_ROOT}/src/ppu_renderer.c
    ${NESRECOMP_RUNNER_ROOT}/src/apu.c
    ${NESRECOMP_RUNNER_ROOT}/src/mapper.c
    ${NESRECOMP_RUNNER_ROOT}/src/logger.c
    ${NESRECOMP_RUNNER_ROOT}/src/input_script.c
    ${NESRECOMP_RUNNER_ROOT}/src/savestate.c
    ${NESRECOMP_RUNNER_ROOT}/src/launcher.c
    ${NESRECOMP_RUNNER_ROOT}/src/crc32.c
)

set(NESRECOMP_RUNNER_INCLUDE_DIRS ${NESRECOMP_RUNNER_ROOT}/include)
