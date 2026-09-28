# The Famicom Disk System BIOS alone, recompiled (the "BIOS showcase"): the RAM
# Adapter's disksys.rom with an empty drive, 100% native. Supply your own dump
# at bios/disksys.rom (never committed); it must match bios/disksys.toml.
#
#   cmake -S runner/cyc/fds-bios -B build/fds-bios [-DNESRECOMP_FDS_BIOS=path]
#   cmake --build build/fds-bios --config Release
#   build/fds-bios/fds-bios            (windowed; headless options as any cycle program)
#
# See runner/cyc/README.md, "BIOS showcase".
cmake_minimum_required(VERSION 3.20)
project(NESRecompFdsBios C)
get_filename_component(NESRECOMP_REPO "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(NESRECOMP_FDS_BIOS "${NESRECOMP_REPO}/bios/disksys.rom" CACHE FILEPATH
    "The FDS BIOS (your own dump); its identity file is <stem>.toml beside it")
set(NESRECOMP_HOST_COMPILER "" CACHE FILEPATH "Prebuilt host NESRecomp (optional)")
option(NESRECOMP_HEADLESS "Build without SDL window/audio support" OFF)
if(NOT EXISTS "${NESRECOMP_FDS_BIOS}")
    message(FATAL_ERROR "No FDS BIOS at ${NESRECOMP_FDS_BIOS}: supply your own dump of disksys.rom there "
                        "(or -DNESRECOMP_FDS_BIOS=path); bios/disksys.toml says which image")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../project.cmake")
set(options "")
if(NESRECOMP_HEADLESS)
    list(APPEND options HEADLESS)
endif()
nesrecomp_add_fds_bios(fds-bios BIOS "${NESRECOMP_FDS_BIOS}" RECOMPILER "${NESRECOMP_HOST_COMPILER}" ${options})
