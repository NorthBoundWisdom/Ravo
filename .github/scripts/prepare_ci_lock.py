#!/usr/bin/env python3
"""Patch the active FreeCM lock after ``--init``, the same way a local machine does.

``--init`` copies ``source_roots.lock.jsonc.in`` to the gitignored
``source_roots.lock.jsonc``. This script then rewrites runner paths in that
generated lock so ``--update`` can emit presets with the CI Qt/host prefix,
package runtime roots, and ``cmakeEnvironment.PATH``. It never overwrites the
committed template.
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
import tempfile
from pathlib import Path
from typing import Any

USER_PLACEHOLDER = "<user>"
VALID_PLATFORMS = ("mac", "linux", "win")
PENV_PATH = "$penv{PATH}"
PACKAGE_RUNTIME_SEARCH_PATHS_KEY = "RAVO_PACKAGE_RUNTIME_SEARCH_PATHS"


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[2]


def load_jsonc(path: Path) -> dict[str, Any]:
    freecm_root = path.resolve().parent / "FreeCM"
    if not (freecm_root / "freecm" / "jsonc.py").is_file():
        freecm_root = repo_root_from_script() / "FreeCM"
    if str(freecm_root) not in sys.path:
        sys.path.insert(0, str(freecm_root))
    from freecm.jsonc import loads_jsonc

    data = loads_jsonc(path.read_text(encoding="utf-8"), path_label=str(path))
    if not isinstance(data, dict):
        raise ValueError(f"lock is not an object: {path}")
    return data


def normalize_path_entries(entries: list[str], *, separator: str) -> list[str]:
    parts: list[str] = []
    for entry in entries:
        for item in entry.replace("\\", "/").split(separator):
            text = item.strip().rstrip("/")
            if text and text != PENV_PATH:
                parts.append(text)
    if not parts:
        raise ValueError("expected at least one path entry")
    return parts


def cmake_prefix_path(entries: list[str]) -> str:
    return ";".join(normalize_path_entries(entries, separator=";"))


def cmake_path_env(entries: list[str], *, platform: str) -> str:
    separator = ";" if platform == "win" else ":"
    parts = normalize_path_entries(entries, separator=separator)
    parts.append(PENV_PATH)
    return separator.join(parts)


def package_runtime_search_paths(
    prefix_path: str,
    *,
    platform: str,
    additional_entries: list[str] | None = None,
) -> str:
    prefixes = normalize_path_entries([prefix_path], separator=";")
    suffixes = ("bin",) if platform == "win" else ("lib", "lib64")
    paths = [f"{prefix}/{suffix}" for prefix in prefixes for suffix in suffixes]
    if additional_entries:
        paths.extend(normalize_path_entries(additional_entries, separator=";"))
    return ";".join(dict.fromkeys(paths))


def apply_ci_lock(
    lock: dict[str, Any],
    *,
    platform: str,
    prefix_path: str,
    path_env: str,
    additional_runtime_paths: list[str] | None = None,
) -> dict[str, Any]:
    if platform not in VALID_PLATFORMS:
        raise ValueError(f"platform must be one of {VALID_PLATFORMS}, got {platform!r}")

    patched = json.loads(json.dumps(lock))
    dependencies = patched.get("dependencies")
    if not isinstance(dependencies, dict):
        raise ValueError("lock is missing dependencies")

    cache = patched.setdefault("cmakeCacheVariables", {})
    if not isinstance(cache, dict):
        raise ValueError("cmakeCacheVariables must be an object")
    platform_map = cache.get(platform)
    if platform_map is None:
        platform_map = {}
    if not isinstance(platform_map, dict):
        raise ValueError(f"cmakeCacheVariables.{platform} must be an object")
    updated_platform = dict(platform_map)
    updated_platform["CMAKE_PREFIX_PATH"] = prefix_path
    updated_platform[PACKAGE_RUNTIME_SEARCH_PATHS_KEY] = package_runtime_search_paths(
        prefix_path,
        platform=platform,
        additional_entries=additional_runtime_paths,
    )
    updated_platform.pop("CMAKE_TOOLCHAIN_FILE", None)
    cache[platform] = updated_platform

    environment = patched.setdefault("cmakeEnvironment", {})
    if not isinstance(environment, dict):
        raise ValueError("cmakeEnvironment must be an object")
    environment["PATH"] = path_env

    patched["depsMode"] = "pinned"
    return patched


def assert_ci_lock(
    lock: dict[str, Any],
    *,
    platform: str,
    prefix_path: str,
    path_env: str,
    additional_runtime_paths: list[str] | None = None,
) -> None:
    platform_map = lock["cmakeCacheVariables"][platform]
    encoded = json.dumps(platform_map)
    if USER_PLACEHOLDER in encoded:
        raise ValueError(f"CI lock still contains {USER_PLACEHOLDER!r} in {platform} cache")
    if platform_map.get("CMAKE_PREFIX_PATH") != prefix_path:
        raise ValueError("CI lock CMAKE_PREFIX_PATH does not match the requested prefixes")
    expected_runtime_paths = package_runtime_search_paths(
        prefix_path,
        platform=platform,
        additional_entries=additional_runtime_paths,
    )
    if platform_map.get(PACKAGE_RUNTIME_SEARCH_PATHS_KEY) != expected_runtime_paths:
        raise ValueError(
            "CI lock RAVO_PACKAGE_RUNTIME_SEARCH_PATHS does not match the requested prefixes"
        )
    if "CMAKE_TOOLCHAIN_FILE" in platform_map:
        raise ValueError("CI lock must not keep a developer vcpkg toolchain")
    if lock["cmakeEnvironment"].get("PATH") != path_env:
        raise ValueError("CI lock cmakeEnvironment.PATH does not match the requested PATH")
    if USER_PLACEHOLDER in str(lock["cmakeEnvironment"].get("PATH", "")):
        raise ValueError("CI lock cmakeEnvironment.PATH still contains a user placeholder")


def write_lock(path: Path, lock: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(lock, indent=2) + "\n", encoding="utf-8")


def self_check(repo_root: Path) -> None:
    template_path = repo_root / "source_roots.lock.jsonc.in"
    workspace_lock = (repo_root / "source_roots.lock.jsonc").resolve()
    with tempfile.TemporaryDirectory() as tmp:
        active = Path(tmp) / "source_roots.lock.jsonc"
        shutil.copyfile(template_path, active)
        init_lock = load_jsonc(active)
        linux_prefix = cmake_prefix_path(["/tmp/qt-6.11.2", "/tmp/ci-prefix"])
        linux_path = cmake_path_env(["/tmp/qt-6.11.2/bin"], platform="linux")
        linux_lock = apply_ci_lock(
            init_lock,
            platform="linux",
            prefix_path=linux_prefix,
            path_env=linux_path,
        )
        assert_ci_lock(
            linux_lock,
            platform="linux",
            prefix_path=linux_prefix,
            path_env=linux_path,
        )
        write_lock(active, linux_lock)
        geocontrols = linux_lock["dependencies"]["GeoControls"]["remote"]
        if geocontrols != "git@github.com:NorthBoundWisdom/GeoControls.git":
            raise ValueError(f"CI lock must keep the template GeoControls remote: {geocontrols}")

        win_prefix = cmake_prefix_path(
            [r"D:\a\Ravo\Qt\6.11.2\msvc2022_64", r"D:\a\Ravo\ci-prefix"]
        )
        win_path = cmake_path_env(
            [r"D:\a\Ravo\Qt\6.11.2\msvc2022_64\bin"],
            platform="win",
        )
        win_runtime_paths = [
            r"C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Redist\MSVC\14.44.35112\x64\Microsoft.VC143.CRT"
        ]
        if win_path != "D:/a/Ravo/Qt/6.11.2/msvc2022_64/bin;$penv{PATH}":
            raise ValueError(f"unexpected Windows PATH rewrite: {win_path}")
        win_lock = apply_ci_lock(
            init_lock,
            platform="win",
            prefix_path=win_prefix,
            path_env=win_path,
            additional_runtime_paths=win_runtime_paths,
        )
        assert_ci_lock(
            win_lock,
            platform="win",
            prefix_path=win_prefix,
            path_env=win_path,
            additional_runtime_paths=win_runtime_paths,
        )
        if workspace_lock == active.resolve():
            raise ValueError("self-check must not write the workspace active lock")
    print("prepare_ci_lock self-check passed")


def check_generated_preset_path(repo_root: Path, *, preset: str, required_entries: list[str]) -> None:
    presets_path = repo_root / "CMakePresets.json"
    if not presets_path.is_file():
        raise ValueError(f"generated presets are missing: {presets_path}")
    data = json.loads(presets_path.read_text(encoding="utf-8"))
    presets = data.get("configurePresets")
    if not isinstance(presets, list):
        raise ValueError("CMakePresets.json is missing configurePresets")
    match = next((item for item in presets if item.get("name") == preset), None)
    if not isinstance(match, dict):
        raise ValueError(f"generated CMakePresets.json is missing configure preset {preset!r}")
    path = str(match.get("environment", {}).get("PATH", ""))
    print(f"generated {preset} PATH={path}")
    normalized = path.replace("\\", "/")
    for entry in required_entries:
        required = entry.replace("\\", "/").rstrip("/")
        if required not in normalized:
            raise ValueError(f"generated preset PATH is missing {required}")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=repo_root_from_script(),
        help="Ravo repository root",
    )
    parser.add_argument(
        "--platform",
        choices=VALID_PLATFORMS,
        help="FreeCM cmakeCacheVariables platform key",
    )
    parser.add_argument(
        "--prefix-path",
        action="append",
        default=[],
        help="CMAKE_PREFIX_PATH entry; repeat or use ';'-separated values",
    )
    parser.add_argument(
        "--path-entry",
        action="append",
        default=[],
        help="cmakeEnvironment.PATH entry; Qt bin and other host tool dirs",
    )
    parser.add_argument(
        "--runtime-search-path",
        action="append",
        default=[],
        help="additional RAVO_PACKAGE_RUNTIME_SEARCH_PATHS entry, such as an MSVC redist directory",
    )
    parser.add_argument(
        "--lock",
        type=Path,
        default=None,
        help="active lock created by --init (default: <repo>/source_roots.lock.jsonc)",
    )
    parser.add_argument(
        "--preset",
        help="configure preset name to inspect with --check-preset",
    )
    parser.add_argument(
        "--self-check",
        action="store_true",
        help="validate rewrite rules against a copied template without writing the workspace lock",
    )
    parser.add_argument(
        "--check-preset",
        action="store_true",
        help="assert the generated CMakePresets.json PATH contains --path-entry values",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    repo_root = args.repo_root.resolve()
    if args.self_check:
        self_check(repo_root)
        return 0
    if args.check_preset:
        if args.preset is None or not args.path_entry:
            raise SystemExit("--check-preset requires --preset and --path-entry")
        check_generated_preset_path(repo_root, preset=args.preset, required_entries=args.path_entry)
        return 0
    if args.platform is None or not args.prefix_path or not args.path_entry:
        raise SystemExit(
            "--platform, --prefix-path, and --path-entry are required unless --self-check is set"
        )
    lock_path = (args.lock or repo_root / "source_roots.lock.jsonc").resolve()
    if not lock_path.is_file():
        raise SystemExit(
            f"active lock is missing: {lock_path}; run "
            "python configs/source_root_workflow.py --init first"
        )
    prefix_path = cmake_prefix_path(args.prefix_path)
    path_env = cmake_path_env(args.path_entry, platform=args.platform)
    lock = apply_ci_lock(
        load_jsonc(lock_path),
        platform=args.platform,
        prefix_path=prefix_path,
        path_env=path_env,
        additional_runtime_paths=args.runtime_search_path,
    )
    assert_ci_lock(
        lock,
        platform=args.platform,
        prefix_path=prefix_path,
        path_env=path_env,
        additional_runtime_paths=args.runtime_search_path,
    )
    write_lock(lock_path, lock)
    print(f"patched CI lock {lock_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
