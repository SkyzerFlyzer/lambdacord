#!/usr/bin/env python3

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
from discord_modules import ModuleError, route_map  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description="Print a merged module route map as JSON.")
    parser.add_argument(
        "kind",
        choices=["commands", "components", "modals", "autocomplete"],
        help="Route kind to merge from modules/*/module.manifest.json.",
    )
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    print(json.dumps(route_map(repo_root, args.kind), separators=(",", ":")))


if __name__ == "__main__":
    try:
        main()
    except ModuleError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
