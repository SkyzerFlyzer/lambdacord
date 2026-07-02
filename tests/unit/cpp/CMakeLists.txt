cmake_minimum_required(VERSION 3.20)
project(discord_unit_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# doctest is the framework-wide unit test harness (AD-1). Fetched the same way as
# nlohmann/json so no third-party runtime dependency is added to the builder image.
FetchContent_Declare(
  doctest
  URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.12.tar.gz
)
FetchContent_MakeAvailable(doctest)

FetchContent_Declare(
  json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
)
FetchContent_MakeAvailable(json)

# GLOB over test_*.cpp so later tasks add test files without editing this
# CMakeLists (plan section 8.2). CONFIGURE_DEPENDS re-globs on rebuild.
file(GLOB UNIT_TEST_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

# Single translation unit that owns doctest's main(), generated so individual
# test_*.cpp files never define main and can be added freely by later tasks.
set(DOCTEST_MAIN "${CMAKE_CURRENT_BINARY_DIR}/doctest_main.cpp")
file(WRITE "${DOCTEST_MAIN}"
     "#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN\n#include <doctest/doctest.h>\n")

add_executable(discord_unit_tests ${DOCTEST_MAIN} ${UNIT_TEST_SOURCES})

# Headers under src/include are the framework's public API; unit tests consume
# them exactly as the Lambdas do. No AWS SDK, libsodium, or curl linkage here.
target_include_directories(discord_unit_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/include")

target_link_libraries(discord_unit_tests PRIVATE
    doctest::doctest
    nlohmann_json::nlohmann_json)

target_compile_options(discord_unit_tests PRIVATE -Wall -Wextra)
