# Nestopia libretro core — static library. No dependencies.
# Games set NESTOPIA_DIR before including this file (or it defaults to game/nestopia-core).
# Oracle accessor patches are auto-applied from nestopia_oracle.patch if needed.

if(NOT DEFINED NESTOPIA_DIR)
    set(NESTOPIA_DIR ${CMAKE_SOURCE_DIR}/nestopia-core)
endif()
set(NESTOPIA_CORE_DIR ${NESTOPIA_DIR}/source/core)

# Auto-apply oracle patches if not already applied
set(_ORACLE_PATCH "${CMAKE_CURRENT_LIST_DIR}/nestopia_oracle.patch")
if(EXISTS "${_ORACLE_PATCH}")
    execute_process(
        COMMAND git -C "${NESTOPIA_DIR}" apply --check "${_ORACLE_PATCH}"
        RESULT_VARIABLE _PATCH_CHECK_RC
        ERROR_QUIET
    )
    if(_PATCH_CHECK_RC EQUAL 0)
        message(STATUS "Applying nestopia oracle patches...")
        execute_process(
            COMMAND git -C "${NESTOPIA_DIR}" apply "${_ORACLE_PATCH}"
            RESULT_VARIABLE _PATCH_RC
        )
        if(NOT _PATCH_RC EQUAL 0)
            message(WARNING "Failed to apply nestopia oracle patch")
        endif()
    else()
        message(STATUS "nestopia oracle patches already applied")
    endif()
endif()

file(GLOB_RECURSE NESTOPIA_CORE_SOURCES ${NESTOPIA_CORE_DIR}/*.cpp)
list(FILTER NESTOPIA_CORE_SOURCES EXCLUDE REGEX "NstVideoFilterxBR\\.cpp$")
list(FILTER NESTOPIA_CORE_SOURCES EXCLUDE REGEX "NstVideoFilter2xSaI\\.cpp$")
list(FILTER NESTOPIA_CORE_SOURCES EXCLUDE REGEX "NstVideoFilterHqX\\.cpp$")
list(FILTER NESTOPIA_CORE_SOURCES EXCLUDE REGEX "NstVideoFilterScaleX\\.cpp$")
list(FILTER NESTOPIA_CORE_SOURCES EXCLUDE REGEX "NstZlib\\.cpp$")

file(GLOB NESTOPIA_NTSC_SOURCES ${NESTOPIA_DIR}/source/nes_ntsc/*.cpp)

add_library(nestopia_core STATIC
    ${NESTOPIA_CORE_SOURCES}
    ${NESTOPIA_NTSC_SOURCES}
    ${NESTOPIA_DIR}/libretro/libretro.cpp
)

target_include_directories(nestopia_core PUBLIC
    ${NESTOPIA_DIR}
    ${NESTOPIA_DIR}/source
    ${NESTOPIA_DIR}/libretro
    ${NESTOPIA_DIR}/libretro/libretro-common/include
)

target_compile_definitions(nestopia_core PRIVATE _CRT_SECURE_NO_WARNINGS NST_NO_ZLIB NST_NO_XBR)

if(MSVC)
    target_compile_options(nestopia_core PRIVATE /W2 /WX- /EHsc /wd4244 /wd4267 /wd4018 /wd4996 /wd4305)
endif()
