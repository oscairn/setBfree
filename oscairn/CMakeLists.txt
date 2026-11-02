# Oscairn's CLAP organ around setBfree's engine (doc/oscairn-plan.md).
# cmake -S oscairn -B build && cmake --build build && ctest --test-dir build
cmake_minimum_required(VERSION 3.21)
project(oscairn-organ VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(SETBFREE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH
    "setBfree tree whose engine sources to build (the stock-vs-fork gate points it at upstream)")
option(SETBFREE_STOCK "Build against upstream's engine API (global sample rate)" OFF)
option(SETBFREE_LIBC_RAND "Use libc rand() in the tone generator, as upstream does" OFF)
option(OSCAIRN_ORGAN_TESTS "Build the tests" ON)

set(S "${SETBFREE_SOURCE_DIR}")
add_library(setbfree_engine STATIC
    ${S}/src/midi.c ${S}/src/cfgParser.c ${S}/src/program.c ${S}/src/vibrato.c
    ${S}/src/state.c ${S}/src/tonegen.c ${S}/src/pgmParser.c
    ${S}/b_whirl/eqcomp.c ${S}/b_whirl/whirl.c ${S}/b_overdrive/overdrive.c ${S}/b_reverb/reverb.c
    engine.c engine.h)
target_include_directories(setbfree_engine PUBLIC . PRIVATE ${S}/src ${S}/b_overdrive ${S}/b_reverb ${S}/b_whirl)
# HAVE_MEMSTREAM: the system's open_memstream, never src/memstream.c (GPL-3.0, plan section 3)
target_compile_definitions(setbfree_engine PRIVATE HAVE_MEMSTREAM LV2SYNTH VERSION="oscairn-${PROJECT_VERSION}")
if(SETBFREE_STOCK)
    target_compile_definitions(setbfree_engine PRIVATE SETBFREE_STOCK)
endif()
if(SETBFREE_LIBC_RAND)
    target_compile_definitions(setbfree_engine PRIVATE SETBFREE_LIBC_RAND)
endif()
target_link_libraries(setbfree_engine PUBLIC $<$<NOT:$<PLATFORM_ID:Darwin>>:m>)

# Renders the gate's fixed performance to raw float (tools/gate-render.c).
add_executable(setbfree-gate-render tools/gate-render.c)
target_link_libraries(setbfree-gate-render PRIVATE setbfree_engine)

# The CLAP bundle: the organ and the Leslie effect (entry.cpp). CLAP's headers and Oscairn's extension headers (both MIT) are vendored in
# third_party/ with the commits they came from.
add_library(oscairn_organ_headers INTERFACE)
target_include_directories(oscairn_organ_headers INTERFACE third_party/clap/include third_party/oscairn-clap/include)

add_library(oscairn_organ_plugin STATIC plugin.cpp plugin.h params.h leslie.cpp leslie.h)
target_link_libraries(oscairn_organ_plugin PUBLIC oscairn_organ_headers setbfree_engine)

add_library(oscairn_organ MODULE entry.cpp)
target_link_libraries(oscairn_organ PRIVATE oscairn_organ_plugin)
set_target_properties(oscairn_organ PROPERTIES PREFIX "" SUFFIX ".clap" OUTPUT_NAME setbfree
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
if(APPLE)
    target_link_options(oscairn_organ PRIVATE -Wl,-exported_symbol,_clap_entry)
endif()
install(TARGETS oscairn_organ LIBRARY DESTINATION lib/clap)

if(OSCAIRN_ORGAN_TESTS)
    option(ORGAN_ALLOC_GUARD "Count allocations in process() (off for sanitizer builds)" ON)
    enable_testing()
    add_executable(oscairn-organ-tests tests/test.cpp tests/leslie_test.cpp tests/harness.cpp tests/harness.h entry.cpp)
    target_include_directories(oscairn-organ-tests PRIVATE tests .)
    target_compile_definitions(oscairn-organ-tests PRIVATE ORGAN_ALLOC_GUARD=$<BOOL:${ORGAN_ALLOC_GUARD}>)
    target_link_libraries(oscairn-organ-tests PRIVATE oscairn_organ_plugin)
    # Renders the listening examples (doc/oscairn-plan.md, section 5).
    add_executable(organ-listen tools/listen.cpp tests/harness.cpp tests/harness.h)
    target_include_directories(organ-listen PRIVATE tests .)
    target_compile_definitions(organ-listen PRIVATE ORGAN_ALLOC_GUARD=0)
    target_link_libraries(organ-listen PRIVATE oscairn_organ_plugin)
    foreach(test metadata determinism realtime state notes choke contexts sleep performance rebuild threads quiet rates click voices transpose startup memory presets overdrive)
        add_test(NAME organ-${test} COMMAND oscairn-organ-tests ${test})
    endforeach()
    foreach(test metadata realtime state rates speed rebuild tail contexts memory determinism threads)
        add_test(NAME leslie-${test} COMMAND oscairn-organ-tests leslie-${test})
    endforeach()
endif()
