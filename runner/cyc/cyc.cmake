# runner/cyc/cyc.cmake - sources for NESRecomp --cycle-accurate game projects.
#
#   set(NESRECOMP_ROOT <path to nesrecomp>)
#   include(${NESRECOMP_ROOT}/runner/cyc/cyc.cmake)
#   add_executable(MyGame ${NESRECOMP_CYC_SOURCES} generated/<prefix>_cyc.c)
#   target_include_directories(MyGame PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS})
#   target_link_libraries(MyGame PRIVATE ${NESRECOMP_CYC_LIBRARIES})
#   nesrecomp_cyc_enable_sdl(MyGame)      # optional window and audio (runner/external/SDL2)
#   nesrecomp_cyc_add_oracle(cyc_oracle)  # optional TriCNES reference executable (needs CXX)
#
# The runtime is C11. Only the oracle is C++17.

set(NESRECOMP_CYC_DIR ${CMAKE_CURRENT_LIST_DIR})

set(NESRECOMP_CYC_SOURCES
    ${NESRECOMP_CYC_DIR}/cpu6502.c
    ${NESRECOMP_CYC_DIR}/cpu6502_interp.c
    ${NESRECOMP_CYC_DIR}/hw_machine.c
    ${NESRECOMP_CYC_DIR}/hw_mapper.c
    ${NESRECOMP_CYC_DIR}/vendor/emu2413/emu2413.c
    ${NESRECOMP_CYC_DIR}/hw_ppu.c
    ${NESRECOMP_CYC_DIR}/hw_apu.c
    ${NESRECOMP_CYC_DIR}/hw_palette.c
    ${NESRECOMP_CYC_DIR}/cyc_trace.c
    ${NESRECOMP_CYC_DIR}/cyc_run.c
    ${NESRECOMP_CYC_DIR}/cyc_host.c
    ${NESRECOMP_CYC_DIR}/cyc_accuracycoin.c
    ${NESRECOMP_CYC_DIR}/cyc_png.c
)
set(NESRECOMP_CYC_INCLUDE_DIRS ${NESRECOMP_CYC_DIR})
set(NESRECOMP_CYC_LIBRARIES "")
if(UNIX)
    # The C runtime's NTSC palette uses sin/cos. C++ oracle linking can hide
    # this dependency, but plain C executables need libm explicitly on Linux.
    list(APPEND NESRECOMP_CYC_LIBRARIES m)
endif()

# The TriCNES oracle: TriCNES's complete machine with the same host, writing
# the same --hash-out/--trace-out files as a recompiled build. Used only to
# check NESRecomp's implementation; it needs no generated code.
function(nesrecomp_cyc_add_oracle target)
    add_executable(${target}
        ${NESRECOMP_CYC_DIR}/tric_core.cpp
        ${NESRECOMP_CYC_DIR}/cyc_trace.c
        ${NESRECOMP_CYC_DIR}/cyc_host.c
        ${NESRECOMP_CYC_DIR}/cyc_accuracycoin.c
        ${NESRECOMP_CYC_DIR}/cyc_png.c
    )
    target_compile_definitions(${target} PRIVATE CYC_ORACLE)
    target_include_directories(${target} PRIVATE ${NESRECOMP_CYC_DIR})
    if(MSVC)
        # The ported core keeps C#'s implicit narrowing conversions.
        target_compile_options(${target} PRIVATE /W3 /wd4244 /wd4267 /wd4305 /wd4838 /wd4309 /bigobj)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endfunction()

# Exhaustive check of the CPU operation helpers in cpu6502.h. Build it with
# the game's compiler and flags.
set(NESRECOMP_CYC_HELPER_TEST_SOURCES ${NESRECOMP_CYC_DIR}/cyc_helper_test.c)

# Adds the SDL2 window and audio (cyc_sdl.c) to a host executable if SDL2 is
# found. Without it the executable is headless only.
function(nesrecomp_cyc_enable_sdl target)
    list(APPEND CMAKE_PREFIX_PATH "${NESRECOMP_CYC_DIR}/../external/SDL2/cmake")
    find_package(SDL2 CONFIG QUIET)
    if(NOT SDL2_FOUND)
        message(STATUS "${target}: SDL2 not found, building headless only")
        return()
    endif()
    target_sources(${target} PRIVATE ${NESRECOMP_CYC_DIR}/cyc_sdl.c)
    target_compile_definitions(${target} PRIVATE CYC_WITH_SDL)
    target_link_libraries(${target} PRIVATE SDL2::SDL2)
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:${target}>)
    endif()
endfunction()
