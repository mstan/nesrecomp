cmake_minimum_required(VERSION 3.16)
project(ppu_hle_contract C)
enable_testing()
add_executable(ppu_hle_kernel_test ppu_hle_kernel_test.c)
target_include_directories(ppu_hle_kernel_test PRIVATE ../../runner/cyc)
target_compile_features(ppu_hle_kernel_test PRIVATE c_std_11)
if(MSVC)
    target_compile_options(ppu_hle_kernel_test PRIVATE /W4 /WX)
else()
    target_compile_options(ppu_hle_kernel_test PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME packed_rows_and_sprite_lanes COMMAND ppu_hle_kernel_test)

# A game-neutral interpreter host exercises the actual shared machine, not a
# mock renderer, against ROM-free cartridge/register/IRQ fixtures.
include(../../runner/cyc/cyc.cmake)
list(REMOVE_ITEM NESRECOMP_CYC_SOURCES ${NESRECOMP_CYC_DIR}/cyc_host_extras_none.c)
add_executable(ppu_contract_host ${NESRECOMP_CYC_SOURCES}
    ../../runner/cyc/cyc_native_none.c ../../runner/src/crc32.c contract_host_extras.c)
target_include_directories(ppu_contract_host PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS})
target_link_libraries(ppu_contract_host PRIVATE ${NESRECOMP_CYC_LIBRARIES})
target_compile_features(ppu_contract_host PRIVATE c_std_11)
# The pinned FDS source has an existing missing trace declaration; use the
# same declaration-only workaround as the preceding title capture.
if(MSVC)
    target_compile_options(ppu_contract_host PRIVATE /FI${NESRECOMP_CYC_DIR}/cyc_trace.h)
else()
    target_compile_options(ppu_contract_host PRIVATE -include ${NESRECOMP_CYC_DIR}/cyc_trace.h)
endif()
