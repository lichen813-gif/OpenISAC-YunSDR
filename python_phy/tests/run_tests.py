"""Dependency-free runner for the small function-based regression suite."""

from __future__ import annotations

import os
import inspect
import runpy
import sys
from pathlib import Path


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    os.environ.setdefault("MPLCONFIGDIR", str(root / ".matplotlib"))
    sys.path.insert(0, str(root))
    passed = 0
    skipped = 0
    for path in sorted((root / "tests").glob("test_*.py")):
        try:
            namespace = runpy.run_path(str(path))
        except ModuleNotFoundError as error:
            print(f"SKIP {path.name}: optional dependency {error.name!r} is unavailable")
            skipped += 1
            continue
        for name, value in sorted(namespace.items()):
            if name.startswith("test_") and callable(value):
                required = [
                    parameter
                    for parameter in inspect.signature(value).parameters.values()
                    if parameter.default is inspect.Parameter.empty
                    and parameter.kind
                    in (inspect.Parameter.POSITIONAL_ONLY,
                        inspect.Parameter.POSITIONAL_OR_KEYWORD,
                        inspect.Parameter.KEYWORD_ONLY)
                ]
                if required:
                    print(
                        f"SKIP {path.name}::{name}: requires pytest fixture(s) "
                        + ", ".join(parameter.name for parameter in required)
                    )
                    skipped += 1
                    continue
                value()
                passed += 1
                print(f"PASS {path.name}::{name}")
    print(f"{passed} tests passed, {skipped} optional/fixture tests skipped")


if __name__ == "__main__":
    main()
