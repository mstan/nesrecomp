cmake_minimum_required(VERSION 3.20)
project(AccuracyCoinRecomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cyc.cmake)

# generated/accuracycoin_cyc.c (dispatch) plus one accuracycoin_cyc_bNN.c per
# 8KB PRG bank come from: NESRecomp AccuracyCoin.nes --game game.toml
file(GLOB ACCURACYCOIN_GEN CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/generated/accuracycoin_cyc*.c")
add_executable(AccuracyCoinRecomp ${NESRECOMP_CYC_SOURCES} ${ACCURACYCOIN_GEN})
target_include_directories(AccuracyCoinRecomp PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS})
target_link_libraries(AccuracyCoinRecomp PRIVATE ${NESRECOMP_CYC_LIBRARIES})
nesrecomp_cyc_enable_sdl(AccuracyCoinRecomp)
if(MSVC)
    target_compile_definitions(AccuracyCoinRecomp PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(AccuracyCoinRecomp PRIVATE /W3 /bigobj)
endif()

nesrecomp_cyc_add_oracle(cyc_oracle)

add_executable(cyc_helper_test ${NESRECOMP_CYC_HELPER_TEST_SOURCES})
target_include_directories(cyc_helper_test PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS})
