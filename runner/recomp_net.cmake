# Optional recomp-net delay-synchronised netcode for NESRecomp games.

if(NOT NESRECOMP_RECOMP_NET_ROOT)
    get_filename_component(NESRECOMP_RECOMP_NET_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/../lib/recomp-net" ABSOLUTE)
endif()

option(NESRECOMP_ENABLE_NET "Build recomp-net for game targets" OFF)
option(NESRECOMP_NET_ICE "Enable recomp-net ICE/libjuice WAN transport" OFF)

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
        # Compatibility for the pinned recomp-net/libjuice combination. The
        # upstream ICE file currently uses pthread mutex names and the older
        # JUICE_ERR_AGAIN spelling.
        target_include_directories(recomp_net PRIVATE
            "${NESRECOMP_RUNNER_ROOT}/src/netplay/compat")
        target_compile_definitions(recomp_net PRIVATE
            JUICE_ERR_AGAIN=JUICE_ERR_NOT_AVAIL)
    endif()
endfunction()

function(nesrecomp_enable_recomp_net target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "nesrecomp_enable_recomp_net: '${target}' is not a target")
    endif()
    _nesrecomp_add_recomp_net()
    target_sources(${target} PRIVATE
        "${NESRECOMP_RUNNER_ROOT}/src/netplay/nes_netplay.c"
        "${NESRECOMP_RUNNER_ROOT}/src/netplay/nes_launcher_netplay.c"
        "${NESRECOMP_RUNNER_ROOT}/src/lobby/nes_lobby_client.c"
        "${NESRECOMP_RUNNER_ROOT}/src/lobby/ws/rnet_ws.c"
        "${NESRECOMP_RUNNER_ROOT}/src/lobby/ws/rnet_sha1.c")
    target_include_directories(${target} PRIVATE
        "${NESRECOMP_RUNNER_ROOT}/src/netplay"
        "${NESRECOMP_RUNNER_ROOT}/src/lobby"
        "${NESRECOMP_RUNNER_ROOT}/src/lobby/ws")
    target_link_libraries(${target} PRIVATE recomp_net)
    target_compile_definitions(${target} PRIVATE NESRECOMP_NET=1 NES_HAS_LOBBY_CLIENT=1)
    if(WIN32)
        target_link_libraries(${target} PRIVATE ws2_32)
    endif()
endfunction()

if(NESRECOMP_ENABLE_NET)
    _nesrecomp_add_recomp_net()
    list(APPEND NESRECOMP_RUNNER_SOURCES
        "${NESRECOMP_RUNNER_ROOT}/src/netplay/nes_netplay.c"
        "${NESRECOMP_RUNNER_ROOT}/src/netplay/nes_launcher_netplay.c"
        "${NESRECOMP_RUNNER_ROOT}/src/lobby/nes_lobby_client.c"
        "${NESRECOMP_RUNNER_ROOT}/src/lobby/ws/rnet_ws.c"
        "${NESRECOMP_RUNNER_ROOT}/src/lobby/ws/rnet_sha1.c")
    list(APPEND NESRECOMP_RUNNER_LIBRARIES recomp_net)
    list(APPEND NESRECOMP_RUNNER_INCLUDE_DIRS
        "${NESRECOMP_RUNNER_ROOT}/src/netplay"
        "${NESRECOMP_RUNNER_ROOT}/src/lobby"
        "${NESRECOMP_RUNNER_ROOT}/src/lobby/ws")
    add_compile_definitions(NESRECOMP_NET=1 NES_HAS_LOBBY_CLIENT=1)
    if(WIN32)
        list(APPEND NESRECOMP_RUNNER_LIBRARIES ws2_32)
    endif()
endif()
