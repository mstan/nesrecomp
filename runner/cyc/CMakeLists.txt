cmake_minimum_required(VERSION 3.20)
project(nesrecomp_cyc C CXX)

# Standalone build of the pieces that need no generated code:
#   cyc_oracle       TriCNES's complete machine (the reference)
#   cyc_interp       NESRecomp's CPU on the interpreter alone, for any NROM ROM
#   cyc_helper_test  exhaustive check of the cpu6502.h operation helpers
#   cyc_fds_disk_test  FDS media parser/stream contracts (common/nes_fds.h) and
#                    disk save file / Mesen .ips contracts (common/nes_fds_save.h)
#                    over fixtures written by tools/cyc/fds_fixtures.py
#   cyc_fds_saves    disk saves across processes (tools/cyc/test_cyc_fds_saves.py)
#   cyc_fds_board_test FDS RAM Adapter/drive contracts and a synthetic BIOS
#                    (tools/cyc/fds_board_fixtures.py) on the whole machine
#   cyc_ramview_test compiled RAM views never run stale code (builds the
#                    recompiler and a synthetic self-modifying FDS program)
#   cyc_fds_audio_test the FDS sound unit against an independent model and in
#                    rendered PCM (builds synthetic FDS sound programs)
#   cyc_fds_hle_plan_test the FDS HLE plan (common/nes_fds_hle.h)
#   cyc_fds_hle_test the FDS HLE tier on the whole machine: auto swap and fast
#                    load with a synthetic BIOS and multi-side disks
#   cyc_console_audio_test / cyc_console_audio_pcm  the console output stage
#                    (nes / famicom): defaults, responses, pinned renders
#   cyc_disk_action_test  the two-press Disk action's state machine
#   cyc_input_test   the window's input actions, bindings and config.ini
#                    (needs SDL2 for key and controller names; no window)
#   cyc_window_run   the window over its TCP server, hidden, dummy video driver:
#                    bindings, the Disk action and toast, the controller, the
#                    menu (with recomp-ui), HLE settings, dev vs production
#   cyc_disk_action_run  the Disk action on the whole machine (cyc_interp):
#                    swaps, peeks, time-outs, cycling, and the toast never in
#                    --hash-out / --frame-log / screenshots
# Tests that build a generated program use this build's toolchain
# (nested_build.json below, tools/cyc/nested_build.py).
# Game projects use runner/cyc/cyc.cmake instead.

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

include(${CMAKE_CURRENT_SOURCE_DIR}/cyc.cmake)

nesrecomp_cyc_add_oracle(cyc_oracle)

add_executable(cyc_interp ${NESRECOMP_CYC_SOURCES} cyc_native_none.c)
target_include_directories(cyc_interp PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS})
target_link_libraries(cyc_interp PRIVATE ${NESRECOMP_CYC_LIBRARIES})
if(MSVC)
    target_compile_definitions(cyc_interp PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(cyc_helper_test ${NESRECOMP_CYC_HELPER_TEST_SOURCES})
target_include_directories(cyc_helper_test PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS})
enable_testing()
add_test(NAME cyc_helper_test COMMAND cyc_helper_test)

# Every cartridge board source; each test that links the boards uses this list.
set(CYC_TEST_BOARD_SOURCES hw_mapper.c hw_fds.c hw_fds_audio.c hw_fds_hle.c vendor/emu2413/emu2413.c)

add_executable(cyc_mapper_test mapper_test.c ${CYC_TEST_BOARD_SOURCES} cyc_ring.c cyc_trace.c)
target_link_libraries(cyc_mapper_test PRIVATE ${NESRECOMP_CYC_LIBRARIES})
target_include_directories(cyc_mapper_test PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS})
add_test(NAME cyc_mapper_test COMMAND cyc_mapper_test)

add_executable(cyc_cart_header_test cart_header_test.c)
add_test(NAME cyc_cart_header_test COMMAND cyc_cart_header_test)

# The FDS HLE tier's pure decisions (common/nes_fds_hle.h): request parsing,
# source precedence, the plan over every request x capability, disk-ID matching.
add_executable(cyc_fds_hle_plan_test fds_hle_plan_test.c)
add_test(NAME cyc_fds_hle_plan_test COMMAND cyc_fds_hle_plan_test)

# The player's Disk action (cyc_disk_action.h): peek, swap, time-out, cycling,
# retargeting, a swap behind a disk write, an empty drive, the menu's choice.
add_executable(cyc_disk_action_test disk_action_test.c cyc_disk_action.c)
if(MSVC)
    target_compile_definitions(cyc_disk_action_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME cyc_disk_action_test COMMAND cyc_disk_action_test)

# The window's input actions and their persistence (cyc_input.h, cyc_settings.h).
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
add_test(NAME cyc_bad_game_config COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/cyc/test_bad_game_config.py" --compiler $<TARGET_FILE:NESRecomp>)
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../external/SDL2/cmake")
find_package(SDL2 CONFIG QUIET)
if(SDL2_FOUND)
    include("${CMAKE_CURRENT_SOURCE_DIR}/hdpack.cmake")
    add_executable(cyc_hdpack_probe ${NESRECOMP_CYC_SOURCES} cyc_native_none.c hdpack_probe.c cyc_presentation.c)
    target_include_directories(cyc_hdpack_probe PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS})
    target_link_libraries(cyc_hdpack_probe PRIVATE ${NESRECOMP_CYC_LIBRARIES})
    if(MSVC)
        target_compile_definitions(cyc_hdpack_probe PRIVATE _CRT_SECURE_NO_WARNINGS SDL_MAIN_HANDLED)
    endif()
    nesrecomp_cyc_enable_sdl(cyc_hdpack_probe NO_DEV_UI)
    nesrecomp_cyc_enable_hdpack(cyc_hdpack_probe)
    add_test(NAME cyc_hdpack COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/cyc/test_cyc_hdpack.py"
        --host $<TARGET_FILE:cyc_hdpack_probe> --out "${CMAKE_CURRENT_BINARY_DIR}/hdpack-test")
    add_test(NAME cyc_hdpack_window COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/cyc/test_cyc_hdpack_window.py"
        --host $<TARGET_FILE:cyc_hdpack_probe> --out "${CMAKE_CURRENT_BINARY_DIR}/hdpack-window-test")
    add_executable(cyc_input_test input_test.c cyc_input.c cyc_settings.c)
    target_include_directories(cyc_input_test PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS})
    target_link_libraries(cyc_input_test PRIVATE SDL2::SDL2)
    target_compile_definitions(cyc_input_test PRIVATE SDL_MAIN_HANDLED)
    if(MSVC)
        target_compile_definitions(cyc_input_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    if(WIN32)
        add_custom_command(TARGET cyc_input_test POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:cyc_input_test>)
    endif()
    add_test(NAME cyc_input_test COMMAND cyc_input_test ${CMAKE_CURRENT_BINARY_DIR})
    add_executable(cyc_input_four_test input_test.c cyc_input.c cyc_settings.c)
    target_include_directories(cyc_input_four_test PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS})
    target_link_libraries(cyc_input_four_test PRIVATE SDL2::SDL2)
    target_compile_definitions(cyc_input_four_test PRIVATE SDL_MAIN_HANDLED CYC_INPUT_PLAYERS=4 _CRT_SECURE_NO_WARNINGS)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/input-four")
    add_test(NAME cyc_input_four_test COMMAND cyc_input_four_test "${CMAKE_CURRENT_BINARY_DIR}/input-four")
    # The window itself, on the standalone interpreter host: production and dev
    # builds, driven over TCP on a hidden window, SDL dummy video (never the desktop)
    # by tools/cyc/test_cyc_window.py. -DNESRECOMP_RECOMP_UI=<recomp-ui> adds the
    # runtime menu (and its checks) to the production one.
    set(NESRECOMP_RECOMP_UI "" CACHE STRING "recomp-ui checkout for cyc_window's runtime menu (optional)")
    add_executable(cyc_window ${NESRECOMP_CYC_SOURCES} cyc_native_none.c)
    add_executable(cyc_window_dev ${NESRECOMP_CYC_SOURCES} cyc_native_none.c)
    foreach(t cyc_window cyc_window_dev)
        target_include_directories(${t} PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS})
        target_link_libraries(${t} PRIVATE ${NESRECOMP_CYC_LIBRARIES})
        if(MSVC)
            target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS)
        endif()
    endforeach()
    nesrecomp_cyc_enable_sdl(cyc_window NO_DEV_UI)
    nesrecomp_cyc_enable_sdl(cyc_window_dev DEV_UI)
    set(window_ui "")
    if(NESRECOMP_RECOMP_UI)
        include("${NESRECOMP_RECOMP_UI}/recomp_ui.cmake")
        nesrecomp_cyc_enable_recomp_ui(cyc_window)
        set(window_ui --recomp-ui)
        # The host's side of the launcher against a scripted launcher: what it
        # hands over, and every edit coming back into config.ini.
        foreach(t cyc_ui_launcher_test cyc_ui_launcher_hd_test)
        add_executable(${t} ui_launcher_test.c cyc_ui_launcher.c cyc_settings.c cyc_input.c
            cyc_fds_bios.c)
        target_include_directories(${t} PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS}
            "${NESRECOMP_RECOMP_UI}/src" "${NESRECOMP_RECOMP_UI}/src/common")
        target_link_libraries(${t} PRIVATE SDL2::SDL2)
        target_compile_definitions(${t} PRIVATE SDL_MAIN_HANDLED _CRT_SECURE_NO_WARNINGS
            "CYC_GAME_PASSWORD_SAVE=\"test-password.srm\"" "CYC_GAME_PASSWORD_SAVE_LABEL=\"Mantra\""
            "CYC_LAUNCHER_ROM_SHA256=\"ab00000000000000000000000000000000000000000000000000000000000001\"")
        if(WIN32)
            add_custom_command(TARGET ${t} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:${t}>)
        endif()
        add_test(NAME ${t} COMMAND ${t} ${CMAKE_CURRENT_BINARY_DIR})
        endforeach()
        target_compile_definitions(cyc_ui_launcher_hd_test PRIVATE NESRECOMP_CYCLE_HDPACK)
    endif()
    add_test(NAME cyc_window_run
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/cyc/test_cyc_window.py
                --window $<TARGET_FILE:cyc_window> --dev-window $<TARGET_FILE:cyc_window_dev> ${window_ui}
                --out ${CMAKE_CURRENT_BINARY_DIR}/window_run)
    set_tests_properties(cyc_window_run PROPERTIES TIMEOUT 900)
    add_test(NAME cyc_cart_saves
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/cyc/test_cyc_cart_saves.py
                --exe $<TARGET_FILE:cyc_window> --out ${CMAKE_CURRENT_BINARY_DIR}/cart_saves)
    set_tests_properties(cyc_cart_saves PROPERTIES TIMEOUT 120)
else()
    message(STATUS "SDL2 not found: cyc_input_test and cyc_window are not built")
endif()

# FDS media: the fixture generator runs first (FIXTURES_SETUP), then the test.
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
add_executable(cyc_fds_disk_test fds_disk_test.c)
if(MSVC)
    target_compile_definitions(cyc_fds_disk_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
set(NESRECOMP_FDS_FIXTURES ${CMAKE_CURRENT_BINARY_DIR}/fds_fixtures)
add_test(NAME cyc_fds_fixtures
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/cyc/fds_fixtures.py
            --out ${NESRECOMP_FDS_FIXTURES})
set_tests_properties(cyc_fds_fixtures PROPERTIES FIXTURES_SETUP fds_fixtures)
add_test(NAME cyc_fds_disk_test COMMAND cyc_fds_disk_test ${NESRECOMP_FDS_FIXTURES})
set_tests_properties(cyc_fds_disk_test PROPERTIES FIXTURES_REQUIRED fds_fixtures)

# FDS RAM Adapter and drive: register/IRQ/drive contracts, then a synthetic
# BIOS (tools/cyc/fds_board_fixtures.py) run on the whole machine.
add_executable(cyc_fds_board_test fds_board_test.c cpu6502.c cpu6502_interp.c hw_machine.c ${CYC_TEST_BOARD_SOURCES}
    hw_ppu.c hw_apu.c hw_palette.c cyc_trace.c cyc_ring.c ${NESRECOMP_CYC_SCHEDULER_SOURCES} cyc_native_none.c)
target_include_directories(cyc_fds_board_test PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS})
target_link_libraries(cyc_fds_board_test PRIVATE ${NESRECOMP_CYC_LIBRARIES})
if(MSVC)
    target_compile_definitions(cyc_fds_board_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
set(NESRECOMP_FDS_BOARD_FIXTURES ${CMAKE_CURRENT_BINARY_DIR}/fds_board_fixtures)
add_test(NAME cyc_fds_board_fixtures
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/cyc/fds_board_fixtures.py
            --out ${NESRECOMP_FDS_BOARD_FIXTURES})
set_tests_properties(cyc_fds_board_fixtures PROPERTIES FIXTURES_SETUP fds_board_fixtures)
add_test(NAME cyc_fds_board_test COMMAND cyc_fds_board_test ${NESRECOMP_FDS_BOARD_FIXTURES})
set_tests_properties(cyc_fds_board_test PROPERTIES FIXTURES_REQUIRED fds_board_fixtures)

# The presented width (cyc_video.h, common/nes_video_geometry.h): every mode's
# width, Fit's clamp, queuing after the window exists, the apply point.
add_executable(cyc_video_test video_test.c cyc_video.c cpu6502.c cpu6502_interp.c hw_machine.c ${CYC_TEST_BOARD_SOURCES}
    hw_ppu.c hw_apu.c hw_palette.c cyc_trace.c cyc_ring.c ${NESRECOMP_CYC_SCHEDULER_SOURCES} cyc_native_none.c)
target_include_directories(cyc_video_test PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS})
target_link_libraries(cyc_video_test PRIVATE ${NESRECOMP_CYC_LIBRARIES})
if(MSVC)
    target_compile_definitions(cyc_video_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME cyc_video_test COMMAND cyc_video_test)

# The light gun: connector bits, beam timing, sensor decay, offscreen aim,
# and save/snapshot restoration, including refusal of a damaged sensor state.
add_executable(cyc_zapper_test zapper_test.c cpu6502.c cpu6502_interp.c hw_machine.c ${CYC_TEST_BOARD_SOURCES}
    hw_ppu.c hw_apu.c hw_palette.c cyc_trace.c cyc_ring.c ${NESRECOMP_CYC_SCHEDULER_SOURCES} cyc_native_none.c)
target_include_directories(cyc_zapper_test PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS})
target_link_libraries(cyc_zapper_test PRIVATE ${NESRECOMP_CYC_LIBRARIES})
if(MSVC)
    target_compile_definitions(cyc_zapper_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME cyc_zapper_test COMMAND cyc_zapper_test)

# The console output stage (hw_apu.c, cyc_set_console): board defaults, each
# model's frequency and step response against its documented RC sections, and
# no effect on the machine. The FDS default uses the board fixtures' disk.
add_executable(cyc_console_audio_test console_audio_test.c cpu6502.c cpu6502_interp.c hw_machine.c ${CYC_TEST_BOARD_SOURCES}
    hw_ppu.c hw_apu.c hw_palette.c cyc_trace.c cyc_ring.c ${NESRECOMP_CYC_SCHEDULER_SOURCES}
    cyc_native_none.c)
target_include_directories(cyc_console_audio_test PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS})
target_link_libraries(cyc_console_audio_test PRIVATE ${NESRECOMP_CYC_LIBRARIES})
if(MSVC)
    target_compile_definitions(cyc_console_audio_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME cyc_console_audio_test COMMAND cyc_console_audio_test ${NESRECOMP_FDS_BOARD_FIXTURES})
set_tests_properties(cyc_console_audio_test PROPERTIES FIXTURES_REQUIRED fds_board_fixtures)

# Compiled views of code in RAM (FDS disk files, captured code): a synthetic
# FDS program whose RAM code rewrites itself every way a view can go stale
# (tools/cyc/fds_ramview_fixtures.py), compiled from its disk, then again with
# the capture of a run; native, --interp-only and cyc_interp must match on
# every cycle at all four alignments. Needs the recompiler, built here.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../recompiler ${CMAKE_CURRENT_BINARY_DIR}/recompiler)

# Tests that compile a generated program build it with this build's toolchain,
# recorded here (tools/cyc/nested_build.py): cmake, generator, make program,
# compiler and tools, and for MSVC with a single-configuration generator the
# compiler environment this was configured in, so a plain ctest needs no
# developer prompt. The CTest commands only name this file.
function(nesrecomp_json_string out value)
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\\\"" value "${value}")
    set(${out} "\"${value}\"" PARENT_SCOPE)
endfunction()
set(NESRECOMP_NESTED_BUILD ${CMAKE_CURRENT_BINARY_DIR}/nested_build.json)
set(nested_fields "")
foreach(pair "cmake=${CMAKE_COMMAND}" "generator=${CMAKE_GENERATOR}" "platform=${CMAKE_GENERATOR_PLATFORM}"
             "toolset=${CMAKE_GENERATOR_TOOLSET}" "instance=${CMAKE_GENERATOR_INSTANCE}")
    string(FIND "${pair}" "=" eq)
    string(SUBSTRING "${pair}" 0 ${eq} key)
    math(EXPR eq "${eq} + 1")
    string(SUBSTRING "${pair}" ${eq} -1 value)
    nesrecomp_json_string(value "${value}")
    string(APPEND nested_fields "  \"${key}\": ${value},\n")
endforeach()
if(CMAKE_CONFIGURATION_TYPES)
    string(APPEND nested_fields "  \"multi_config\": true\n")
else()
    nesrecomp_json_string(make "${CMAKE_MAKE_PROGRAM}")
    nesrecomp_json_string(cc "${CMAKE_C_COMPILER}")
    string(APPEND nested_fields "  \"multi_config\": false,\n  \"make_program\": ${make},\n  \"c_compiler\": ${cc},\n")
    set(tools "")
    set(path_dirs "")
    foreach(pair "linker=${CMAKE_LINKER}" "rc=${CMAKE_RC_COMPILER}" "mt=${CMAKE_MT}" "ar=${CMAKE_AR}")
        string(FIND "${pair}" "=" eq)
        string(SUBSTRING "${pair}" 0 ${eq} key)
        math(EXPR eq "${eq} + 1")
        string(SUBSTRING "${pair}" ${eq} -1 value)
        if(value)
            nesrecomp_json_string(json "${value}")
            list(APPEND tools "\"${key}\": ${json}")
        endif()
    endforeach()
    list(JOIN tools ", " tools)
    string(APPEND nested_fields "  \"tools\": {${tools}},\n")
    set(env "")
    if(MSVC)
        foreach(tool "${CMAKE_C_COMPILER}" "${CMAKE_LINKER}" "${CMAKE_RC_COMPILER}" "${CMAKE_MT}")
            if(tool)
                get_filename_component(dir "${tool}" DIRECTORY)
                file(TO_NATIVE_PATH "${dir}" dir)
                nesrecomp_json_string(dir "${dir}")
                list(APPEND path_dirs "${dir}")
            endif()
        endforeach()
        list(REMOVE_DUPLICATES path_dirs)
        # Built as a string: the values are ;-separated and must not become CMake lists.
        foreach(var INCLUDE LIB LIBPATH)
            if(DEFINED ENV{${var}})
                nesrecomp_json_string(json "$ENV{${var}}")
                if(env)
                    string(APPEND env ", ")
                endif()
                string(APPEND env "\"${var}\": ${json}")
            endif()
        endforeach()
    endif()
    list(JOIN path_dirs ", " path_dirs)
    string(APPEND nested_fields "  \"env\": {${env}},\n  \"path\": [${path_dirs}]\n")
endif()
file(WRITE ${NESRECOMP_NESTED_BUILD} "{\n${nested_fields}}\n")
set(NESRECOMP_NESTED_BUILD_ARGS --toolchain ${NESRECOMP_NESTED_BUILD} --config "$<CONFIG>")

add_test(NAME cyc_ramview_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/cyc/test_cyc_ramviews.py
            --recompiler $<TARGET_FILE:NESRecomp> --interp $<TARGET_FILE:cyc_interp>
            --out ${CMAKE_CURRENT_BINARY_DIR}/ramview_test ${NESRECOMP_NESTED_BUILD_ARGS})
set_tests_properties(cyc_ramview_test PROPERTIES TIMEOUT 1800)

# The mod surface (tools/cyc/test_cyc_mods.py, a synthetic FDS program from
# tools/cyc/fds_mod_fixtures.py and the test plugin tests/mods/mod_test_extras.c):
# hook sites keyed on the code in RAM, native = interpreter, isolated calls
# that change nothing, committed calls, save states with mod records, and a
# plugin at an undeclared site refused.
add_test(NAME cyc_mods_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/cyc/test_cyc_mods.py
            --recompiler $<TARGET_FILE:NESRecomp> --out ${CMAKE_CURRENT_BINARY_DIR}/mods_test ${NESRECOMP_NESTED_BUILD_ARGS})
set_tests_properties(cyc_mods_test PROPERTIES TIMEOUT 1800)

add_test(NAME cyc_extra_timing
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/cyc/test_cyc_extra_timing.py
            --host $<TARGET_FILE:cyc_interp> --out ${CMAKE_CURRENT_BINARY_DIR}/extra_timing_test)
set_tests_properties(cyc_extra_timing PROPERTIES TIMEOUT 300)

# FDS disk saves across processes: a synthetic program that saves to its disk
# (tools/cyc/fds_save_fixtures.py) run by cyc_interp at all four alignments:
# write -> save file -> reload round trips, the image never modified, foreign /
# corrupt / newer saves refused and left intact, save cadence, write protect,
# Mesen .ips export/import and the write position.
add_test(NAME cyc_fds_saves
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/cyc/test_cyc_fds_saves.py
            --host $<TARGET_FILE:cyc_interp> --out ${CMAKE_CURRENT_BINARY_DIR}/fds_saves)
set_tests_properties(cyc_fds_saves PROPERTIES TIMEOUT 900)
# The FDS sound unit (hw_fds_audio.c): synthetic FDS programs that exercise
# every register and behaviour the CPU can observe, compiled from their disks;
# native, --interp-only and cyc_interp must match on every cycle at all four
# alignments, every sound register read and every frame's sound state must
# match an independent model of Mesen's FdsAudio (tools/cyc/fds_audio_model.py),
# and the rendered PCM must have the expected pitch and FDS/2A03 level.
add_test(NAME cyc_fds_audio_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/cyc/test_cyc_fds_audio.py
            --recompiler $<TARGET_FILE:NESRecomp> --interp $<TARGET_FILE:cyc_interp>
            --out ${CMAKE_CURRENT_BINARY_DIR}/fds_audio_test ${NESRECOMP_NESTED_BUILD_ARGS})
set_tests_properties(cyc_fds_audio_test PROPERTIES TIMEOUT 1800)
# The FDS HLE tier (hw_fds_hle.c, common/nes_fds_hle.h) on the whole machine: a
# synthetic BIOS-and-game program whose disk-ID check anchor its identity file
# declares (tools/cyc/fds_hle_fixtures.py), with 2- and 4-sided disks, one per
# scenario (a program that asks the BIOS for side B, one that waits for the
# player, one that never asks, a drive poller, ambiguous / unmatched / multi-disk
# / already-inserted requests, a game that reads the header itself). Compiled
# with and without game.toml [fds] hle; auto swap on and off at all four
# alignments, native = --interp-only = cyc_interp, fast load hashing exactly as
# its absence, config precedence, refusals, a host disk change, --realtime.
add_test(NAME cyc_fds_hle_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/cyc/test_cyc_fds_hle.py
            --recompiler $<TARGET_FILE:NESRecomp> --interp $<TARGET_FILE:cyc_interp>
            --out ${CMAKE_CURRENT_BINARY_DIR}/fds_hle_test ${NESRECOMP_NESTED_BUILD_ARGS})
set_tests_properties(cyc_fds_hle_test PROPERTIES TIMEOUT 1800)
# The Disk action end to end on the standalone interpreter host
# (tools/cyc/test_cyc_disk_action.py, the synthetic program and 2- and 4-sided
# disks of fds_hle_fixtures.py): the swap reaches a program waiting for side B
# through the normal drive path (byte-identical to --fds-event), a peek or a
# timed-out press changes nothing, presses cycle every side of a two-disk
# image, and the toast shows in --present-out but never in the picture,
# --hash-out, --frame-log or the ring, at all four alignments.
add_test(NAME cyc_disk_action_run
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/cyc/test_cyc_disk_action.py
            --interp $<TARGET_FILE:cyc_interp> --out ${CMAKE_CURRENT_BINARY_DIR}/disk_action_run)
set_tests_properties(cyc_disk_action_run PROPERTIES TIMEOUT 900)
# The console output stage end to end (tools/cyc/test_cyc_console_audio.py):
# board defaults in cyc_interp, the nes and famicom renders pinned by SHA-256,
# their spectra against the models, and game.toml [game] console compiled in.
add_test(NAME cyc_console_audio_pcm
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/cyc/test_cyc_console_audio.py
            --recompiler $<TARGET_FILE:NESRecomp> --interp $<TARGET_FILE:cyc_interp>
            --out ${CMAKE_CURRENT_BINARY_DIR}/console_audio_test ${NESRECOMP_NESTED_BUILD_ARGS})
set_tests_properties(cyc_console_audio_pcm PROPERTIES TIMEOUT 1800)

# PAL master-clock ratio, frame/APU timing, opcode-only DMA and state phase.
add_executable(cyc_region_test region_test.c cpu6502.c cpu6502_interp.c hw_machine.c ${CYC_TEST_BOARD_SOURCES}
    hw_ppu.c hw_apu.c hw_palette.c cyc_trace.c cyc_ring.c ${NESRECOMP_CYC_SCHEDULER_SOURCES} cyc_native_none.c)
target_include_directories(cyc_region_test PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS})
target_link_libraries(cyc_region_test PRIVATE ${NESRECOMP_CYC_LIBRARIES})
if(MSVC)
    target_compile_definitions(cyc_region_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME cyc_region_test COMMAND cyc_region_test)

add_executable(cyc_content_test content_test.c cpu6502.c cpu6502_interp.c hw_machine.c ${CYC_TEST_BOARD_SOURCES}
    hw_ppu.c hw_apu.c hw_palette.c cyc_trace.c cyc_ring.c ${NESRECOMP_CYC_SCHEDULER_SOURCES} cyc_native_none.c)
target_include_directories(cyc_content_test PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS})
target_link_libraries(cyc_content_test PRIVATE ${NESRECOMP_CYC_LIBRARIES})
if(MSVC)
    target_compile_definitions(cyc_content_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME cyc_content_test COMMAND cyc_content_test)

add_executable(cyc_return_test return_test.c cpu6502.c cpu6502_interp.c hw_machine.c ${CYC_TEST_BOARD_SOURCES}
    hw_ppu.c hw_apu.c hw_palette.c cyc_trace.c cyc_ring.c ${NESRECOMP_CYC_SCHEDULER_SOURCES} cyc_native_none.c)
target_include_directories(cyc_return_test PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS})
target_link_libraries(cyc_return_test PRIVATE ${NESRECOMP_CYC_LIBRARIES})
if(MSVC)
    target_compile_definitions(cyc_return_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME cyc_return_test COMMAND cyc_return_test)

add_executable(cyc_ram_policy_test ram_policy_test.c cpu6502.c cpu6502_interp.c hw_machine.c ${CYC_TEST_BOARD_SOURCES}
    hw_ppu.c hw_apu.c hw_palette.c cyc_trace.c cyc_ring.c ${NESRECOMP_CYC_SCHEDULER_SOURCES} cyc_native_none.c)
target_include_directories(cyc_ram_policy_test PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS})
target_link_libraries(cyc_ram_policy_test PRIVATE ${NESRECOMP_CYC_LIBRARIES})
if(MSVC)
    target_compile_definitions(cyc_ram_policy_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME cyc_ram_policy_test COMMAND cyc_ram_policy_test)
