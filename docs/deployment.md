# Deployment

Root Terraform lives in `infra/terraform`. It deploys the generic framework
infrastructure and calls each installed module's Terraform explicitly.

## Generated wiring

Terraform cannot dynamically instantiate arbitrary module sources by scanning
the filesystem, so this repo generates the root wiring from installed module
manifests into `infra/terraform/generated_*.tf`:

```bash
python3 scripts/generate-terraform-modules.py
```

This emits root module calls, pass-through variables, manifest locals
(including the merged `ephemeral_defer` allowlist), and module output
proxies. Run it after adding or removing module Terraform, or after changing
any manifest `ephemeral_defer` flag.

**Never edit `infra/terraform/generated_*.tf` by hand** — update module
manifests/module Terraform and rerun the generator.

## Deploying

Build all zip artifacts first:

```bash
scripts/build-all-lambdas.sh
```

Then configure and deploy from the root stack:

```bash
cd infra/terraform
cp terraform.tfvars.example terraform.tfvars
# fill in the root variables
terraform init
terraform validate
terraform plan
terraform apply
```

Or use the wrapper script from the repo root:

```bash
scripts/terraform-deploy.sh plan
scripts/terraform-deploy.sh apply
```

The wrapper regenerates the `generated_*.tf` files from installed module
manifests before running Terraform, and builds Lambda zips unless you pass
`--skip-build`. Supported actions: `init`, `validate`, `plan`, `apply`,
`output`.

## After the first apply

The root stack outputs the Discord interactions Function URL. Use that URL as
the **Interactions Endpoint URL** in the Discord Developer Portal.

## Module Terraform

For module-specific Terraform variables, resources, and outputs, read that
module's `README.md`. Modules may provide their own
`terraform.tfvars.example` with values to copy into the root stack's
`terraform.tfvars` or pass as an additional Terraform var-file.

## Optional: durable interaction dedup table

Cross-container interaction dedup (see
[Errors & Idempotency](error-handling.md#durable-cross-container-dedup)) is
opt-in: provision the DynamoDB table via the root
`discord_idempotency_table_enabled` Terraform variable and set
`DISCORD_IDEMPOTENCY_TABLE` on the worker Lambdas that should use it.
