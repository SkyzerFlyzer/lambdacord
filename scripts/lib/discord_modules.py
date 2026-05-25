import json
from pathlib import Path


class ModuleError(RuntimeError):
    pass


def read_json(path: Path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise ModuleError(f"module file not found: {path}") from exc
    except json.JSONDecodeError as exc:
        raise ModuleError(f"invalid JSON in {path}: {exc}") from exc


def discover_modules(repo_root: Path):
    modules_root = repo_root / "modules"
    if not modules_root.exists():
        return []

    modules = []
    for manifest_path in sorted(modules_root.glob("*/module.manifest.json")):
        manifest = read_json(manifest_path)
        module_dir = manifest_path.parent
        name = manifest.get("name")
        if not isinstance(name, str) or not name:
            raise ModuleError(f"{manifest_path} must include a non-empty string name")
        modules.append({"dir": module_dir, "manifest_path": manifest_path, "manifest": manifest})
    return modules


def module_commands(module):
    manifest = module["manifest"]
    commands_file = manifest.get("commands")
    if not isinstance(commands_file, str) or not commands_file:
        raise ModuleError(f"{module['manifest_path']} must include a commands file")
    commands = read_json(module["dir"] / commands_file)
    if not isinstance(commands, list):
        raise ModuleError(f"{module['dir'] / commands_file} must contain a JSON array")
    return commands


def all_module_commands(repo_root: Path):
    commands = []
    for module in discover_modules(repo_root):
        commands.extend(module_commands(module))
    return commands


def lambda_dirs(repo_root: Path):
    dirs = [
        repo_root / "src" / "lambdas" / "discord-interactions",
        repo_root / "src" / "lambdas" / "discord-application-command-handler",
        repo_root / "src" / "lambdas" / "discord-message-component-handler",
        repo_root / "src" / "lambdas" / "discord-modal-handler",
        repo_root / "src" / "lambdas" / "discord-autocomplete-handler",
    ]

    for module in discover_modules(repo_root):
        manifest = module["manifest"]
        for rel in manifest.get("lambdas", []):
            if not isinstance(rel, str) or not rel:
                raise ModuleError(f"{module['manifest_path']} contains an invalid lambda path")
            dirs.append(module["dir"] / rel)
    return dirs


def route_map(repo_root: Path, route_kind: str):
    routes = {}
    for module in discover_modules(repo_root):
        mapping = module["manifest"].get("routes", {}).get(route_kind, {})
        if not isinstance(mapping, dict):
            raise ModuleError(f"{module['manifest_path']} routes.{route_kind} must be an object")
        for route, function_name in mapping.items():
            if route in routes:
                raise ModuleError(f"duplicate {route_kind} route: {route}")
            routes[route] = function_name
    return routes


def validate_module_manifests(repo_root: Path):
    problems = []
    for module in discover_modules(repo_root):
        manifest = module["manifest"]
        module_dir = module["dir"]
        routes = manifest.get("routes", {})
        lambdas = set(manifest.get("lambdas", []))

        for rel in sorted(lambdas):
            lambda_dir = module_dir / rel
            if not (lambda_dir / "main.cpp").exists():
                problems.append(f"{manifest['name']} lambda missing main.cpp: {lambda_dir}")

        for route_kind in ("commands", "components", "modals", "autocomplete"):
            mapping = routes.get(route_kind, {})
            if not isinstance(mapping, dict):
                problems.append(f"{module['manifest_path']} routes.{route_kind} must be an object")
                continue
            for route, function_name in mapping.items():
                if not isinstance(route, str) or not route:
                    problems.append(f"{module['manifest_path']} has an invalid {route_kind} route")
                if not isinstance(function_name, str) or not function_name:
                    problems.append(
                        f"{module['manifest_path']} route {route!r} has an invalid function name"
                    )

        error_mapper = manifest.get("error_mapper", {})
        mapper_path = error_mapper.get("path")
        if isinstance(mapper_path, str) and mapper_path:
            if not (module_dir / mapper_path / "main.cpp").exists():
                problems.append(f"{manifest['name']} error mapper missing main.cpp: {mapper_path}")

        errors_file = manifest.get("errors")
        if isinstance(errors_file, str) and errors_file:
            errors = read_json(module_dir / errors_file)
            if not isinstance(errors.get("errors"), dict):
                problems.append(f"{module_dir / errors_file} must contain an errors object")

        terraform_dir = manifest.get("terraform")
        if isinstance(terraform_dir, str) and terraform_dir:
            if not (module_dir / terraform_dir / "main.tf").exists():
                problems.append(f"{manifest['name']} terraform missing main.tf: {terraform_dir}")

    if problems:
        raise ModuleError("; ".join(problems))
