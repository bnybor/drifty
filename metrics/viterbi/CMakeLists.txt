# The viterbi harness measures edit distance only (the codec has no lock metric),
# so it can drive the public API and link the localized drifty::drifty archive -
# like the hybrid harness, and unlike vindel's (which needs the private engine
# for the lock probability).
add_executable(dt_viterbi_metrics dt_viterbi_metrics.c)
target_link_libraries(dt_viterbi_metrics PRIVATE drifty::drifty)

# Fan the independent work out across cores when OpenMP is available.
find_package(OpenMP)
if(OpenMP_C_FOUND)
    target_link_libraries(dt_viterbi_metrics PRIVATE OpenMP::OpenMP_C)
endif()
