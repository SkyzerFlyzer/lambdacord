cmake_minimum_required(VERSION 3.20)
project(discord-interactions LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-static -Wl,--gc-sections")
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
set(BUILD_SHARED_LIBS OFF)
set(CURL_USE_STATIC_LIBS ON)
set(OPENSSL_USE_STATIC_LIBS ON)

find_package(aws-lambda-runtime REQUIRED)
find_package(AWSSDK REQUIRED COMPONENTS lambda dynamodb kms)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
include(FetchContent)
FetchContent_Declare(
  json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

add_executable(discord-interactions
    "${CMAKE_CURRENT_LIST_DIR}/main.cpp"
)

target_include_directories(discord-interactions PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "/workspace/src/include"
)

target_compile_definitions(discord-interactions PRIVATE
    CURL_STATICLIB
)

if(DEFINED ENV{LAMBDA_LOCAL_TESTING} AND "$ENV{LAMBDA_LOCAL_TESTING}" STREQUAL "1")
    target_compile_definitions(discord-interactions PRIVATE
        DISCORD_ALLOW_SIGNATURE_BYPASS
    )
endif()

target_compile_options(discord-interactions PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -ffunction-sections
    -fdata-sections
    $<$<CONFIG:Debug>:-O0 -g -fno-omit-frame-pointer>
)

target_link_options(discord-interactions PRIVATE
    -static
    -Wl,--gc-sections
)

target_link_libraries(discord-interactions PRIVATE
    AWS::aws-lambda-runtime
    ${AWSSDK_LINK_LIBRARIES}
    nlohmann_json::nlohmann_json
    PkgConfig::CURL
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    PkgConfig::SODIUM
)
