# The bcjr tests exercise the private engine API (dt_ccode / dt_bcjr_stream and
# the dt_bcjr_* factories), so they link the non-localized engine objects
# directly (not the localized drifty::drifty archive) and reach the private
# headers under src/ via -Isrc. drifty_bare_obj propagates the public include
# dir; drifty_stdlib supplies the freestanding shims and propagates libm. (No
# OpenMP: the bcjr suite has no parallel Monte-Carlo loops.)
foreach(t bcjr_decoder_test)
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE drifty_bare_obj drifty_stdlib)
    target_include_directories(${t} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME ${t} COMMAND ${t})
endforeach()
