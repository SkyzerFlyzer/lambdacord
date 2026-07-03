"""Fixtures for the Python unit test layer.

Fake module trees are built inside pytest tmp dirs. The real repository's
``modules/`` directory (absent in framework-only checkouts) is never read or
written by these tests.
"""

import json
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPTS_LIB = REPO_ROOT / "scripts" / "lib"
if str(SCRIPTS_LIB) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_LIB))


@pytest.fixture
def repo_root(tmp_path):
    """A fake repository root. No modules/ directory exists until a test adds one."""
    return tmp_path


@pytest.fixture
def make_module(repo_root):
    """Factory creating a fake installed module under ``<repo_root>/modules/<dir_name>``.

    - ``manifest`` is written as ``module.manifest.json`` (dict serialized to
      JSON, or a raw string written verbatim for malformed-JSON cases).
    - ``commands`` / ``errors``, when given, are written to the relative path the
      manifest declares (falling back to conventional filenames).
    - ``lambda_mains`` is an iterable of module-relative Lambda folders that get
      a stub ``main.cpp``.
    - ``extra_files`` is an iterable of ``(relative_path, content)`` pairs.

    Returns the module directory path.
    """

    def _make_module(
        dir_name,
        manifest,
        *,
        commands=None,
        errors=None,
        lambda_mains=(),
        extra_files=(),
    ):
        module_dir = repo_root / "modules" / dir_name
        module_dir.mkdir(parents=True, exist_ok=True)
        manifest_text = (
            manifest if isinstance(manifest, str) else json.dumps(manifest, indent=2)
        )
        (module_dir / "module.manifest.json").write_text(manifest_text, encoding="utf-8")

        if commands is not None:
            rel = "discord.commands.json"
            if isinstance(manifest, dict) and isinstance(manifest.get("commands"), str):
                rel = manifest["commands"]
            path = module_dir / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(commands, indent=2), encoding="utf-8")

        if errors is not None:
            rel = "discord.errors.json"
            if isinstance(manifest, dict) and isinstance(manifest.get("errors"), str):
                rel = manifest["errors"]
            path = module_dir / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(errors, indent=2), encoding="utf-8")

        for rel in lambda_mains:
            main_cpp = module_dir / rel / "main.cpp"
            main_cpp.parent.mkdir(parents=True, exist_ok=True)
            main_cpp.write_text("// test fixture\n", encoding="utf-8")

        for rel, content in extra_files:
            path = module_dir / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")

        return module_dir

    return _make_module
