"""Terraform module-wiring generation in ``scripts/generate-terraform-modules.py``.

The root Terraform never names installed modules by hand: module blocks,
variable pass-through, manifest locals, and output proxies are all derived from
``modules/*/module.manifest.json`` plus the module's own ``terraform/``
directory. These tests pin that derivation.

Existing coverage of the generator (``test_ephemeral_defer.py``) is limited to
``DISCORD_EPHEMERAL_DEFER_ROUTES`` emission; the wiring itself was untested.

Fake module trees are built in pytest tmp dirs — the real ``modules/``
directory is never read.
"""

import importlib.util
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
GENERATOR = REPO_ROOT / "scripts" / "generate-terraform-modules.py"

MODULE_OUTPUTS_TF = """output "lambda_names" {
  value = []
}
"""


def load_generator():
    spec = importlib.util.spec_from_file_location("generate_terraform_modules", GENERATOR)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def manifest(name, **overrides):
    base = {
        "name": name,
        "commands": "discord.commands.json",
        "errors": "errors.json",
        "terraform": "terraform",
        "lambdas": [],
        "routes": {},
    }
    base.update(overrides)
    return base


def terraform_files(*, variables="", outputs=MODULE_OUTPUTS_TF):
    files = [("terraform/main.tf", "# fixture\n")]
    if variables:
        files.append(("terraform/variables.tf", variables))
    if outputs:
        files.append(("terraform/outputs.tf", outputs))
    return files


def install(make_module, dir_name, module_manifest, extra_files=()):
    """Write a module whose declared schema files exist, as validation requires."""
    return make_module(
        dir_name,
        module_manifest,
        commands=[],
        errors={"errors": {}},
        extra_files=extra_files,
    )


def generated(repo_root, filename):
    files = load_generator().generate(repo_root)
    return files[repo_root / "infra" / "terraform" / filename]


# ---------------------------------------------------------------------------
# terraform_id: manifest name -> Terraform identifier
# ---------------------------------------------------------------------------


def test_dashes_become_underscores():
    assert load_generator().terraform_id("ark-survival-ascended") == "ark_survival_ascended"


def test_leading_digit_is_prefixed():
    """A name may not start with a digit, so the generator prefixes it."""
    assert load_generator().terraform_id("7-days-to-die") == "module_7_days_to_die"


def test_underscores_and_alphanumerics_are_preserved():
    assert load_generator().terraform_id("nitrado_core9") == "nitrado_core9"


# ---------------------------------------------------------------------------
# Module blocks
# ---------------------------------------------------------------------------


def test_module_block_sources_the_module_terraform_dir(repo_root, make_module):
    install(make_module, "dayz", manifest("dayz"), extra_files=terraform_files())
    modules_tf = generated(repo_root, "generated_modules.tf")

    assert 'module "dayz" {' in modules_tf
    assert 'source = "../../modules/dayz/terraform"' in modules_tf


def test_module_without_terraform_key_gets_no_block(repo_root, make_module):
    install(make_module, "dayz", manifest("dayz", terraform=None))
    modules_tf = generated(repo_root, "generated_modules.tf")

    assert 'module "' not in modules_tf
    assert "modules/dayz/module.manifest.json" not in modules_tf


def test_manifest_locals_list_every_wired_module(repo_root, make_module):
    install(make_module, "dayz", manifest("dayz"), extra_files=terraform_files())
    install(make_module, "rust", manifest("rust"), extra_files=terraform_files())
    modules_tf = generated(repo_root, "generated_modules.tf")

    assert "modules/dayz/module.manifest.json" in modules_tf
    assert "modules/rust/module.manifest.json" in modules_tf


def test_no_modules_still_emits_an_empty_manifest_local(repo_root):
    modules_tf = generated(repo_root, "generated_modules.tf")

    assert "module_manifests = [" in modules_tf
    assert 'module "' not in modules_tf


# ---------------------------------------------------------------------------
# Variable pass-through
# ---------------------------------------------------------------------------


def test_common_inputs_are_wired_to_root_expressions(repo_root, make_module):
    install(make_module, 
        "dayz",
        manifest("dayz"),
        extra_files=terraform_files(
            variables='variable "project_name" {\n  type = string\n}\n'
            'variable "tags" {\n  type = map(string)\n}\n'
        ),
    )
    modules_tf = generated(repo_root, "generated_modules.tf")

    assert "project_name" in modules_tf
    assert "var.project_name" in modules_tf
    assert "local.common_tags" in modules_tf


def test_zip_root_and_tags_resolve_to_root_locals_not_variables(repo_root, make_module):
    """Two common inputs come from root locals; the rest come from root vars."""
    install(
        make_module,
        "dayz",
        manifest("dayz"),
        extra_files=terraform_files(
            variables='variable "zip_root" {\n  type = string\n}\n'
            'variable "tags" {\n  type = map(string)\n}\n'
        ),
    )
    modules_tf = generated(repo_root, "generated_modules.tf")

    assert "local.zip_root" in modules_tf
    assert "local.common_tags" in modules_tf
    assert "var.zip_root" not in modules_tf


def test_dedup_table_input_is_not_a_common_input(repo_root, make_module):
    """The idempotency table is count-gated, so it is not auto-wired.

    It left COMMON_INPUTS when the table became opt-in; a module declaring it
    gets an ordinary promoted root variable rather than a direct reference to
    the (possibly absent) table resource.
    """
    install(
        make_module,
        "dayz",
        manifest("dayz"),
        extra_files=terraform_files(
            variables='variable "discord_idempotency_table_name" {\n  type = string\n}\n'
        ),
    )
    modules_tf = generated(repo_root, "generated_modules.tf")

    assert "aws_dynamodb_table" not in modules_tf
    assert "var.discord_idempotency_table_name" in modules_tf


def test_module_specific_variable_is_promoted_to_a_root_variable(repo_root, make_module):
    """A module-owned input the root does not know becomes a generated root variable."""
    install(make_module, 
        "dayz",
        manifest("dayz"),
        extra_files=terraform_files(
            variables='variable "dayz_adm_poll_minutes" {\n'
            "  description = \"Poll cadence.\"\n"
            "  type        = number\n"
            "}\n"
        ),
    )
    variables_tf = generated(repo_root, "generated_module_variables.tf")
    modules_tf = generated(repo_root, "generated_modules.tf")

    assert 'variable "dayz_adm_poll_minutes"' in variables_tf
    assert "var.dayz_adm_poll_minutes" in modules_tf


def test_generated_files_carry_the_do_not_edit_header(repo_root, make_module):
    install(make_module, "dayz", manifest("dayz"), extra_files=terraform_files())

    for filename in (
        "generated_modules.tf",
        "generated_module_outputs.tf",
        "generated_module_variables.tf",
    ):
        assert "Do not edit by hand" in generated(repo_root, filename)


# ---------------------------------------------------------------------------
# Output proxies
# ---------------------------------------------------------------------------


def test_lambda_names_outputs_are_concatenated(repo_root, make_module):
    install(make_module, "dayz", manifest("dayz"), extra_files=terraform_files())
    install(make_module, "rust", manifest("rust"), extra_files=terraform_files())
    outputs_tf = generated(repo_root, "generated_module_outputs.tf")

    assert "module.dayz.lambda_names" in outputs_tf
    assert "module.rust.lambda_names" in outputs_tf
    assert "sort(concat(" in outputs_tf


def test_worker_lambda_names_is_empty_without_modules(repo_root):
    outputs_tf = generated(repo_root, "generated_module_outputs.tf")

    assert 'output "worker_lambda_names"' in outputs_tf
    assert "value       = []" in outputs_tf


def test_module_without_lambda_names_output_is_not_concatenated(repo_root, make_module):
    install(make_module, "dayz", manifest("dayz"), extra_files=terraform_files(outputs=""))
    outputs_tf = generated(repo_root, "generated_module_outputs.tf")

    assert "module.dayz.lambda_names" not in outputs_tf
    assert "value       = []" in outputs_tf


def test_other_module_outputs_are_reexported_with_a_namespaced_name(repo_root, make_module):
    install(make_module, 
        "dayz",
        manifest("dayz"),
        extra_files=terraform_files(
            outputs=MODULE_OUTPUTS_TF + 'output "killfeed_table_name" {\n  value = ""\n}\n'
        ),
    )
    outputs_tf = generated(repo_root, "generated_module_outputs.tf")

    assert 'output "module_dayz_killfeed_table_name"' in outputs_tf
    assert "module.dayz.killfeed_table_name" in outputs_tf


def test_reexported_output_names_do_not_collide_across_modules(repo_root, make_module):
    """Two modules exporting the same output name stay distinct at the root."""
    extra = MODULE_OUTPUTS_TF + 'output "table_name" {\n  value = ""\n}\n'
    install(make_module, "dayz", manifest("dayz"), extra_files=terraform_files(outputs=extra))
    install(make_module, "rust", manifest("rust"), extra_files=terraform_files(outputs=extra))
    outputs_tf = generated(repo_root, "generated_module_outputs.tf")

    assert 'output "module_dayz_table_name"' in outputs_tf
    assert 'output "module_rust_table_name"' in outputs_tf


def test_module_id_is_sanitized_in_block_and_outputs(repo_root, make_module):
    """A dashed directory/name pair still produces valid Terraform identifiers."""
    install(make_module, 
        "farming-simulator-25",
        manifest("farming-simulator-25"),
        extra_files=terraform_files(),
    )
    modules_tf = generated(repo_root, "generated_modules.tf")
    outputs_tf = generated(repo_root, "generated_module_outputs.tf")

    assert 'module "farming_simulator_25" {' in modules_tf
    assert 'source = "../../modules/farming-simulator-25/terraform"' in modules_tf
    assert "module.farming_simulator_25.lambda_names" in outputs_tf
