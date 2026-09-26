cmake_minimum_required(VERSION 3.20)
project(nesrecomp_cyc C CXX)

# Standalone build of the pieces that need no generated code:
#   cyc_oracle       TriCNES's complete machine (the reference)
#   cyc_interp       NESRecomp's CPU on the interpreter alone, for any NROM ROM
#   cyc_helper_test  exhaustive check of the cpu6502.h operation helpers
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

add_executable(cyc_mapper_test mapper_test.c hw_mapper.c cyc_trace.c)
target_include_directories(cyc_mapper_test PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS})
add_test(NAME cyc_mapper_test COMMAND cyc_mapper_test)
