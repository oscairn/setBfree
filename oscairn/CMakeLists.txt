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
