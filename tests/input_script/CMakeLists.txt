cmake_minimum_required(VERSION 3.20)
project(NESRecompInputScriptSelftest C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../runner/external/SDL2/cmake")
find_package(SDL2 REQUIRED)

add_executable(input_script_selftest
    input_script_selftest.c
    ../../runner/src/input_script.c
)
target_include_directories(input_script_selftest PRIVATE
    ../../runner/include
    ${SDL2_INCLUDE_DIRS}
)
target_compile_definitions(input_script_selftest PRIVATE SDL_MAIN_HANDLED)
target_link_libraries(input_script_selftest PRIVATE ${SDL2_LIBRARIES})

enable_testing()
add_test(NAME input_script_selftest COMMAND input_script_selftest
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
if(WIN32)
    # SDL2.dll lives next to the import library in the vendored tree.
    get_target_property(_sdl_dll SDL2::SDL2 IMPORTED_LOCATION)
    if(_sdl_dll)
        add_custom_command(TARGET input_script_selftest POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_sdl_dll}"
                    "$<TARGET_FILE_DIR:input_script_selftest>")
    endif()
endif()
