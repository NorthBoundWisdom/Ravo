#!/usr/bin/env python3
"""Create a gitignored CI source-root lock from the committed template.

The generated lock is local state: it never overwrites
``source_roots.lock.jsonc.in``. CI uses it so ``--init`` / ``--update`` can
run without machine paths such as ``/Users/<user>/...`` or SSH remotes.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import tempfile
from pathlib import Path
from typing import Any

SSH_GITHUB_REMOTE = re.compile(r"^git@github\.com:(.+)$")
USER_PLACEHOLDER = "<user>"
VALID_PLATFORMS = ("mac", "linux", "win")


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[2]


def load_jsonc(path: Path) -> dict[str, Any]:
    freecm_root = path.resolve().parent / "FreeCM"
    if str(freecm_root) not in sys.path:
        sys.path.insert(0, str(freecm_root))
    from freecm.jsonc import loads_jsonc

    data = loads_jsonc(path.read_text(encoding="utf-8"), path_label=str(path))
    if not isinstance(data, dict):
        raise ValueError(f"lock template is not an object: {path}")
    return data


def to_https_github_remote(remote: str) -> str:
    match = SSH_GITHUB_REMOTE.match(remote.strip())
    if match is None:
        return remote
    return f"https://github.com/{match.group(1)}"


def cmake_prefix_path(entries: list[str]) -> str:
    parts: list[str] = []
    for entry in entries:
        for item in entry.replace("\\", "/").split(";"):
            text = item.strip()
            if text:
                parts.append(text.rstrip("/"))
    if not parts:
        raise ValueError("CMAKE_PREFIX_PATH requires at least one prefix")
    return ";".join(parts)


def prepare_ci_lock(
    template: dict[str, Any],
    *,
    platform: str,
    prefix_path: str,
) -> dict[str, Any]:
    if platform not in VALID_PLATFORMS:
        raise ValueError(f"platform must be one of {VALID_PLATFORMS}, got {platform!r}")

    lock = json.loads(json.dumps(template))
    dependencies = lock.get("dependencies")
    if not isinstance(dependencies, dict):
        raise ValueError("lock template is missing dependencies")
    for name, spec in dependencies.items():
        if not isinstance(spec, dict) or "remote" not in spec:
            raise ValueError(f"dependency {name!r} is missing a remote")
        spec["remote"] = to_https_github_remote(str(spec["remote"]))

    cache = lock.setdefault("cmakeCacheVariables", {})
    if not isinstance(cache, dict):
        raise ValueError("cmakeCacheVariables must be an object")
    cache[platform] = {
        "CMAKE_PREFIX_PATH": prefix_path,
        "CMAKE_C_COMPILER_LAUNCHER": "ccache",
        "CMAKE_CXX_COMPILER_LAUNCHER": "ccache",
    }
    lock["depsMode"] = "pinned"
    return lock


def assert_ci_lock(lock: dict[str, Any], *, platform: str, prefix_path: str) -> None:
    platform_map = lock["cmakeCacheVariables"][platform]
    if USER_PLACEHOLDER in json.dumps(platform_map):
        raise ValueError(f"CI lock still contains {USER_PLACEHOLDER!r} in {platform} cache")
    if platform_map.get("CMAKE_PREFIX_PATH") != prefix_path:
        raise ValueError("CI lock CMAKE_PREFIX_PATH does not match the requested prefixes")
    if "CMAKE_TOOLCHAIN_FILE" in platform_map:
        raise ValueError("CI lock must not keep a developer vcpkg toolchain")
    for spec in lock["dependencies"].values():
        remote = str(spec["remote"])
        if remote.startswith("git@"):
            raise ValueError(f"CI lock still has an SSH remote: {remote}")


def write_lock(path: Path, lock: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(lock, indent=2) + "\n", encoding="utf-8")


def self_check(repo_root: Path) -> None:
    template_path = repo_root / "source_roots.lock.jsonc.in"
    template = load_jsonc(template_path)
    prefix_path = cmake_prefix_path(["/tmp/qt-6.11.2", "/tmp/ci-prefix"])
    with tempfile.TemporaryDirectory() as tmp:
        output = Path(tmp) / "source_roots.lock.jsonc"
        lock = prepare_ci_lock(template, platform="linux", prefix_path=prefix_path)
        assert_ci_lock(lock, platform="linux", prefix_path=prefix_path)
        write_lock(output, lock)
        geocontrols = lock["dependencies"]["GeoControls"]["remote"]
        if geocontrols != "https://github.com/NorthBoundWisdom/GeoControls.git":
            raise ValueError(f"unexpected GeoControls remote after rewrite: {geocontrols}")
        if (repo_root / "source_roots.lock.jsonc").resolve() == output.resolve():
            raise ValueError("self-check must not write the workspace active lock")
    print("prepare_ci_lock self-check passed")


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
        "--output",
        type=Path,
        default=None,
        help="active lock path (default: <repo>/source_roots.lock.jsonc)",
    )
    parser.add_argument(
        "--self-check",
        action="store_true",
        help="validate rewrite rules against the committed template without writing the workspace lock",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    repo_root = args.repo_root.resolve()
    if args.self_check:
        self_check(repo_root)
        return 0
    if args.platform is None or not args.prefix_path:
        raise SystemExit("--platform and --prefix-path are required unless --self-check is set")
    prefix_path = cmake_prefix_path(args.prefix_path)
    template_path = repo_root / "source_roots.lock.jsonc.in"
    lock = prepare_ci_lock(
        load_jsonc(template_path),
        platform=args.platform,
        prefix_path=prefix_path,
    )
    assert_ci_lock(lock, platform=args.platform, prefix_path=prefix_path)
    output = (args.output or repo_root / "source_roots.lock.jsonc").resolve()
    write_lock(output, lock)
    print(f"wrote CI lock {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
