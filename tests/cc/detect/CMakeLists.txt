# The detect tests use only the public detect API (dt_cc_detect_soft_*), so they
# need no private -Isrc headers; they link the engine objects (drifty_bare_obj)
# plus the freestanding shims (drifty_stdlib). detect is a soft-only STUB - extend
# this with round-trip tests once the decode engine is implemented.
foreach(t detect_decoder_test)
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE drifty_bare_obj drifty_stdlib)
    add_test(NAME ${t} COMMAND ${t})
endforeach()
