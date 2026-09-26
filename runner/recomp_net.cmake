# recomp_net.cmake — rollback netplay for NESRecomp games (recomp-net +
# retcomm-rbengine), mirrored from snesrecomp runner/recomp_net.cmake.
#
# recomp-net (lib/recomp-net) owns the rollback episode driver, input history,
# scheduler, lobby client and link simulator; retcomm-rbengine
# (lib/retcomm-rbengine) owns the snapshot ring. What is compiled here is the
# NES binding over them (docs/NETPLAY.md):
#
#   runner/src/netplay/nes_netplay.c           session facade, config seal, SRAM
#   runner/src/netplay/nes_netplay_rb.c        RNetRbHost binding (INCREMENTAL)
#   runner/src/netplay/nes_netplay_identity.c  game_version / ROM SHA-256
#   runner/src/netplay/nes_host_lobby.c        adapter over recomp-ui's shared
#                                              netplay backend (launcher only)
#
# The snapshot + digest (src/rollback/nes_rb_state.c) and the determinism
# probe are part of every runner (runner.cmake) and inert without netplay.
#
# Usage from a game project's CMakeLists.txt, after add_executable() and after
# include(recomp-ui/recomp_ui.cmake) when the launcher is linked:
#
#   nesrecomp_enable_recomp_net(MyGame)
#
# or configure with -DNESRECOMP_ENABLE_NET=ON and call it the same way.
# NESRECOMP_GAME_VERSION: the release pin compared by the lobby and the
# driver's IDENT handshake; defaults to `git describe` of the game repository.

if(NOT NESRECOMP_RECOMP_NET_ROOT)
    get_filename_component(NESRECOMP_RECOMP_NET_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/../lib/recomp-net" ABSOLUTE)
endif()
if(NOT NESRECOMP_RBENGINE_ROOT)
    get_filename_component(NESRECOMP_RBENGINE_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/../lib/retcomm-rbengine" ABSOLUTE)
endif()

option(NESRECOMP_ENABLE_NET "Build recomp-net rollback netplay for game targets" OFF)
option(NESRECOMP_NET_ICE "Enable recomp-net ICE/libjuice WAN transport (not wired in the NES host yet)" OFF)

function(_nesrecomp_add_recomp_net)
    if(TARGET recomp_net)
        return()
    endif()
    if(NOT EXISTS "${NESRECOMP_RECOMP_NET_ROOT}/CMakeLists.txt")
        message(FATAL_ERROR
            "recomp-net is missing at ${NESRECOMP_RECOMP_NET_ROOT}. Run: "
            "git submodule update --init --recursive lib/recomp-net")
    endif()
    set(RNET_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(RNET_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(RNET_ENABLE_ICE ${NESRECOMP_NET_ICE} CACHE BOOL "" FORCE)
    add_subdirectory("${NESRECOMP_RECOMP_NET_ROOT}"
                     "${CMAKE_BINARY_DIR}/recomp-net-build" EXCLUDE_FROM_ALL)
    if(WIN32 AND NESRECOMP_NET_ICE)
        target_include_directories(recomp_net PRIVATE
            "${NESRECOMP_RUNNER_ROOT}/src/netplay/compat")
        target_compile_definitions(recomp_net PRIVATE
            JUICE_ERR_AGAIN=JUICE_ERR_NOT_AVAIL)
    endif()
    if(NOT TARGET recomp_net)
        message(FATAL_ERROR "recomp-net CMake did not create target recomp_net")
    endif()
endfunction()

# rbengine is C++ in its build; recomp-net is added first so rbengine finds it
# through RECOMP_NET_ROOT rather than re-adding the subdirectory.
function(_nesrecomp_add_rbengine)
    if(TARGET retcomm_rbengine)
        return()
    endif()
    if(NOT EXISTS "${NESRECOMP_RBENGINE_ROOT}/CMakeLists.txt")
        message(FATAL_ERROR
            "retcomm-rbengine is missing at ${NESRECOMP_RBENGINE_ROOT}. Run: "
            "git submodule update --init --recursive lib/retcomm-rbengine")
    endif()
    _nesrecomp_add_recomp_net()
    enable_language(CXX)
    set(RECOMP_NET_ROOT "${NESRECOMP_RECOMP_NET_ROOT}" CACHE PATH "" FORCE)
    set(RBE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${NESRECOMP_RBENGINE_ROOT}"
                     "${CMAKE_BINARY_DIR}/retcomm-rbengine-build" EXCLUDE_FROM_ALL)
    if(NOT TARGET retcomm_rbengine)
        message(FATAL_ERROR
            "retcomm-rbengine CMake did not create target retcomm_rbengine")
    endif()
endfunction()

function(nesrecomp_enable_recomp_net target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "nesrecomp_enable_recomp_net: '${target}' is not a target")
    endif()
    get_target_property(_done ${target} NESRECOMP_NET_ENABLED)
    if(_done)
        return()
    endif()
    set_target_properties(${target} PROPERTIES NESRECOMP_NET_ENABLED TRUE)
    _nesrecomp_add_recomp_net()
    _nesrecomp_add_rbengine()
    target_sources(${target} PRIVATE
        "${NESRECOMP_RUNNER_ROOT}/src/netplay/nes_netplay.c"
        "${NESRECOMP_RUNNER_ROOT}/src/netplay/nes_netplay_rb.c"
        "${NESRECOMP_RUNNER_ROOT}/src/netplay/nes_netplay_identity.c")
    target_include_directories(${target} PRIVATE
        "${NESRECOMP_RUNNER_ROOT}/src/netplay"
        "${NESRECOMP_RBENGINE_ROOT}/include")
    target_link_libraries(${target} PRIVATE recomp_net retcomm_rbengine)
    target_compile_definitions(${target} PRIVATE NESRECOMP_NET=1)

    # The release pin every peer compares (SHIPPING.md §2; the exe hash is
    # appended at runtime, so two trees never share an identity).
    if(NOT NESRECOMP_GAME_VERSION)
        execute_process(
            COMMAND git -C "${CMAKE_SOURCE_DIR}" describe --tags --always --dirty
            OUTPUT_VARIABLE _nes_ver OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _nes_ver_rc ERROR_QUIET)
        if(NOT _nes_ver_rc EQUAL 0 OR NOT _nes_ver)
            set(_nes_ver "dev")
        endif()
        set(NESRECOMP_GAME_VERSION "${_nes_ver}")
    endif()
    target_compile_definitions(${target} PRIVATE
        NESRECOMP_GAME_VERSION="${NESRECOMP_GAME_VERSION}")
    message(STATUS "NESRECOMP_NET: ${target} links recomp_net + retcomm_rbengine "
                   "(game_version ${NESRECOMP_GAME_VERSION})")

    # The lobby: recomp-ui's shared netplay backend, when the launcher is
    # linked. There is no NES copy to fall back to: a recomp-ui without the
    # backend is a configure error naming what to bump.
    if(DEFINED RECOMP_UI_ROOT AND EXISTS "${RECOMP_UI_ROOT}/src/recomp_launcher.h")
        if(NOT COMMAND recomp_target_launcher_netplay)
            message(FATAL_ERROR
                "nesrecomp_enable_recomp_net(${target}): recomp-ui at "
                "${RECOMP_UI_ROOT} has no recomp_target_launcher_netplay(). "
                "Bump recomp-ui to a revision carrying src/recomp_netplay_host.h "
                "(RetroPortingToolKit/recomp-ui b688ca7 or later).")
        endif()
        target_sources(${target} PRIVATE
            "${NESRECOMP_RUNNER_ROOT}/src/netplay/nes_host_lobby.c")
        target_include_directories(${target} PRIVATE "${RECOMP_UI_ROOT}/src")
        target_compile_definitions(${target} PRIVATE NES_HOST_HAS_RECOMP_UI=1)
        recomp_target_launcher_netplay(${target} RECOMP_NET_TARGET recomp_net)
    endif()
    if(WIN32)
        target_link_libraries(${target} PRIVATE ws2_32)
    endif()
endfunction()
