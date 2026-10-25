#!/usr/bin/env python3
"""Verify the version is declared consistently across CMake, vcpkg and Conan.

Reads `project(... VERSION x.y.z)` from CMakeLists.txt, `"version"` from
vcpkg.json and `version = "x.y.z"` from conanfile.py and fails if they differ.

Usage: check_version.py [repo-root]
"""

import re
import sys
from pathlib import Path


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".")

    cmake = re.search(
        r"project\([^\n]*?\n\s*VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)",
        (root / "CMakeLists.txt").read_text(),
    )
    vcpkg = re.search(
        r'"version"\s*:\s*"([0-9]+\.[0-9]+\.[0-9]+)"',
        (root / "vcpkg.json").read_text(),
    )
    conan = re.search(
        r'^\s*version\s*=\s*"([0-9]+\.[0-9]+\.[0-9]+)"',
        (root / "conanfile.py").read_text(),
        re.MULTILINE,
    )
    pyproject = re.search(
        r'^version\s*=\s*"([0-9]+\.[0-9]+\.[0-9]+)"',
        (root / "pyproject.toml").read_text(),
        re.MULTILINE,
    )

    versions = {
        "CMakeLists.txt": cmake.group(1) if cmake else None,
        "vcpkg.json": vcpkg.group(1) if vcpkg else None,
        "conanfile.py": conan.group(1) if conan else None,
        "pyproject.toml": pyproject.group(1) if pyproject else None,
    }

    ok = True
    for name, version in versions.items():
        if version is None:
            print(f"ERROR: no version found in {name}")
            ok = False
        else:
            print(f"{name}: {version}")

    if ok and len(set(versions.values())) != 1:
        print("ERROR: versions are inconsistent")
        ok = False

    print("OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
