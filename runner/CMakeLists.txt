cmake_minimum_required(VERSION 3.16)
project(NESRecompGame C)

set(CMAKE_C_STANDARD 11)

# SDL2 — pre-configured, no FetchContent needed
set(SDL2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/SDL2/cmake")

find_package(SDL2 REQUIRED)

add_executable(NESRecompGame
    src/main_runner.c
    src/runtime.c
    src/apu.c
    src/ppu_renderer.c
    src/mapper.c
    src/logger.c
    src/input_script.c
    src/savestate.c
    ../generated/faxanadu_full.c
    ../generated/faxanadu_dispatch.c
)

target_include_directories(NESRecompGame PRIVATE
    include
    src
    ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(NESRecompGame PRIVATE ${SDL2_LIBRARIES})

# Copy SDL2 DLL to output directory on Windows
if(WIN32)
    add_custom_command(TARGET NESRecompGame POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/external/SDL2/lib/x64/SDL2.dll"
            "$<TARGET_FILE_DIR:NESRecompGame>"
    )
endif()

set_target_properties(NESRecompGame PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

if(MSVC)
    target_compile_options(NESRecompGame PRIVATE /W3 /WX-)
    # Suppress unreferenced label warnings (generated code has many goto labels)
    target_compile_options(NESRecompGame PRIVATE /wd4102)
endif()
