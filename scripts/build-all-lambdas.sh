#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
LAMBDA_ARCH="${LAMBDA_ARCH:-arm64}"
LAMBDA_BUILDER_IMAGE="${LAMBDA_BUILDER_IMAGE:-lambdacord-lambda-builder:${LAMBDA_ARCH}}"

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

if ! command -v docker >/dev/null 2>&1; then
  echo "Docker is required but was not found in PATH." >&2
  exit 1
fi

LAMBDA_DIRS=()
while IFS= read -r lambda_dir; do
  [[ -n "${lambda_dir}" ]] && LAMBDA_DIRS+=("${lambda_dir}")
done < <(
  python3 -c '
from pathlib import Path
import sys

repo_root = Path(sys.argv[1])
sys.path.insert(0, str(repo_root / "scripts" / "lib"))
from discord_modules import lambda_dirs

for path in lambda_dirs(repo_root):
    print(path.relative_to(repo_root))
' "${REPO_ROOT}"
)

if [[ "${LAMBDA_SKIP_IMAGE_BUILD:-0}" != "1" ]]; then
  docker buildx build \
    --load \
    --platform "${DOCKER_PLATFORM}" \
    --tag "${LAMBDA_BUILDER_IMAGE}" \
    --file "${REPO_ROOT}/docker/lambda-builder/Dockerfile" \
    "${REPO_ROOT}"
fi

for lambda_dir in "${LAMBDA_DIRS[@]}"; do
  LAMBDA_ARCH="${LAMBDA_ARCH}" \
  LAMBDA_BUILDER_IMAGE="${LAMBDA_BUILDER_IMAGE}" \
  LAMBDA_SKIP_IMAGE_BUILD=1 \
  "${REPO_ROOT}/scripts/build-lambda.sh" "${REPO_ROOT}/${lambda_dir}"
done
