cmake_minimum_required(VERSION 3.16)
project(NESRecompMapperSelftest C)

set(CMAKE_C_STANDARD 11)
enable_testing()

add_executable(mapper_selftest
    mapper_selftest.c
    ../../runner/src/mapper.c
)
target_include_directories(mapper_selftest PRIVATE ../../runner/include)
add_test(NAME mapper_selftest COMMAND mapper_selftest)
