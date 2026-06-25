add_library(drifty_stdlib_interface INTERFACE)
target_include_directories(drifty_stdlib_interface INTERFACE include)

# OBJECT library so its compiled object is archived directly into the public
# drifty library.
add_library(drifty_stdlib OBJECT src/stdlib.c)
target_link_libraries(drifty_stdlib PUBLIC m drifty_stdlib_interface)
