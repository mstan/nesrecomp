# runner.cmake — Source list for NESRecomp game projects.
#
# Usage in a game project CMakeLists.txt:
#   set(NESRECOMP_ROOT ${CMAKE_SOURCE_DIR}/nesrecomp)
#   include(${NESRECOMP_ROOT}/runner/runner.cmake)
#   add_executable(MyGame ${NESRECOMP_RUNNER_SOURCES} extras.c generated/game_full.c ...)
#   target_include_directories(MyGame PRIVATE ${NESRECOMP_RUNNER_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR})
#   target_link_libraries(MyGame SDL2::SDL2)

set(NESRECOMP_RUNNER_ROOT ${CMAKE_CURRENT_LIST_DIR})

# launcher.c uses DbgHelp for native Windows crash backtraces.  Game projects
# include runner.cmake before creating their executable, so a directory-scoped
# link dependency keeps older titles from each having to know about this runner
# implementation detail.  The MSVC pragma in launcher.c covers that toolchain;
# this also supplies the import library, at the correct end-of-link position,
# for MinGW.
if(WIN32)
    link_libraries(dbghelp)
endif()

set(NESRECOMP_RUNNER_SOURCES
    ${NESRECOMP_RUNNER_ROOT}/src/main_runner.c
    ${NESRECOMP_RUNNER_ROOT}/src/runtime.c
    ${NESRECOMP_RUNNER_ROOT}/src/recomp_stack.c
    ${NESRECOMP_RUNNER_ROOT}/src/ppu_renderer.c
    ${NESRECOMP_RUNNER_ROOT}/src/ppu_dot.c
    ${NESRECOMP_RUNNER_ROOT}/src/apu.c
    # Mono PCM one-shots registered by trusted game mods. Mixed into the same
    # producer frame as the APU, before launcher volume and the audio bridge.
    ${NESRECOMP_RUNNER_ROOT}/src/mod_audio.c
    ${NESRECOMP_RUNNER_ROOT}/src/mapper.c
    ${NESRECOMP_RUNNER_ROOT}/src/logger.c
    ${NESRECOMP_RUNNER_ROOT}/src/input_script.c
    ${NESRECOMP_RUNNER_ROOT}/src/savestate.c
    ${NESRECOMP_RUNNER_ROOT}/src/save_ram.c
    ${NESRECOMP_RUNNER_ROOT}/src/config.c
    ${NESRECOMP_RUNNER_ROOT}/src/launcher.c
    ${NESRECOMP_RUNNER_ROOT}/src/crc32.c
    ${NESRECOMP_RUNNER_ROOT}/src/coroutine.c
    # Interpreter fallback tier + the shared 6502 decode table from the
    # recompiler (single source of truth, so interpreted/recompiled decode
    # cannot diverge). See docs/PHASE1_INTERP_FALLBACK_PLAN.md.
    ${NESRECOMP_RUNNER_ROOT}/src/interp.c
    ${NESRECOMP_RUNNER_ROOT}/../recompiler/src/cpu6502_decoder.c
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
    # Optional game-authored 3D presentation layer. It is inert unless a
    # game calls nes_voxel_render() from game_post_render().
    ${NESRECOMP_RUNNER_ROOT}/src/voxel_renderer.c
    ${NESRECOMP_RUNNER_ROOT}/src/voxel_screen_profile.c
    # Host-side movement controllers imported from another game, plus the
    # always-on movement trace ring. Inert unless a game registers and selects
    # a controller. See docs/FOREIGN_CONTROLLER.md.
    ${NESRECOMP_RUNNER_ROOT}/src/foreign_controller.c
    # Trusted mod callbacks at recompiled 6502 function entries. Always
    # compiled, because generated code for an opted-in title calls into it
    # whether or not the mod package runtime is built.
    # See docs/MOD_PACKAGES.md and runner/include/mod_function_hooks.h.
    ${NESRECOMP_RUNNER_ROOT}/src/mod_function_hooks.c
    # Id-keyed per-mod save-state extension registry. Always compiled for the
    # same reason as mod_function_hooks.c above; savestate.c calls into it
    # unconditionally. See MODDING.md and runner/include/mod_savestate.h.
    ${NESRECOMP_RUNNER_ROOT}/src/mod_savestate.c
)

set(NESRECOMP_RUNNER_INCLUDE_DIRS
    ${NESRECOMP_RUNNER_ROOT}/include
    ${NESRECOMP_RUNNER_ROOT}/../recompiler/src   # cpu6502_decoder.h (shared decode table)
)

# Schema-driven mod packages and trusted static plugins. This is deliberately
# opt-in: ordinary games do not compile the loader, expose a Mods navigation
# item, create a mods directory, or change runtime behavior. An opting-in game
# owns its recomp-ui pin, package catalog, and linked plugin implementations.
option(NESRECOMP_ENABLE_MODS
    "Build the NES mod package loader and trusted-plugin runtime"
    OFF)
if(NESRECOMP_ENABLE_MODS)
    list(APPEND NESRECOMP_RUNNER_SOURCES
        ${NESRECOMP_RUNNER_ROOT}/src/mod_runtime.cpp
    )
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    add_compile_definitions(NESRECOMP_ENABLE_MODS=1)
    # recomp-ui requires both its compile-time gate and a non-null provider.
    set(RECOMP_UI_ENABLE_MODS ON CACHE BOOL
        "Enable recomp-ui Mods view for this NES mod-enabled game" FORCE)
    message(STATUS
        "NES mods: package loader + trusted static plugins enabled")
else()
    add_compile_definitions(NESRECOMP_ENABLE_MODS=0)
endif()

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

# Generated functions can optionally maintain a diagnostic shadow call stack.
# Some older game projects define RECOMP_STACK_TRACKING unconditionally even
# for production builds. Make the shared option authoritative in both
# directions: define the macro when enabled, and explicitly undefine it when
# disabled so trace-off Release builds do not retain two out-of-line diagnostic
# calls at every generated function boundary. Projects can therefore request
# stack tracking without the TCP trace server even if their own CMakeLists does
# not know about the implementation macro.
option(NESRECOMP_ENABLE_STACK_TRACKING
    "Track generated function entries/exits for diagnostics"
    ${NESRECOMP_ENABLE_TRACE})
if(NESRECOMP_ENABLE_STACK_TRACKING)
    add_compile_definitions(RECOMP_STACK_TRACKING)
else()
    if(MSVC)
        add_compile_options(/URECOMP_STACK_TRACKING)
    else()
        add_compile_options(-URECOMP_STACK_TRACKING)
    endif()
endif()

# Recent-dispatch and frame-event rings are useful post-mortem diagnostics, but
# production builds should not pay their hot-path writes or reserve the large
# frame ring. Compile the policy into runtime.c only so toggling it does not
# invalidate every generated translation unit. It may be enabled independently
# of the TCP trace server for a diagnostic Release build.
option(NESRECOMP_ENABLE_POSTMORTEM_RINGS
    "Capture recent dispatches and frame events for post-mortem diagnostics"
    ${NESRECOMP_ENABLE_TRACE})
if(NESRECOMP_ENABLE_POSTMORTEM_RINGS)
    set(_NESRECOMP_POSTMORTEM_RINGS 1)
else()
    set(_NESRECOMP_POSTMORTEM_RINGS 0)
endif()
set_property(SOURCE ${NESRECOMP_RUNNER_ROOT}/src/runtime.c APPEND PROPERTY
    COMPILE_DEFINITIONS
    NESRECOMP_POSTMORTEM_RINGS=${_NESRECOMP_POSTMORTEM_RINGS})
unset(_NESRECOMP_POSTMORTEM_RINGS)

# The recompiled C in each game's generated/ is machine-generated and leans on
# K&R-style implicit declarations (cross-bank func_XXXX calls without a prior
# prototype). gcc warns; clang (and gcc 14+) make it a hard error by default,
# which breaks the macOS/strict-Linux build. Demote it to non-fatal for the whole
# game target, exactly as the per-game MSVC builds tolerate /wd4102 et al.
# Directory-scoped so it reaches the game target that include()s this file.
if(MSVC)
    # Large generated game translation units can exceed COFF's default
    # section-count limit, especially in debug-information-bearing builds.
    # Apply /bigobj directory-wide because the game target is declared after
    # including this file.
    add_compile_options(/bigobj)
else()
    add_compile_options(-Wno-implicit-function-declaration -Wno-implicit-int)
endif()

include(${NESRECOMP_RUNNER_ROOT}/recomp_net.cmake)
