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
    ${NESRECOMP_RUNNER_ROOT}/src/ppu_dot.c
    ${NESRECOMP_RUNNER_ROOT}/src/apu.c
    ${NESRECOMP_RUNNER_ROOT}/src/mapper.c
    ${NESRECOMP_RUNNER_ROOT}/src/logger.c
    ${NESRECOMP_RUNNER_ROOT}/src/input_script.c
    ${NESRECOMP_RUNNER_ROOT}/src/savestate.c
    ${NESRECOMP_RUNNER_ROOT}/src/save_ram.c
    ${NESRECOMP_RUNNER_ROOT}/src/config.c
    ${NESRECOMP_RUNNER_ROOT}/src/launcher.c
    ${NESRECOMP_RUNNER_ROOT}/src/crc32.c
    ${NESRECOMP_RUNNER_ROOT}/src/coroutine.c
    ${NESRECOMP_RUNNER_ROOT}/src/keybinds.c
    ${NESRECOMP_RUNNER_ROOT}/src/controller.c
    ${NESRECOMP_RUNNER_ROOT}/src/override_chr.c
    ${NESRECOMP_RUNNER_ROOT}/src/chr_codec.c
    ${NESRECOMP_RUNNER_ROOT}/src/hdpack.c
    # Verified-enhancement shadow QoL layer (default OFF; byte-identical when
    # off). See runner/src/{audio_shadow,apu_shadow,color_lut}.{c,h} and
    # docs/SHADOW_ENHANCEMENTS.md.
    ${NESRECOMP_RUNNER_ROOT}/src/audio_shadow.c
    ${NESRECOMP_RUNNER_ROOT}/src/apu_shadow.c
    ${NESRECOMP_RUNNER_ROOT}/src/color_lut.c
)

set(NESRECOMP_RUNNER_INCLUDE_DIRS ${NESRECOMP_RUNNER_ROOT}/include)

# ---- Prod vs debug: TCP debug server + observability rings ----
# The TCP debug server (debug_server.c: socket listener, 36000-frame ring buffer,
# JSON command protocol) is a developer-only feature. It is OFF by default so a
# normal shipping build never opens a port or carries the ring. When OFF we
# compile debug_server_stub.c, which provides no-op definitions of the same public
# API so the runner + per-game extras.c still link. Opt in with
# -DNESRECOMP_ENABLE_TRACE=ON (tools/build-linux.sh --config debug does this).
# add_compile_definitions() is directory-scoped and applies to the game target,
# which include()s this file before add_executable(), so every TU sees the flag.
# Default ON preserves the prior always-on dev behavior; release builds pass
# -DNESRECOMP_ENABLE_TRACE=OFF (tools/build-linux.sh --config prod) to strip it.
option(NESRECOMP_ENABLE_TRACE "Build the TCP debug server / observability rings" ON)
if(NESRECOMP_ENABLE_TRACE)
    list(APPEND NESRECOMP_RUNNER_SOURCES ${NESRECOMP_RUNNER_ROOT}/src/debug_server.c)
    add_compile_definitions(NESRECOMP_TRACE=1)
else()
    list(APPEND NESRECOMP_RUNNER_SOURCES ${NESRECOMP_RUNNER_ROOT}/src/debug_server_stub.c)
    add_compile_definitions(NESRECOMP_TRACE=0)
endif()

# The recompiled C in each game's generated/ is machine-generated and leans on
# K&R-style implicit declarations (cross-bank func_XXXX calls without a prior
# prototype). gcc warns; clang (and gcc 14+) make it a hard error by default,
# which breaks the macOS/strict-Linux build. Demote it to non-fatal for the whole
# game target, exactly as the per-game MSVC builds tolerate /wd4102 et al.
# Directory-scoped so it reaches the game target that include()s this file.
if(NOT MSVC)
    add_compile_options(-Wno-implicit-function-declaration -Wno-implicit-int)
endif()

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
