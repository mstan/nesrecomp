cmake_minimum_required(VERSION 3.20)
project(CycStress C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cyc.cmake)

# generated/cyc_stress_cyc.c (dispatch) plus one cyc_stress_cyc_bNN.c per 8KB
# PRG bank come from: NESRecomp cyc_stress.nes --game game.toml
file(GLOB CYC_STRESS_GEN CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/generated/cyc_stress_cyc*.c")
add_executable(cyc_stress ${NESRECOMP_CYC_SOURCES} ${CYC_STRESS_GEN})
target_include_directories(cyc_stress PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS})
if(MSVC)
    target_compile_definitions(cyc_stress PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(cyc_stress PRIVATE /W3 /bigobj)
endif()

nesrecomp_cyc_add_oracle(cyc_oracle)
