"""Dependency-free runner for the small function-based regression suite."""

from __future__ import annotations

import os
import runpy
import sys
from pathlib import Path


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    os.environ.setdefault("MPLCONFIGDIR", str(root / ".matplotlib"))
    sys.path.insert(0, str(root))
    passed = 0
    for path in sorted((root / "tests").glob("test_*.py")):
        namespace = runpy.run_path(str(path))
        for name, value in sorted(namespace.items()):
            if name.startswith("test_") and callable(value):
                value()
                passed += 1
                print(f"PASS {path.name}::{name}")
    print(f"{passed} tests passed")


if __name__ == "__main__":
    main()
