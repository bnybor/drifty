# Example programs for drifty. Each lives in its own subfolder as main.c and links
# the public library (drifty::drifty), using only the public headers under
# include/drifty/. The shared examples/util.h (channel simulators, drive loops) is
# reachable via the examples/ include dir below - it is example scaffolding, not
# part of the drifty API.

set(DRIFTY_EXAMPLES
    01_basics
    02_custom_code
    03_decoders
    04_soft_output
    05_symbols
    06_acquisition
    07_block_rs251
    08_frames_naive
    09_frames_marker
    10_concatenated
    11_detect
    12_pipe
    13_funnel
    14_pipeline)

foreach(ex ${DRIFTY_EXAMPLES})
  # Most examples are a single main.c; some (e.g. 14_pipeline) split across files.
  file(GLOB ex_src ${CMAKE_CURRENT_SOURCE_DIR}/${ex}/*.c)
  add_executable(${ex} ${ex_src})
  target_link_libraries(${ex} PRIVATE drifty::drifty)
  target_include_directories(${ex} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  set_target_properties(${ex} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${ex})
endforeach()
