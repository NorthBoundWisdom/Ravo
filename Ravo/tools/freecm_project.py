#!/usr/bin/env python3
# Usage: python3 Ravo/tools/freecm_project.py --action Configure --configuration Debug
"""Optional CLI wrappers around the same cmake --preset commands FreeCM uses."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
from typing import Any


def load_host_preset_name(repository_root: Path, configuration: str) -> str:
    if sys.platform == "darwin":
        return f"mac_clang_{configuration.lower()}"
    if sys.platform.startswith("linux"):
        return f"linux_clang_{configuration.lower()}"
    raise RuntimeError(
        "Use freecm_project.ps1 for the Windows MSVC project commands."
    )


def load_host_preset(repository_root: Path, preset_name: str) -> dict[str, Any]:
    preset_path = repository_root / "CMakePresets.json"
    document = json.loads(preset_path.read_text(encoding="utf-8"))
    for preset in document.get("configurePresets", []):
        if preset.get("name") == preset_name:
            return preset
    raise RuntimeError(
        f"FreeCM did not generate the required host preset {preset_name!r}; "
        "run the source-root update first."
    )


def run(command: list[str], *, cwd: Path) -> None:
    subprocess.run(command, cwd=cwd, check=True)


def studio_executable(build_directory: Path) -> Path:
    desktop = build_directory / "Ravo" / "desktop"
    if sys.platform == "win32":
        return desktop / "ravo_studio.exe"
    if sys.platform == "darwin":
        return desktop / "ravo_studio.app" / "Contents" / "MacOS" / "ravo_studio"
    return desktop / "ravo_studio"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--action",
        required=True,
        choices=("Configure", "Build", "Run", "Test", "Install"),
    )
    parser.add_argument(
        "--configuration", required=True, choices=("Debug", "Release")
    )
    arguments = parser.parse_args()

    repository_root = Path(__file__).resolve().parents[2]
    preset_name = load_host_preset_name(repository_root, arguments.configuration)
    load_host_preset(repository_root, preset_name)
    build_directory = repository_root / "build" / preset_name
    install_directory = repository_root / "install" / preset_name

    if arguments.action == "Configure":
        testing = "ON" if arguments.configuration == "Debug" else "OFF"
        run(
            ["cmake", "--preset", preset_name, f"-DBUILD_TESTING={testing}"],
            cwd=repository_root,
        )
    elif arguments.action == "Build":
        run(
            ["cmake", "--build", str(build_directory), "--parallel"],
            cwd=repository_root,
        )
    elif arguments.action == "Run":
        run(
            [
                "cmake",
                "--build",
                str(build_directory),
                "--target",
                "ravo_studio",
                "--parallel",
            ],
            cwd=repository_root,
        )
        run([str(studio_executable(build_directory))], cwd=repository_root)
    elif arguments.action == "Test":
        run(
            ["cmake", "--preset", preset_name, "-DBUILD_TESTING=ON"],
            cwd=repository_root,
        )
        run(
            ["cmake", "--build", str(build_directory), "--parallel"],
            cwd=repository_root,
        )
        run(
            ["ctest", "--test-dir", str(build_directory), "--output-on-failure"],
            cwd=repository_root,
        )
    else:
        run(
            ["cmake", "--build", str(build_directory), "--parallel"],
            cwd=repository_root,
        )
        run(
            [
                "cmake",
                "--install",
                str(build_directory),
                "--prefix",
                str(install_directory),
            ],
            cwd=repository_root,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
