#!/usr/bin/env python3
# Usage: python3 Ravo/tools/freecm_project.py --action Configure --configuration Debug
"""Run cross-platform FreeCM project actions against the Ravo source tree."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import Any


def load_host_preset(repository_root: Path, configuration: str) -> dict[str, Any]:
    if sys.platform == "darwin":
        preset_name = f"mac_clang_{configuration.lower()}"
    elif sys.platform.startswith("linux"):
        preset_name = f"linux_clang_{configuration.lower()}"
    else:
        raise RuntimeError(
            "Use freecm_project.ps1 for the Windows MSVC project commands."
        )

    preset_path = repository_root / "CMakePresets.json"
    document = json.loads(preset_path.read_text(encoding="utf-8"))
    for preset in document.get("configurePresets", []):
        if preset.get("name") == preset_name:
            return preset
    raise RuntimeError(
        f"FreeCM did not generate the required host preset {preset_name!r}; "
        "run the source-root update first."
    )


def expand_parent_environment(value: str, environment: dict[str, str]) -> str:
    return re.sub(
        r"\$penv\{([^}]+)\}",
        lambda match: environment.get(match.group(1), ""),
        value,
    )


def expand_preset_variables(
    value: str,
    *,
    repository_root: Path,
    preset_name: str,
) -> str:
    return (
        value.replace("${sourceDir}", str(repository_root))
        .replace("${presetName}", preset_name)
    )


def run(command: list[str], *, cwd: Path, environment: dict[str, str]) -> None:
    subprocess.run(command, cwd=cwd, env=environment, check=True)


def configure_command(
    repository_root: Path,
    ravo_root: Path,
    build_directory: Path,
    preset: dict[str, Any],
    testing: bool,
) -> list[str]:
    command = [
        "cmake",
        "-S",
        str(ravo_root),
        "-B",
        str(build_directory),
        "-G",
        str(preset["generator"]),
    ]
    for name, value in preset.get("cacheVariables", {}).items():
        if value is not None:
            resolved_value = expand_preset_variables(
                str(value),
                repository_root=repository_root,
                preset_name=str(preset["name"]),
            )
            command.append(f"-D{name}={resolved_value}")
    command.append(f"-DBUILD_TESTING={'ON' if testing else 'OFF'}")
    return command


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
    ravo_root = repository_root / "Ravo"
    host_preset = load_host_preset(repository_root, arguments.configuration)
    preset_name = f"ravo_{host_preset['name']}"
    build_directory = repository_root / "build" / preset_name
    install_directory = repository_root / "install" / preset_name
    environment = dict(os.environ)
    for name, value in host_preset.get("environment", {}).items():
        environment[name] = expand_parent_environment(str(value), environment)

    if arguments.action == "Configure":
        run(
            configure_command(
                repository_root,
                ravo_root,
                build_directory,
                host_preset,
                arguments.configuration == "Debug",
            ),
            cwd=repository_root,
            environment=environment,
        )
    elif arguments.action == "Build":
        run(
            ["cmake", "--build", str(build_directory), "--parallel"],
            cwd=repository_root,
            environment=environment,
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
            environment=environment,
        )
        studio_name = "ravo_studio.exe" if sys.platform == "win32" else "ravo_studio"
        run(
            [str(build_directory / "desktop" / studio_name)],
            cwd=repository_root,
            environment=environment,
        )
    elif arguments.action == "Test":
        run(
            configure_command(
                repository_root,
                ravo_root,
                build_directory,
                host_preset,
                True,
            ),
            cwd=repository_root,
            environment=environment,
        )
        run(
            ["cmake", "--build", str(build_directory), "--parallel"],
            cwd=repository_root,
            environment=environment,
        )
        run(
            ["ctest", "--test-dir", str(build_directory), "--output-on-failure"],
            cwd=repository_root,
            environment=environment,
        )
    else:
        run(
            ["cmake", "--build", str(build_directory), "--parallel"],
            cwd=repository_root,
            environment=environment,
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
            environment=environment,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
