cmake_minimum_required(VERSION 3.16)
project(NESRecomp C)

set(CMAKE_C_STANDARD 11)

# Pure C11, zero external dependencies
add_executable(NESRecomp
    src/main_nes.c
    src/rom_parser.c
    src/cpu6502_decoder.c
    src/function_finder.c
    src/code_generator.c
    src/annotations.c
    src/symbol_table.c
    src/game_config.c
    src/toml.c
)

target_include_directories(NESRecomp PRIVATE src)

# Output directly to build/recompiler/
set_target_properties(NESRecomp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

if(MSVC)
    target_compile_options(NESRecomp PRIVATE /W3 /WX-)
endif()
