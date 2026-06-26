# Selects the standard codes' generator polynomials (see src/hybrid/encode.c).
# Uses the private engine API directly, so link the non-localized engine objects
# and reach the private headers under src/ via -Isrc (see tests/CMakeLists.txt).
add_executable(dt_codesearch dt_codesearch.c)
target_link_libraries(dt_codesearch PRIVATE drifty_bare_obj drifty_stdlib)
target_include_directories(dt_codesearch PRIVATE ${PROJECT_SOURCE_DIR}/src)

# Fan the independent work out across cores when OpenMP is available. Without it
# the code still builds and runs single-threaded (the pragmas are simply
# ignored).
find_package(OpenMP)
if(OpenMP_C_FOUND)
    target_link_libraries(dt_codesearch PRIVATE OpenMP::OpenMP_C)
endif()
