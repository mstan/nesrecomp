# HD presentation for a cycle game that already links cyc_presentation.c.
# The game's extras call cyc_hdpack_power_on() and cyc_hdpack_present().
function(nesrecomp_cyc_enable_hdpack target)
    set(cyc_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    target_sources(${target} PRIVATE
        "${cyc_dir}/cyc_hdpack.c"
        "${cyc_dir}/../src/hdpack.c"
        "${cyc_dir}/../src/chr_codec.c")
    target_include_directories(${target} PRIVATE "${cyc_dir}/../include" "${cyc_dir}/../src")
    target_compile_definitions(${target} PRIVATE NESRECOMP_CYCLE_HDPACK)
endfunction()
