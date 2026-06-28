# The naive frame-codec tests use only the public frame API (dt_fc_naive_frame_*,
# the frame interfaces, dt_bit, and dt_soft_bit), so they link the engine objects
# but need no private -Isrc headers. drifty_bare_obj propagates the public include
# dir; drifty_stdlib supplies the freestanding shims.
foreach(t naive_test)
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE drifty_bare_obj drifty_stdlib)
    add_test(NAME ${t} COMMAND ${t})
endforeach()
