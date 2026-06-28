# The rs251 block-codec tests use only the public block API (dt_rs_rs251_block_*,
# the block interfaces, and dt_bit), so they link the engine objects but need no
# private -Isrc headers. drifty_bare_obj propagates the public include dir;
# drifty_stdlib supplies the freestanding shims.
foreach(t rs251_block_test rs251_block_soft_test)
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE drifty_bare_obj drifty_stdlib)
    add_test(NAME ${t} COMMAND ${t})
endforeach()
