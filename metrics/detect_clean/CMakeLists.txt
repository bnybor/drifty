add_executable(dt_detect_clean_metrics dt_detect_clean_metrics.c)
target_link_libraries(dt_detect_clean_metrics PRIVATE drifty::drifty)

# Fan the independent sweep points out across cores when OpenMP is available.
# Without it the harness still builds and runs single-threaded.
find_package(OpenMP)
if(OpenMP_C_FOUND)
    target_link_libraries(dt_detect_clean_metrics PRIVATE OpenMP::OpenMP_C)
endif()
