add_executable(dt_metrics dt_metrics.c)
target_link_libraries(dt_metrics PRIVATE drifty::drifty)

# Fan the independent work out across cores when OpenMP is available. Without it
# the code still builds and runs single-threaded (the pragmas are simply
# ignored).
find_package(OpenMP)
if(OpenMP_C_FOUND)
    target_link_libraries(dt_metrics PRIVATE OpenMP::OpenMP_C)
endif()
