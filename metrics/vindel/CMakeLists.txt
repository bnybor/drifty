# The vindel harness reports the decoder's per-bit lock probability, which is
# only on the private engine API (vindel/decode.h) - the public vindel decoder is
# hard-decision only. So, like the vindel tests, it links the non-localized engine
# objects directly (not the drifty::drifty archive the hybrid harness uses) and
# reaches the private headers under src/ via -Isrc. drifty_bare_obj propagates the
# public include dir; drifty_stdlib supplies the freestanding shims and libm.
add_executable(dt_vindel_metrics dt_vindel_metrics.c)
target_link_libraries(dt_vindel_metrics PRIVATE drifty_bare_obj drifty_stdlib)
target_include_directories(dt_vindel_metrics PRIVATE ${PROJECT_SOURCE_DIR}/src)

# Fan the independent work out across cores when OpenMP is available. Without it
# the code still builds and runs single-threaded (the pragmas are simply
# ignored).
find_package(OpenMP)
if(OpenMP_C_FOUND)
    target_link_libraries(dt_vindel_metrics PRIVATE OpenMP::OpenMP_C)
endif()
