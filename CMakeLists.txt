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
  src/core/params.cpp
  src/core/node_registry.cpp
  src/core/random.cpp
  src/nodes/all_nodes.cpp
  src/nodes/gain.cpp
  src/project/project.cpp
  src/io/wav.cpp)
target_include_directories(waxcore PUBLIC src)
target_link_libraries(waxcore PUBLIC nlohmann_json::nlohmann_json)

# Command-line tools.
add_executable(wax-info tools/wax_info.cpp)
target_link_libraries(wax-info PRIVATE waxcore)

add_executable(wax-compare tools/wax_compare.cpp)
target_link_libraries(wax-compare PRIVATE waxcore)

# Tests: one plain program, run by `ctest`.
enable_testing()
add_executable(wax-tests tests/tests.cpp)
target_link_libraries(wax-tests PRIVATE waxcore)
target_compile_definitions(wax-tests PRIVATE WAX_TEST_SONGS="${CMAKE_SOURCE_DIR}/tests/songs")
add_test(NAME wax-tests COMMAND wax-tests)
