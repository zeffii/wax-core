cmake_minimum_required(VERSION 3.21)
project(wax-core VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /utf-8 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# The only outside code so far: a JSON reader for .wax files (header only, fetched once at configure time).
include(FetchContent)
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)
FetchContent_MakeAvailable(json)

# The engine: everything that makes sound, with no window and no sound card.
add_library(waxcore
  third_party/v8-ieee754/ieee754.cc
  src/core/params.cpp
  src/core/node_registry.cpp
  src/core/random.cpp
  src/project/project.cpp
  src/project/master.cpp
  src/graph/compile.cpp
  src/seq/cells.cpp
  src/seq/chordname.cpp
  src/seq/columns.cpp
  src/seq/lane.cpp
  src/engine/runtime.cpp
  src/engine/sleep.cpp
  src/engine/limiter.cpp
  src/engine/sequencer.cpp
  src/engine/offline.cpp
  src/io/wav.cpp
  src/nodeseq/nodeseq.cpp
  src/nodeseq/seq_parse.cpp
  src/nodeseq/types.cpp
  src/nodeseq/types_time.cpp
  src/nodeseq/types_values.cpp
  src/nodeseq/types_chords.cpp
  src/nodeseq/types_output.cpp
  src/nodeseq/types_logic.cpp
  src/nodes/all_nodes.cpp
  src/nodes/waves.cpp
  src/nodes/plumbing.cpp
  src/nodes/out.cpp
  src/nodes/busin.cpp
  src/nodes/tempo.cpp
  src/nodes/voices.cpp
  src/nodes/canyon.cpp
  src/nodes/chord.cpp
  src/nodes/chorus.cpp
  src/nodes/clap.cpp
  src/nodes/combperc.cpp
  src/nodes/control_nodes.cpp
  src/nodes/delay.cpp
  src/nodes/duette.cpp
  src/nodes/env.cpp
  src/nodes/fft.cpp
  src/nodes/gain.cpp
  src/nodes/hall.cpp
  src/nodes/hat808.cpp
  src/nodes/lfo.cpp
  src/nodes/filter.cpp
  src/nodes/noise.cpp
  src/nodes/crusher.cpp
  src/nodes/gater.cpp
  src/nodes/multitap.cpp
  src/nodes/freqshift.cpp
  src/nodes/phaser.cpp
  src/nodes/shimmer.cpp
  src/nodes/fmsynth.cpp
  src/nodes/piano.cpp
  src/nodes/wave_table_data.cpp
  src/nodes/tape.cpp
  src/nodes/string.cpp
  src/nodes/wavetable.cpp
  src/nodes/addsynth.cpp
  src/nodes/arp.cpp
  src/nodes/specpan.cpp
  src/nodes/blur.cpp
  src/nodes/shiva.cpp
  src/nodes/woodblock.cpp
  src/nodes/mosh.cpp
  src/nodes/osc.cpp
  src/nodes/plate.cpp
  src/nodes/pluckgate.cpp
  src/nodes/sidechain.cpp
  src/nodes/squelch.cpp
  src/nodes/stft.cpp
  src/nodes/subsynth.cpp
  src/nodes/vocoder.cpp
  src/nodes/wavefolder.cpp
  src/nodes/westcoast.cpp)
target_include_directories(waxcore PUBLIC src third_party)
# Results must be the same on every machine: no fused multiply-add, no fast-math.
if(NOT MSVC)
  target_compile_options(waxcore PUBLIC -ffp-contract=off)
endif()
target_link_libraries(waxcore PUBLIC nlohmann_json::nlohmann_json)

# Command-line tools.
add_executable(wax-info tools/wax_info.cpp)
target_link_libraries(wax-info PRIVATE waxcore)

add_executable(wax-render tools/wax_render.cpp)
target_link_libraries(wax-render PRIVATE waxcore)

add_executable(wax-compare tools/wax_compare.cpp)
target_link_libraries(wax-compare PRIVATE waxcore)

# Tests: one plain program, run by `ctest`.
enable_testing()
add_executable(wax-tests tests/tests.cpp)
target_link_libraries(wax-tests PRIVATE waxcore)
target_compile_definitions(wax-tests PRIVATE WAX_TEST_SONGS="${CMAKE_SOURCE_DIR}/tests/songs")
add_test(NAME wax-tests COMMAND wax-tests)
