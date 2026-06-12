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
    ${NESRECOMP_RUNNER_ROOT}/src/debug_server.c
    ${NESRECOMP_RUNNER_ROOT}/src/coroutine.c
    ${NESRECOMP_RUNNER_ROOT}/src/keybinds.c
    ${NESRECOMP_RUNNER_ROOT}/src/controller.c
    ${NESRECOMP_RUNNER_ROOT}/src/override_chr.c
    ${NESRECOMP_RUNNER_ROOT}/src/chr_codec.c
    # Verified-enhancement shadow QoL layer (default OFF; byte-identical when
    # off). See runner/src/{audio_shadow,apu_shadow,color_lut}.{c,h} and
    # docs/SHADOW_ENHANCEMENTS.md.
    ${NESRECOMP_RUNNER_ROOT}/src/audio_shadow.c
    ${NESRECOMP_RUNNER_ROOT}/src/apu_shadow.c
    ${NESRECOMP_RUNNER_ROOT}/src/color_lut.c
)

set(NESRECOMP_RUNNER_INCLUDE_DIRS ${NESRECOMP_RUNNER_ROOT}/include)

# ---- Optional Nestopia Oracle ----
# Games opt in by setting ENABLE_NESTOPIA_ORACLE=ON and optionally NESTOPIA_DIR.
if(ENABLE_NESTOPIA_ORACLE)
    list(APPEND NESRECOMP_RUNNER_SOURCES
        ${NESRECOMP_RUNNER_ROOT}/src/nestopia_bridge.cpp
        ${NESRECOMP_RUNNER_ROOT}/src/nestopia_oracle_cmds.c
    )
    include(${NESRECOMP_RUNNER_ROOT}/nestopia_cmake.cmake)
endif()

# ---- Optional reverse debugger (Tier 1+) ----
# Games opt in by setting NESRECOMP_REVERSE_DEBUG=ON. Requires regenerating
# the recompiled C with NESRecomp.exe --reverse-debug so the generator emits
# RDB_STORE8 in place of direct nes_write calls. The game's CMakeLists
# must also add target_compile_definitions(<game> PRIVATE NESRECOMP_REVERSE_DEBUG=1)
# so both the runner translation units and the generated C see the flag.
# See REVERSE_DEBUGGER.md §Build flag design.
if(NESRECOMP_REVERSE_DEBUG)
    list(APPEND NESRECOMP_RUNNER_SOURCES
        ${NESRECOMP_RUNNER_ROOT}/src/reverse_debug.c
    )
endif()
