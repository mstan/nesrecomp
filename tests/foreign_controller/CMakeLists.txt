cmake_minimum_required(VERSION 3.16)
project(NESRecompForeignControllerSelftest C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(foreign_controller_selftest
    foreign_controller_selftest.c
    ../../runner/src/foreign_controller.c
)
target_include_directories(foreign_controller_selftest PRIVATE
    ../../runner/include
)
if(NOT MSVC)
    target_link_libraries(foreign_controller_selftest m)
endif()

enable_testing()
add_test(NAME foreign_controller_selftest COMMAND foreign_controller_selftest)
