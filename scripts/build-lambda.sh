#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: scripts/build-lambda.sh <lambda-folder>

Builds a C++ AWS Lambda in Docker using an Amazon Linux 2023 environment,
strips the resulting bootstrap binary, and writes a deployable zip to:
  packaged-lambdas/<lambda-name>.zip

Environment variables:
  LAMBDA_ARCH=arm64|x86_64          Lambda architecture. Default: arm64
  LAMBDA_BUILDER_IMAGE=<tag>        Docker image tag. Default derived from arch
  LAMBDA_SKIP_IMAGE_BUILD=1         Reuse an existing builder image
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

if [[ $# -ne 1 ]]; then
  usage >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
LAMBDA_DIR_INPUT="$1"
PACKAGED_LAMBDAS_DIR="${REPO_ROOT}/packaged-lambdas"

if [[ ! -d "${LAMBDA_DIR_INPUT}" ]]; then
  echo "Lambda folder does not exist: ${LAMBDA_DIR_INPUT}" >&2
  exit 1
fi

LAMBDA_DIR="$(cd "${LAMBDA_DIR_INPUT}" && pwd)"
LAMBDA_ARCH="${LAMBDA_ARCH:-arm64}"

if [[ "${LAMBDA_DIR}" != "${REPO_ROOT}"/* ]]; then
  echo "Lambda folder must be inside the repository: ${LAMBDA_DIR}" >&2
  exit 1
fi

LAMBDA_DIR_RELATIVE="${LAMBDA_DIR#${REPO_ROOT}/}"
LAMBDA_PACKAGE_NAME="$(basename "${LAMBDA_DIR}")"

case "${LAMBDA_DIR_RELATIVE}" in
  modules/nitrado/lambdas/oauth-*)
    LAMBDA_PACKAGE_NAME="nitrado-${LAMBDA_PACKAGE_NAME}"
    ;;
esac

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

mkdir -p "${PACKAGED_LAMBDAS_DIR}"

docker run --rm \
  --platform "${DOCKER_PLATFORM}" \
  -e LAMBDA_ARCH="${LAMBDA_ARCH}" \
  -e LAMBDA_LOCAL_TESTING="${LAMBDA_LOCAL_TESTING:-0}" \
  -e LAMBDA_PACKAGE_NAME="${LAMBDA_PACKAGE_NAME}" \
  -e PACKAGED_LAMBDAS_DIR="/workspace/packaged-lambdas" \
  -v "${REPO_ROOT}:/workspace" \
  -v "${PACKAGED_LAMBDAS_DIR}:/workspace/packaged-lambdas" \
  "${IMAGE_TAG}" \
  /opt/lambda-builder/build-lambda-in-docker.sh "/workspace/${LAMBDA_DIR_RELATIVE}"
