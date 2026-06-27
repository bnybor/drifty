add_executable(dt_maxir_metrics dt_maxir_metrics.c)
target_link_libraries(dt_maxir_metrics PRIVATE drifty::drifty)

# Fan the independent work out across cores when OpenMP is available. Without it
# the code still builds and runs single-threaded (the pragmas are simply
# ignored).
find_package(OpenMP)
if(OpenMP_C_FOUND)
    target_link_libraries(dt_maxir_metrics PRIVATE OpenMP::OpenMP_C)
endif()
