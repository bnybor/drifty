# The bcjr harness measures edit distance only (bcjr emits a lock probability via
# its soft decoder, but this coarse harness does not sweep it), so it drives the
# public API and links the localized drifty::drifty archive - like the hybrid and
# viterbi harnesses, and unlike vindel's (which needs the private engine).
add_executable(dt_bcjr_metrics dt_bcjr_metrics.c)
target_link_libraries(dt_bcjr_metrics PRIVATE drifty::drifty)

# Fan the independent work out across cores when OpenMP is available.
find_package(OpenMP)
if(OpenMP_C_FOUND)
    target_link_libraries(dt_bcjr_metrics PRIVATE OpenMP::OpenMP_C)
endif()
