cmake_minimum_required(VERSION 3.16)
project(NESRecompExternalRomGateSelftest C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(external_rom_gate_selftest
    external_rom_gate_selftest.cpp
    ../../runner/src/mod_runtime.cpp
    ../../runner/src/crc32.c
)
target_include_directories(external_rom_gate_selftest PRIVATE
    ../../runner/include
)
if(MSVC)
    target_compile_options(external_rom_gate_selftest PRIVATE /utf-8)
endif()

enable_testing()
add_test(NAME external_rom_gate_selftest COMMAND external_rom_gate_selftest)
