#!/usr/bin/env bash
set -euo pipefail

# Internal build+run implementation for the C++ unit test layer. Runs INSIDE the
# builder container (mirroring scripts/build-lambda-in-docker.sh). Do not call
# this directly on the host; use scripts/test-unit.sh.
#
# Usage: test-unit-in-docker.sh [doctest-filter]

usage() {
  cat <<'EOF'
Usage: test-unit-in-docker.sh [doctest-filter]

Configures, builds, and runs the doctest unit test suite against src/include.
Intended to run inside the builder container with the repo mounted at /workspace.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

FILTER="${1:-}"

REPO_ROOT="/workspace"
TEST_DIR="${REPO_ROOT}/tests/unit/cpp"
# Build dir lives inside the repo (mounted, so warm runs are fast) but is
# gitignored via the **/.lambda-build/ rule.
BUILD_DIR="${REPO_ROOT}/.lambda-build/unit-tests"

if [[ ! -d "${TEST_DIR}" ]]; then
  echo "Unit test directory does not exist: ${TEST_DIR}" >&2
  exit 1
fi

mkdir -p "${BUILD_DIR}"

# Match the compiler selection convention of build-lambda-in-docker.sh.
CXX_COMPILER="${CXX:-g++}"

cmake -S "${TEST_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER="${CXX_COMPILER}"

cmake --build "${BUILD_DIR}" --parallel

TEST_BIN="${BUILD_DIR}/discord_unit_tests"
if [[ ! -x "${TEST_BIN}" ]]; then
  echo "Expected unit test binary was not produced: ${TEST_BIN}" >&2
  exit 1
fi

if [[ -n "${FILTER}" ]]; then
  exec "${TEST_BIN}" "--test-case=${FILTER}"
fi

exec "${TEST_BIN}"
