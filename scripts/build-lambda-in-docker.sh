#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  build-lambda-in-docker.sh [--mode build|checks] <lambda-src>

Modes:
  build   Build, llvm-strip, and zip the Lambda bootstrap.
  checks  Run clang-tidy, clang-check, cppcheck, cppcheck bug hunting,
          and, when a local event file is present, valgrind + LLVM coverage.
EOF
}

MODE="build"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode)
      MODE="${2:-}"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      break
      ;;
  esac
done

SRC_DIR="${1:-/lambda-src}"

if [[ ! -d "${SRC_DIR}" ]]; then
  echo "Source directory does not exist: ${SRC_DIR}" >&2
  exit 1
fi

if [[ "${MODE}" != "build" && "${MODE}" != "checks" ]]; then
  echo "Unsupported mode: ${MODE}" >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MOCK_RUNTIME_API="${SCRIPT_DIR}/mock-lambda-runtime-api.py"
LAMBDA_NAME="$(basename "${SRC_DIR}")"
LAMBDA_PACKAGE_NAME="${LAMBDA_PACKAGE_NAME:-${LAMBDA_NAME}}"
BUILD_ROOT="${SRC_DIR}/.lambda-build"
DIST_DIR="${SRC_DIR}/dist"
PACKAGED_LAMBDAS_DIR="${PACKAGED_LAMBDAS_DIR:-${DIST_DIR}}"
ARCHIVE_PATH="${PACKAGED_LAMBDAS_DIR}/${LAMBDA_PACKAGE_NAME}.zip"
TEMP_BOOTSTRAP_PATH="${BUILD_ROOT}/bootstrap"
BUILD_DIR="${BUILD_ROOT}/build"
PROFILE_RAW="${BUILD_ROOT}/coverage.profraw"
PROFILE_DATA="${BUILD_ROOT}/coverage.profdata"

find_llvm_tool() {
  local tool_name="$1"
  local tool_path

  for tool_path in "${tool_name}-20" "${tool_name}"; do
    if command -v "${tool_path}" >/dev/null 2>&1; then
      command -v "${tool_path}"
      return 0
    fi
  done

  echo "Unable to locate ${tool_name} or ${tool_name}-20 in PATH." >&2
  exit 1
}

SOURCE_FILES=()
while IFS= read -r source_file; do
  [[ -n "${source_file}" ]] && SOURCE_FILES+=("${source_file}")
done < <(
  find "${SRC_DIR}" -type f \( -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' \) \
    ! -path "${BUILD_ROOT}/*" \
    ! -path "${DIST_DIR}/*" \
    | sort
)

if [[ ${#SOURCE_FILES[@]} -eq 0 ]]; then
  echo "No C++ source files found in ${SRC_DIR}" >&2
  exit 1
fi

find_event_file() {
  local candidate
  local candidates=(
    "${SRC_DIR}/test-event.json"
    "${SRC_DIR}/tests/test-event.json"
    "/workspace/tests/events/${LAMBDA_PACKAGE_NAME}.json"
    "/workspace/tests/events/${LAMBDA_NAME}.json"
  )

  for candidate in "${candidates[@]}"; do
    if [[ -f "${candidate}" ]]; then
      printf '%s\n' "${candidate}"
      return 0
    fi
  done

  return 1
}

cleanup() {
  rm -rf "${BUILD_ROOT}"
}

trap cleanup EXIT

generate_cmake() {
  local cmake_file="${BUILD_ROOT}/CMakeLists.txt"
  mkdir -p "${BUILD_ROOT}" "${PACKAGED_LAMBDAS_DIR}"

  {
    cat <<EOF
cmake_minimum_required(VERSION 3.20)
project(${LAMBDA_NAME} LANGUAGES CXX)

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

add_executable(${LAMBDA_NAME}
EOF

    local source_file escaped_source_file
    for source_file in "${SOURCE_FILES[@]}"; do
      escaped_source_file="${source_file//\"/\\\"}"
      printf "    \"%s\"\n" "${escaped_source_file}"
    done

    cat <<EOF
)

target_include_directories(${LAMBDA_NAME} PRIVATE
    "${SRC_DIR}"
    "${SRC_DIR}/include"
    "/workspace/src/include"
)

target_compile_definitions(${LAMBDA_NAME} PRIVATE
    CURL_STATICLIB
)

target_compile_options(${LAMBDA_NAME} PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -ffunction-sections
    -fdata-sections
    \$<$<CONFIG:Debug>:-O0 -g -fno-omit-frame-pointer>
)

target_link_options(${LAMBDA_NAME} PRIVATE
    -static
    -Wl,--gc-sections
)

target_link_libraries(${LAMBDA_NAME} PRIVATE
    AWS::aws-lambda-runtime
    \${AWSSDK_LINK_LIBRARIES}
    nlohmann_json::nlohmann_json
    PkgConfig::CURL
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    PkgConfig::SODIUM
)
EOF
  } > "${cmake_file}"
}

configure_build() {
  local build_type="$1"
  local extra_cxx_flags="${2:-}"
  local extra_exe_linker_flags="${3:-}"
  local cmake_source_dir="${BUILD_ROOT}"

  if [[ -f "${SRC_DIR}/CMakeLists.txt" ]]; then
    cmake_source_dir="${SRC_DIR}"
  fi

  rm -rf "${BUILD_DIR}"

  cmake -S "${cmake_source_dir}" -B "${BUILD_DIR}" \
    -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_BUILD_TYPE="${build_type}" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_PREFIX_PATH="/opt/lambda-static;/opt/lambda-install" \
    -DOPENSSL_ROOT_DIR=/opt/lambda-static \
    -DCURL_USE_STATIC_LIBS=ON \
    -DCMAKE_CXX_FLAGS="${extra_cxx_flags}" \
    -DCMAKE_EXE_LINKER_FLAGS="${extra_exe_linker_flags}"
}

build_target() {
  cmake --build "${BUILD_DIR}" --parallel
}

binary_path() {
  printf '%s\n' "${BUILD_DIR}/${LAMBDA_NAME}"
}

run_runtime_invocation() {
  local executable_path="$1"
  local event_file="$2"
  local response_file="$3"
  local runner=("${@:4}")
  local runtime_port=19080
  local server_pid

  python3 "${MOCK_RUNTIME_API}" \
    --port "${runtime_port}" \
    --event-file "${event_file}" \
    --response-file "${response_file}" &
  server_pid=$!
  sleep 0.2

  cleanup_server() {
    kill "${server_pid}" >/dev/null 2>&1 || true
    wait "${server_pid}" >/dev/null 2>&1 || true
  }

  trap cleanup_server RETURN

  AWS_LAMBDA_RUNTIME_API="127.0.0.1:${runtime_port}" \
  AWS_REGION="${AWS_REGION:-us-east-1}" \
  AWS_ACCESS_KEY_ID="${AWS_ACCESS_KEY_ID:-static-checks}" \
  AWS_SECRET_ACCESS_KEY="${AWS_SECRET_ACCESS_KEY:-static-checks}" \
  AWS_SESSION_TOKEN="${AWS_SESSION_TOKEN:-static-checks}" \
  AWS_EC2_METADATA_DISABLED=true \
    "${runner[@]}" "${executable_path}"

  if [[ ! -s "${response_file}" ]]; then
    echo "Runtime invocation did not produce a response file." >&2
    exit 1
  fi
}

run_build_mode() {
  local llvm_strip
  local bin_path

  generate_cmake
  configure_build "Release"
  build_target

  bin_path="$(binary_path)"
  if [[ ! -f "${bin_path}" ]]; then
    echo "Expected binary was not produced: ${bin_path}" >&2
    exit 1
  fi

  llvm_strip="$(find_llvm_tool llvm-strip)"
  "${llvm_strip}" --strip-all "${bin_path}"
  cp "${bin_path}" "${TEMP_BOOTSTRAP_PATH}"
  chmod 755 "${TEMP_BOOTSTRAP_PATH}"
  touch -t 198001010000 "${TEMP_BOOTSTRAP_PATH}"

  (
    cd "${BUILD_ROOT}"
    rm -f "${ARCHIVE_PATH}"
    zip -X -q -9 "${ARCHIVE_PATH}" bootstrap
  )

  echo "Created ${ARCHIVE_PATH}"
}

run_checks_mode() {
  local compile_commands="${BUILD_DIR}/compile_commands.json"
  local event_file=""
  local bin_path
  local response_file="${BUILD_ROOT}/runtime-response.json"
  local llvm_profdata
  local llvm_cov
  local clang_tidy_bin
  local clang_check_bin

  generate_cmake
  configure_build "Debug"
  build_target

  if [[ ! -f "${compile_commands}" ]]; then
    echo "compile_commands.json was not generated." >&2
    exit 1
  fi

  clang_tidy_bin="$(find_llvm_tool clang-tidy)"
  clang_check_bin="$(find_llvm_tool clang-check)"

  echo "Running clang-tidy for ${SRC_DIR}"
  "${clang_tidy_bin}" -p "${BUILD_DIR}" "${SOURCE_FILES[@]}"

  echo "Running clang-check for ${SRC_DIR}"
  "${clang_check_bin}" -p "${BUILD_DIR}" "${SOURCE_FILES[@]}"

  local -a cppcheck_include_args=("-I" "${SRC_DIR}")
  if [[ -d "${SRC_DIR}/include" ]]; then
    cppcheck_include_args+=("-I" "${SRC_DIR}/include")
  fi

  echo "Running cppcheck for ${SRC_DIR}"
  cppcheck \
    --std=c++17 \
    --language=c++ \
    --inline-suppr \
    --enable=warning,style,performance,portability,information \
    --inconclusive \
    --error-exitcode=1 \
    --suppress=missingIncludeSystem \
    "${cppcheck_include_args[@]}" \
    "${SOURCE_FILES[@]}"

  echo "Running cppcheck bug hunting for ${SRC_DIR}"
  cppcheck \
    --std=c++17 \
    --language=c++ \
    --inline-suppr \
    --bug-hunting \
    --error-exitcode=1 \
    --suppress=missingIncludeSystem \
    "${cppcheck_include_args[@]}" \
    "${SOURCE_FILES[@]}"

  if event_file="$(find_event_file)"; then
    bin_path="$(binary_path)"
    echo "Running valgrind for ${SRC_DIR} using ${event_file}"
    : > "${response_file}"
    run_runtime_invocation \
      "${bin_path}" \
      "${event_file}" \
      "${response_file}" \
      valgrind --quiet --leak-check=full --show-leak-kinds=all --error-exitcode=1

    echo "Running LLVM coverage for ${SRC_DIR} using ${event_file}"
    configure_build "Debug" "-fprofile-instr-generate -fcoverage-mapping" "-fprofile-instr-generate"
    build_target
    bin_path="$(binary_path)"
    : > "${response_file}"
    LLVM_PROFILE_FILE="${PROFILE_RAW}" \
      run_runtime_invocation \
        "${bin_path}" \
        "${event_file}" \
        "${response_file}"

    if [[ ! -f "${PROFILE_RAW}" ]]; then
      echo "Coverage profile was not produced: ${PROFILE_RAW}" >&2
      exit 1
    fi

    llvm_profdata="$(find_llvm_tool llvm-profdata)"
    llvm_cov="$(find_llvm_tool llvm-cov)"
    "${llvm_profdata}" merge -sparse "${PROFILE_RAW}" -o "${PROFILE_DATA}"
    "${llvm_cov}" report "${bin_path}" -instr-profile="${PROFILE_DATA}" "${SOURCE_FILES[@]}"
  else
    echo "Skipping valgrind and coverage for ${SRC_DIR}: no test event file found."
  fi
}

case "${MODE}" in
  build)
    run_build_mode
    ;;
  checks)
    run_checks_mode
    ;;
esac
