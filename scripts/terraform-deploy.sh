#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  scripts/terraform-deploy.sh [plan|apply|validate|init|output] [--skip-build] [--auto-approve] [-- <terraform args>]

Examples:
  scripts/terraform-deploy.sh plan
  scripts/terraform-deploy.sh apply
  scripts/terraform-deploy.sh apply --auto-approve
  scripts/terraform-deploy.sh plan --skip-build -- -var-file=my.tfvars

The script:
  1. regenerates root Terraform module wiring from modules/*/module.manifest.json
  2. optionally builds all Lambda zip artifacts
  3. runs Terraform from infra/terraform
USAGE
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TERRAFORM_DIR="${REPO_ROOT}/infra/terraform"

ACTION="plan"
SKIP_BUILD=0
AUTO_APPROVE=0
TERRAFORM_ARGS=()

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

if [[ $# -gt 0 && "${1:-}" != --* ]]; then
  ACTION="$1"
  shift
fi

while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-build)
      SKIP_BUILD=1
      shift
      ;;
    --auto-approve)
      AUTO_APPROVE=1
      shift
      ;;
    --)
      shift
      TERRAFORM_ARGS+=("$@")
      break
      ;;
    *)
      TERRAFORM_ARGS+=("$1")
      shift
      ;;
  esac
done

case "${ACTION}" in
  init|validate|plan|apply|output)
    ;;
  *)
    echo "Unsupported action: ${ACTION}" >&2
    usage >&2
    exit 1
    ;;
esac

run_terraform() {
  if ((${#TERRAFORM_ARGS[@]})); then
    terraform -chdir="${TERRAFORM_DIR}" "$@" "${TERRAFORM_ARGS[@]}"
  else
    terraform -chdir="${TERRAFORM_DIR}" "$@"
  fi
}

python3 "${REPO_ROOT}/scripts/generate-terraform-modules.py"
terraform -chdir="${TERRAFORM_DIR}" fmt -recursive

if [[ "${ACTION}" == "output" ]]; then
  run_terraform output
  exit 0
fi

if [[ "${ACTION}" != "init" && "${SKIP_BUILD}" != "1" ]]; then
  "${REPO_ROOT}/scripts/build-all-lambdas.sh"
fi

terraform -chdir="${TERRAFORM_DIR}" init

case "${ACTION}" in
  init)
    ;;
  validate)
    run_terraform validate
    ;;
  plan)
    terraform -chdir="${TERRAFORM_DIR}" validate
    run_terraform plan
    ;;
  apply)
    terraform -chdir="${TERRAFORM_DIR}" validate
    if [[ "${AUTO_APPROVE}" == "1" ]]; then
      run_terraform apply -auto-approve
    else
      run_terraform apply
    fi
    ;;
esac
