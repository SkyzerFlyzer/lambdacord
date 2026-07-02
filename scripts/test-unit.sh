#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: scripts/test-unit.sh [--filter <doctest-filter>]

Compiles and runs the C++ unit test suite (tests/unit/cpp/*.cpp) against
src/include inside the Amazon Linux 2023 builder image, on the host
architecture. No zip packaging, no RIE. Exit code propagates from doctest.

Options:
  --filter <doctest-filter>   Only run test cases matching the doctest filter
                              (passed through as --test-case=<filter>).

Environment variables:
  LAMBDA_ARCH=arm64|x86_64    Build/run architecture. Default: arm64
  LAMBDA_BUILDER_IMAGE=<tag>  Docker image tag. Default derived from arch
  LAMBDA_SKIP_IMAGE_BUILD=1   Reuse an existing builder image
EOF
}

FILTER=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --filter)
      FILTER="${2:-}"
      if [[ -z "${FILTER}" ]]; then
        echo "--filter requires a value" >&2
        exit 1
      fi
      shift 2
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

LAMBDA_ARCH="${LAMBDA_ARCH:-arm64}"

case "${LAMBDA_ARCH}" in
  arm64)
    DOCKER_PLATFORM="linux/arm64"
    ;;
  x86_64)
    DOCKER_PLATFORM="linux/amd64"
    ;;
  *)
    echo "Unsupported LAMBDA_ARCH: ${LAMBDA_ARCH}. Use arm64 or x86_64." >&2
    exit 1
    ;;
esac

IMAGE_TAG="${LAMBDA_BUILDER_IMAGE:-lambdacord-lambda-builder:${LAMBDA_ARCH}}"

if ! command -v docker >/dev/null 2>&1; then
  echo "Docker is required but was not found in PATH." >&2
  exit 1
fi

if [[ "${LAMBDA_SKIP_IMAGE_BUILD:-0}" != "1" ]]; then
  docker buildx build \
    --load \
    --platform "${DOCKER_PLATFORM}" \
    --tag "${IMAGE_TAG}" \
    --file "${REPO_ROOT}/docker/lambda-builder/Dockerfile" \
    "${REPO_ROOT}"
fi

docker run --rm \
  --platform "${DOCKER_PLATFORM}" \
  -v "${REPO_ROOT}:/workspace" \
  "${IMAGE_TAG}" \
  /workspace/scripts/test-unit-in-docker.sh "${FILTER}"
