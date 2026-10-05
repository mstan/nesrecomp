# Framework HD packages: MODS uses the common host, with no game callbacks.
# Without MODS this retains the explicit legacy folder/config adapter.
function(nesrecomp_cyc_enable_hdpack target)
    cmake_parse_arguments(HD "MODS" "" "" ${ARGN})
    set(cyc_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    target_sources(${target} PRIVATE
        "${cyc_dir}/cyc_hdpack.c"
        "${cyc_dir}/../src/hdpack.c"
        "${cyc_dir}/../src/chr_codec.c")
    target_include_directories(${target} PRIVATE "${cyc_dir}/../include" "${cyc_dir}/../src")
    target_compile_definitions(${target} PRIVATE NESRECOMP_CYCLE_HDPACK)
    if(HD_MODS)
        target_compile_definitions(${target} PRIVATE NESRECOMP_CYCLE_HDPACK_MODS)
    endif()
endfunction()
