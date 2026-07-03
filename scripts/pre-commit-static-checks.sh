#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
LAMBDA_ARCH="${LAMBDA_ARCH:-arm64}"
LAMBDA_BUILDER_IMAGE="${LAMBDA_BUILDER_IMAGE:-lambdacord-lambda-builder:${LAMBDA_ARCH}}"
DIFF_RANGE="${STATIC_CHECKS_DIFF_RANGE:-}"

usage() {
  cat <<'EOF'
Usage:
  scripts/pre-commit-static-checks.sh [--diff-range <ref>...<ref>] [<lambda-dir>...]

Without arguments, the script discovers staged files (git diff --cached) and
runs checks for any affected Lambda folder that contains a main.cpp entry point.

With --diff-range (or the STATIC_CHECKS_DIFF_RANGE env var), the affected files
are discovered from `git diff --name-only <range>` instead of the staged index.
This is how CI analyzes a whole PR on a fresh checkout with an empty index.
Everything else about tool selection and behavior is identical.
EOF
}

POSITIONAL_ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --diff-range)
      if [[ $# -lt 2 ]]; then
        echo "--diff-range requires a <ref>...<ref> argument." >&2
        exit 1
      fi
      DIFF_RANGE="$2"
      shift 2
      ;;
    --diff-range=*)
      DIFF_RANGE="${1#*=}"
      shift
      ;;
    *)
      POSITIONAL_ARGS+=("$1")
      shift
      ;;
  esac
done

if [[ ${#POSITIONAL_ARGS[@]} -gt 0 ]]; then
  set -- "${POSITIONAL_ARGS[@]}"
else
  set --
fi

if ! command -v docker >/dev/null 2>&1; then
  echo "Docker is required but was not found in PATH." >&2
  exit 1
fi

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

ALL_TARGETS=()
while IFS= read -r line; do
  ALL_TARGETS+=("${line}")
done < <(
  find "${REPO_ROOT}/src/lambdas" "${REPO_ROOT}/modules" -type f -name 'main.cpp' -print 2>/dev/null \
    | sed "s#/main.cpp\$##" \
    | sort
)

if [[ ${#ALL_TARGETS[@]} -eq 0 ]]; then
  echo "No Lambda targets with main.cpp were found." >&2
  exit 1
fi

SELECTED_TARGETS=()

has_target() {
  local candidate="$1"
  local existing

  for existing in "${SELECTED_TARGETS[@]-}"; do
    if [[ "${existing}" == "${candidate}" ]]; then
      return 0
    fi
  done

  return 1
}

package_name_for_target() {
  local target="$1"
  local rel_target="${target#${REPO_ROOT}/}"
  local package_name

  package_name="$(basename "${target}")"
  case "${rel_target}" in
    modules/nitrado/lambdas/oauth-*)
      package_name="nitrado-${package_name}"
      ;;
  esac

  printf '%s\n' "${package_name}"
}

add_target() {
  local candidate="$1"
  local absolute_candidate

  if [[ -d "${candidate}" ]]; then
    absolute_candidate="$(cd "${candidate}" && pwd)"
    if ! has_target "${absolute_candidate}"; then
      SELECTED_TARGETS+=("${absolute_candidate}")
    fi
  fi
}

if [[ $# -gt 0 ]]; then
  for arg in "$@"; do
    if [[ ! -d "${arg}" ]]; then
      echo "Target directory does not exist: ${arg}" >&2
      exit 1
    fi
    add_target "${arg}"
  done
else
  STAGED_FILES=()
  if [[ -n "${DIFF_RANGE}" ]]; then
    while IFS= read -r line; do
      STAGED_FILES+=("${line}")
    done < <(git -C "${REPO_ROOT}" diff --name-only --diff-filter=ACMR "${DIFF_RANGE}")
  else
    while IFS= read -r line; do
      STAGED_FILES+=("${line}")
    done < <(git -C "${REPO_ROOT}" diff --cached --name-only --diff-filter=ACMR)
  fi

  for staged_file in "${STAGED_FILES[@]}"; do
    [[ "${staged_file}" == *.cpp || "${staged_file}" == *.cc || "${staged_file}" == *.cxx || "${staged_file}" == *.hpp || "${staged_file}" == *.hh || "${staged_file}" == *.h ]] || continue

    # Shared framework headers are compiled into every Lambda, and the unit
    # suite deliberately excludes the curl/AWS-including ones — a change under
    # src/include therefore affects every Lambda target, not none.
    if [[ "${staged_file}" == src/include/* ]]; then
      for target in "${ALL_TARGETS[@]}"; do
        add_target "${target}"
      done
      continue
    fi

    staged_dir="${REPO_ROOT}/$(dirname "${staged_file}")"

    for target in "${ALL_TARGETS[@]}"; do
      if [[ "${staged_dir}" == "${target}" || "${staged_dir}" == "${target}"/* || "${target}" == "${staged_dir}"/* ]]; then
        add_target "${target}"
      fi
    done
  done
fi

if [[ ${#SELECTED_TARGETS[@]} -eq 0 ]]; then
  echo "No staged C++ Lambda changes detected; skipping pre-commit checks."
  exit 0
fi

if [[ "${LAMBDA_SKIP_IMAGE_BUILD:-0}" != "1" ]]; then
  docker buildx build \
    --load \
    --platform "${DOCKER_PLATFORM}" \
    --tag "${LAMBDA_BUILDER_IMAGE}" \
    --file "${REPO_ROOT}/docker/lambda-builder/Dockerfile" \
    "${REPO_ROOT}"
fi

SORTED_TARGETS=()
while IFS= read -r line; do
  SORTED_TARGETS+=("${line}")
done < <(printf '%s\n' "${SELECTED_TARGETS[@]-}" | sort)

needs_discord_local_tests() {
  local target

  for target in "${SORTED_TARGETS[@]}"; do
    case "${target#${REPO_ROOT}/}" in
      src/lambdas/*)
        return 0
        ;;
      modules/*/lambdas/*)
        return 0
        ;;
    esac
  done

  return 1
}

for target in "${SORTED_TARGETS[@]}"; do
  rel_target="${target#${REPO_ROOT}/}"
  echo "==> Running pre-commit checks for ${rel_target}"
  docker run --rm \
    --platform "${DOCKER_PLATFORM}" \
    -e LAMBDA_ARCH="${LAMBDA_ARCH}" \
    -e LAMBDA_PACKAGE_NAME="$(package_name_for_target "${target}")" \
    -v "${REPO_ROOT}:/workspace" \
    "${LAMBDA_BUILDER_IMAGE}" \
    /opt/lambda-builder/build-lambda-in-docker.sh --mode checks "/workspace/${rel_target}"
done

if needs_discord_local_tests; then
  DISCORD_TEST_TARGETS=(
    "${REPO_ROOT}/src/lambdas/discord-interactions"
    "${REPO_ROOT}/src/lambdas/discord-application-command-handler"
    "${REPO_ROOT}/src/lambdas/discord-message-component-handler"
    "${REPO_ROOT}/src/lambdas/discord-modal-handler"
    "${REPO_ROOT}/src/lambdas/discord-autocomplete-handler"
  )

  echo "==> Building packaged Discord Lambdas for local valgrind tests"
  for target in "${DISCORD_TEST_TARGETS[@]}"; do
    LAMBDA_ARCH="${LAMBDA_ARCH}" \
    LAMBDA_BUILDER_IMAGE="${LAMBDA_BUILDER_IMAGE}" \
    LAMBDA_SKIP_IMAGE_BUILD=1 \
    "${REPO_ROOT}/scripts/build-lambda.sh" "${target}"
  done

  echo "==> Running existing local Discord tests under valgrind"
  DISCORD_TEST_PLATFORM="${DOCKER_PLATFORM}" \
  DISCORD_TEST_RUNTIME_IMAGE="${LAMBDA_BUILDER_IMAGE}" \
  DISCORD_TEST_CONTAINER_CMD="/usr/local/bin/aws-lambda-rie valgrind --quiet --leak-check=full --show-leak-kinds=all --error-exitcode=1 /var/runtime/bootstrap" \
  python3 "${REPO_ROOT}/tests/local/discord/run_local_tests.py"
fi
