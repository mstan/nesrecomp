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
#   cyc_fds_boot_test the FDS boot skip and auto insert on the whole machine
#   cyc_console_audio_test / cyc_console_audio_pcm  the console output stage
#                    (nes / famicom): defaults, responses, pinned renders
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
set(CYC_TEST_BOARD_SOURCES hw_mapper.c hw_fds.c hw_fds_audio.c hw_fds_hle.c hw_fds_boot.c vendor/emu2413/emu2413.c)

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
    hw_ppu.c hw_apu.c hw_palette.c cyc_trace.c cyc_ring.c cyc_run.c cyc_fds_skip.c cyc_ramview.c cyc_native_none.c)
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

# The console output stage (hw_apu.c, cyc_set_console): board defaults, each
# model's frequency and step response against its documented RC sections, and
# no effect on the machine. The FDS default uses the board fixtures' disk.
add_executable(cyc_console_audio_test console_audio_test.c cpu6502.c cpu6502_interp.c hw_machine.c ${CYC_TEST_BOARD_SOURCES}
    hw_ppu.c hw_apu.c hw_palette.c cyc_trace.c cyc_ring.c cyc_run.c cyc_fds_skip.c cyc_ramview.c
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
# The FDS boot skip and auto insert (cyc_fds_skip.c, hw_fds_boot.c,
# common/nes_fds_boot.h) on the whole machine: a synthetic BIOS that boots the
# way disksys.rom does (tools/cyc/fds_boot_fixtures.py), disks exercising every
# LoadFiles rule and every refusal. The machine at the game's first
# instruction, skipped against the BIOS's own boot, at all four alignments;
# native = --interp-only = cyc_interp with the skip on; refusals run as HLE off;
# auto insert equals a host insert at the same frame; config precedence.
add_test(NAME cyc_fds_boot_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/cyc/test_cyc_fds_boot.py
            --recompiler $<TARGET_FILE:NESRecomp> --interp $<TARGET_FILE:cyc_interp>
            --out ${CMAKE_CURRENT_BINARY_DIR}/fds_boot_test ${NESRECOMP_NESTED_BUILD_ARGS})
set_tests_properties(cyc_fds_boot_test PROPERTIES TIMEOUT 1800)
# The console output stage end to end (tools/cyc/test_cyc_console_audio.py):
# board defaults in cyc_interp, the nes and famicom renders pinned by SHA-256,
# their spectra against the models, and game.toml [game] console compiled in.
add_test(NAME cyc_console_audio_pcm
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/cyc/test_cyc_console_audio.py
            --recompiler $<TARGET_FILE:NESRecomp> --interp $<TARGET_FILE:cyc_interp>
            --out ${CMAKE_CURRENT_BINARY_DIR}/console_audio_test ${NESRECOMP_NESTED_BUILD_ARGS})
set_tests_properties(cyc_console_audio_pcm PROPERTIES TIMEOUT 1800)
