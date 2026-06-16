# launcher.cmake — RmlUi pre-boot GUI launcher integration for NES game targets.
#
# The runner.cmake source list builds the console ROM resolver (launcher.c). This
# file adds the optional graphical launcher (launcher_gui.cpp + RmlUi + the SDL/GL3
# backend), which the SNES project only ever wired into MSBuild — NES builds with
# CMake + Ninja, so the integration is authored here.
#
# Usage from a game project's CMakeLists.txt, after add_executable(<target> ...):
#   include(${NESRECOMP_ROOT}/runner/launcher.cmake)
#   nesrecomp_add_launcher(<target>)
#
# Requires SDL2 already linked on <target>. Pulls in OpenGL + the vendored RmlUi /
# FreeType static libs (nesrecomp/lib) and defines NESRECOMP_LAUNCHER=1 so
# launcher.c compiles the nes_launcher_run_window() call.

set(NESRECOMP_LAUNCHER_ROOT ${CMAKE_CURRENT_LIST_DIR})
get_filename_component(NESRECOMP_LIB_DIR "${NESRECOMP_LAUNCHER_ROOT}/../lib" ABSOLUTE)

function(nesrecomp_add_launcher TARGET)
    find_package(OpenGL REQUIRED)

    # Build the vendored RmlUi (rmlui_core) + FreeType (freetype) static libs once
    # per build tree. Guarded so multiple targets / re-includes don't double-add.
    if(NOT TARGET rmlui_core)
        add_subdirectory("${NESRECOMP_LIB_DIR}" "${CMAKE_BINARY_DIR}/launcher-deps")
    endif()

    set(_rml "${NESRECOMP_LIB_DIR}/RmlUi")
    target_sources(${TARGET} PRIVATE
        ${NESRECOMP_LAUNCHER_ROOT}/src/launcher/launcher_gui.cpp
        ${_rml}/Backends/RmlUi_Renderer_GL3.cpp
        ${_rml}/Backends/RmlUi_Platform_SDL.cpp
    )
    target_include_directories(${TARGET} PRIVATE
        ${_rml}/Include
        ${_rml}/Backends
        ${NESRECOMP_LAUNCHER_ROOT}/src/launcher
    )
    target_link_libraries(${TARGET} PRIVATE rmlui_core freetype OpenGL::GL)
    target_compile_definitions(${TARGET} PRIVATE
        NESRECOMP_LAUNCHER=1
        RMLUI_STATIC_LIB
        RMLUI_NO_THIRDPARTY_CONTAINERS
    )

    # Stage the launcher assets (launcher.rml + fonts) next to the exe so the GUI
    # can load them at runtime.
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${NESRECOMP_LAUNCHER_ROOT}/src/launcher/assets"
            "$<TARGET_FILE_DIR:${TARGET}>/launcher"
        COMMENT "Staging launcher assets -> launcher/")
endfunction()
