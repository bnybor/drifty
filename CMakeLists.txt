cmake_minimum_required(VERSION 3.15)

project(drifty
    VERSION 0.1.0
    DESCRIPTION "Drift-tolerance convolutional coding"
    LANGUAGES C)

# Default to an optimized build. The decoders are numeric hot loops, so an
# unoptimized (-O0) build is several times slower; without this a plain
# `cmake -S . -B build` would inherit the empty default build type and compile
# at -O0. Only set it when we own the cache (top-level, single-config generator,
# no explicit choice), so a parent project's selection is never overridden.
if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (default Release)" FORCE)
endif()

option(DRIFTY_BUILD_TESTS "Build the drifty tests" ${PROJECT_IS_TOP_LEVEL})
option(DRIFTY_BUILD_BENCH "Build the drifty metrics and code-search harnesses" OFF)
option(DRIFTY_NATIVE "Tune the core for the building host's CPU (-march=native; not portable)" OFF)

# Size-focused flags for the MinSizeRel config (applied to the core and stdlib
# below, since this precedes their targets). The library is plain C with no
# exceptions, so the unwind tables (.eh_frame, ~17% of the object) and the
# compiler .ident string are dead weight; dropping them shrinks the archive
# notably. The trade-off is that debuggers/profilers cannot unwind through these
# frames - acceptable for a minimum-size release build.
add_compile_options($<$<CONFIG:MinSizeRel>:-fno-asynchronous-unwind-tables>
                    $<$<CONFIG:MinSizeRel>:-fno-unwind-tables>
                    $<$<CONFIG:MinSizeRel>:-fno-ident>)

add_subdirectory(stdlib)

add_library(drifty_interface INTERFACE)
target_include_directories(drifty_interface INTERFACE include)

# The freestanding core, compiled once as an OBJECT library so the same objects
# can be archived into both libraries below (the bare core, and the public core +
# stdlib) without compiling twice.
add_library(drifty_bare_obj OBJECT
    src/cc/ccode.c
    src/cc/hybrid/encode.c
    src/cc/hybrid/decode.c
    src/cc/hybrid.c
    src/cc/vindel/encode.c
    src/cc/vindel/decode.c
    src/cc/vindel.c
    src/cc/viterbi/encode.c
    src/cc/viterbi/decode.c
    src/cc/viterbi.c
    src/cc/bcjr/encode.c
    src/cc/bcjr/decode.c
    src/cc/bcjr.c
    src/cc/maxir/encode.c
    src/cc/maxir/decode.c
    src/cc/maxir.c
    src/cc/basic_encoder/encode.c
    src/cc/basic_encoder/basic_encoder.c)
target_compile_features(drifty_bare_obj PUBLIC c_std_11)
target_compile_options(drifty_bare_obj PRIVATE -ffreestanding -fno-builtin -Wall -Wextra -Wpedantic -Werror=implicit-function-declaration)
# Optional host-specific tuning. -march=native lets the numeric inner loops use
# the host's vector ISA (AVX2/AVX-512); -ffp-contract=off is REQUIRED alongside
# it, because enabling FMA would otherwise contract the per-step EWMA and could
# shift a lock/erase threshold - with contraction pinned off a native build stays
# bit-identical to the portable one. PRIVATE so -march never leaks to consumers.
if(DRIFTY_NATIVE)
  target_compile_options(drifty_bare_obj PRIVATE -march=native -ffp-contract=off)
endif()
target_link_libraries(drifty_bare_obj PUBLIC drifty_interface PRIVATE drifty_stdlib_interface)

# Only the public API - the symbols declared in the installed include/ headers -
# should be exported from the archives. The decoder shares internal helpers across
# translation units (dt_decode_step, dt_trellis_*, dt_decode_ctx_*), and the full
# library also bundles the stdlib shims (dt_malloc, dt_memcpy, dt_log, ...); both
# are internal plumbing that would otherwise land in the archive's symbol table.
# Because they are referenced across object boundaries, hiding them needs a partial
# link: combine an archive's objects into one relocatable object, then localize
# every *defined* symbol except the public API. Undefined references are left
# alone, so the bare archive still asks the consumer to supply the shims and the
# full archive still resolves libc at the consumer's final link.
# The public API is the abstract surface in include/: the dt_ccode descriptor
# (cc/ccode.h, implemented in src/cc/ccode.c), the standalone basic encoder
# (cc/encoders.h, dt_cc_basic_encoder_*) that every codec encodes through, plus
# the per-codec decoder factories dt_hybrid_* / dt_vindel_* / dt_viterbi_* /
# dt_bcjr_* / dt_maxir_* (cc/hybrid.h, cc/vindel.h, cc/viterbi.h, cc/bcjr.h,
# cc/maxir.h); bcjr and maxir also export their own encoder factories. Each
# codec's engine functions (dt_stream_* / dt_vindel_stream_* / dt_viterbi_stream_* /
# dt_bcjr_stream_* / dt_maxir_stream_*, dt_ccode_encode* / dt_vindel_encode* /
# dt_viterbi_encode* / dt_bcjr_encode* / dt_maxir_encode*, dt_basic_encode*) are
# private - their headers live under src/cc/<codec>/ - so they are localized,
# not exported.
set(_dt_public_symbols
    dt_ccode_create dt_ccode_create_standard dt_ccode_destroy
    dt_ccode_k dt_ccode_n
    dt_hybrid_decoder_create dt_hybrid_decoder_destroy
    dt_hybrid_soft_decoder_create dt_hybrid_soft_decoder_destroy
    dt_vindel_decoder_create dt_vindel_decoder_destroy
    dt_viterbi_decoder_create dt_viterbi_decoder_destroy
    dt_bcjr_encoder_create dt_bcjr_encoder_destroy
    dt_bcjr_decoder_create dt_bcjr_decoder_destroy
    dt_bcjr_soft_decoder_create dt_bcjr_soft_decoder_destroy
    dt_maxir_encoder_create dt_maxir_encoder_destroy
    dt_maxir_decoder_create dt_maxir_decoder_destroy
    dt_maxir_soft_decoder_create dt_maxir_soft_decoder_destroy
    dt_cc_basic_encoder_create dt_cc_basic_encoder_destroy)
string(REPLACE ";" "\n" _dt_public_symbols_text "${_dt_public_symbols}")
set(_dt_keep_file ${CMAKE_CURRENT_BINARY_DIR}/dt_public_symbols.txt)
file(WRITE ${_dt_keep_file} "${_dt_public_symbols_text}\n")

# Build STATIC library `name` from the given OBJECT-library targets, exporting only
# the public API (see above). Falls back to a plain multi-object archive where the
# binutils ld -r / objcopy are unavailable - that still works, it just leaves the
# internals exported.
function(dt_add_archive name)
    set(_obj_exprs "")
    foreach(_lib IN LISTS ARGN)
        list(APPEND _obj_exprs $<TARGET_OBJECTS:${_lib}>)
    endforeach()
    if(CMAKE_LINKER AND CMAKE_OBJCOPY)
        set(_merged ${CMAKE_CURRENT_BINARY_DIR}/${name}_merged.o)
        # In MinSizeRel, follow the localization with a strip of the local symbol
        # names that survived the partial link only because relocations referenced
        # them - they are dead weight in the archive. A separate pass so it runs
        # after, not interleaved with, the localization.
        set(_strip "")
        if(CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
            set(_strip COMMAND ${CMAKE_OBJCOPY} --strip-unneeded ${_merged})
        endif()
        add_custom_command(
            OUTPUT ${_merged}
            COMMAND ${CMAKE_LINKER} -r -o ${_merged} ${_obj_exprs}
            COMMAND ${CMAKE_OBJCOPY} --keep-global-symbols=${_dt_keep_file} ${_merged}
            ${_strip}
            DEPENDS ${ARGN} ${_dt_keep_file} ${_obj_exprs}
            COMMENT "Linking and localizing internal symbols for lib${name}.a"
            COMMAND_EXPAND_LISTS VERBATIM)
        set_source_files_properties(${_merged} PROPERTIES
            EXTERNAL_OBJECT TRUE GENERATED TRUE)
        add_library(${name} STATIC ${_merged})
        set_target_properties(${name} PROPERTIES LINKER_LANGUAGE C)
    else()
        add_library(${name} STATIC ${_obj_exprs})
    endif()
endfunction()

# The freestanding core on its own, as libdrifty_bare.a (no stdlib shims -
# the consumer supplies those, so they stay undefined here).
dt_add_archive(drifty_bare drifty_bare_obj)
target_link_libraries(drifty_bare PUBLIC drifty_interface)
target_compile_features(drifty_bare PUBLIC c_std_11)

# The public library: the core and the stdlib shims archived together into one
# self-contained libdrifty.a. libm is carried through for the shims' math.
dt_add_archive(drifty drifty_bare_obj drifty_stdlib)
target_link_libraries(drifty PUBLIC drifty_interface PRIVATE m)
target_compile_features(drifty PUBLIC c_std_11)
add_library(drifty::drifty ALIAS drifty)

if(DRIFTY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(DRIFTY_BUILD_BENCH)
    add_subdirectory(metrics)
    add_subdirectory(bench)
endif()

include(GNUInstallDirs)

install(TARGETS drifty drifty_bare
    EXPORT drifty-targets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
