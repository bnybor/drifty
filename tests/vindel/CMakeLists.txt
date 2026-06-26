# The heavier suites drive embarrassingly parallel Monte-Carlo loops; OpenMP fans
# those across cores. It is optional - without it the `#pragma omp` lines are
# ignored and the tests simply run single-threaded.
find_package(OpenMP COMPONENTS C QUIET)

# The vindel tests exercise the private engine API (dt_ccode / dt_vindel_stream),
# so they link the non-localized engine objects directly (not the localized
# drifty::drifty archive) and reach the private headers under src/ via -Isrc.
# drifty_bare_obj propagates the public include dir; drifty_stdlib supplies the
# freestanding shims and propagates libm.
foreach(t vindel_decoder_test)
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE drifty_bare_obj drifty_stdlib)
    target_include_directories(${t} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    if(OpenMP_C_FOUND)
        target_link_libraries(${t} PRIVATE OpenMP::OpenMP_C)
    endif()
    add_test(NAME ${t} COMMAND ${t})
endforeach()
