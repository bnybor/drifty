# The detect_full tests use only the public API (dt_cc_detect_full_soft_*), so they
# need no private -Isrc headers; they link the engine objects (drifty_bare_obj)
# plus the freestanding shims (drifty_stdlib).
foreach(t detect_full_decoder_test)
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE drifty_bare_obj drifty_stdlib)
    add_test(NAME ${t} COMMAND ${t})
endforeach()
