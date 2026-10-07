cmake_minimum_required(VERSION 3.20)
project(cycle_diagnostics_test C)
if(NOT RECOMP_UI_ROOT)
    message(FATAL_ERROR "RECOMP_UI_ROOT is required for launcher provider tests")
endif()
add_executable(cycle_diagnostics_test diagnostics_test.c ../../runner/cyc/cyc_diagnostics.c)
target_include_directories(cycle_diagnostics_test PRIVATE ../../runner/cyc
    "${RECOMP_UI_ROOT}/src" "${RECOMP_UI_ROOT}/src/common")
target_compile_definitions(cycle_diagnostics_test PRIVATE NESRECOMP_ENABLE_MODS=1 CYC_WITH_RECOMP_UI
    _CRT_SECURE_NO_WARNINGS CYC_BUILD_VERSION="test")
if(MSVC)
    target_compile_options(cycle_diagnostics_test PRIVATE /Zi)
    target_link_options(cycle_diagnostics_test PRIVATE /DEBUG)
endif()
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
enable_testing()
add_test(NAME cycle_diagnostics COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_diagnostics.py"
    --exe $<TARGET_FILE:cycle_diagnostics_test> --out "${CMAKE_CURRENT_BINARY_DIR}/evidence")
