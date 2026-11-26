# Shared host services; NES hardware, serializers and policies remain local.
include_guard(GLOBAL)
set(NESRECOMP_SHARED_CORE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../external/shared-cores" CACHE PATH
    "Pinned shared runtime provider source root")
set(_nes_saved_testing "${BUILD_TESTING}")
set(BUILD_TESTING OFF)
foreach(_provider mod persistence input diagnostics)
    add_subdirectory("${NESRECOMP_SHARED_CORE_ROOT}/recomp-${_provider}-core"
                     "${CMAKE_BINARY_DIR}/shared/${_provider}" EXCLUDE_FROM_ALL)
endforeach()
set(BUILD_TESTING "${_nes_saved_testing}")
add_library(nes_shared_runtime INTERFACE)
target_link_libraries(nes_shared_runtime INTERFACE recomp-mod-core::core
    recomp-persistence-core::core recomp-input::core recomp-diagnostics-core::core)
# Existing standalone fixtures compile the same production source files.
link_libraries(nes_shared_runtime)
