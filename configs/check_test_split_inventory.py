#!/usr/bin/env python3
"""Keep Ravo test-source splits case-complete and target-preserving."""

from __future__ import annotations

import argparse
import hashlib
import re
from pathlib import Path

from repo_check_utils import REPO_ROOT, emit_errors, load_jsonc


MANIFEST_REL = Path("configs/test_split_inventory.jsonc")
TEST_CMAKE_ROOT = Path("Ravo/tests")
TEST_CMAKE_REL = TEST_CMAKE_ROOT / "CMakeLists.txt"
TEST_CASE_RE = re.compile(
    r"^\s*TEST(?:_F|_P)?\s*\(\s*(?P<suite>[^,\n]+)\s*,\s*(?P<case>[^)\n]+)\)",
    re.MULTILINE,
)
CMAKE_COMMAND_RE = re.compile(
    r"^\s*(?P<command>[A-Za-z0-9_]+)\s*\(\s*(?P<body>.*?)\)",
    re.MULTILINE | re.DOTALL,
)
CMAKE_TOKEN_RE = re.compile(r'"([^"]*)"|([^\s#]+)')


def case_inventory(text: str) -> list[str]:
    return [
        f"{match.group('suite').strip()}.{match.group('case').strip()}"
        for match in TEST_CASE_RE.finditer(text)
    ]


def inventory_hash(cases: list[str]) -> str:
    return hashlib.sha256("\n".join(cases).encode("utf-8")).hexdigest()


def cmake_tokens(body: str) -> list[str]:
    return [match.group(1) or match.group(2) for match in CMAKE_TOKEN_RE.finditer(body)]


def source_key(source: str) -> str | None:
    if not source.endswith((".cpp", ".cc", ".cxx")):
        return None
    if source.startswith("${CMAKE_CURRENT_SOURCE_DIR}/"):
        source = source.removeprefix("${CMAKE_CURRENT_SOURCE_DIR}/")
    if source.startswith("${CMAKE_SOURCE_DIR}/Ravo/tests/"):
        source = source.removeprefix("${CMAKE_SOURCE_DIR}/Ravo/tests/")
    if source.startswith("${") and "}/" in source:
        source = source.split("}/", 1)[1]
    path = Path(source)
    if path.is_absolute() or ".." in path.parts:
        return None
    return path.as_posix()


def cmake_test_membership(cmake_text: str) -> dict[str, set[str]]:
    memberships: dict[str, set[str]] = {}
    for match in CMAKE_COMMAND_RE.finditer(cmake_text):
        command = match.group("command").lower()
        tokens = cmake_tokens(match.group("body"))
        if command not in {"add_executable", "target_sources"} or len(tokens) < 2:
            continue
        target = tokens[0]
        for token in tokens[1:]:
            key = source_key(token)
            if key is not None:
                memberships.setdefault(key, set()).add(target)
    return memberships


def manifest_source_key(source: str) -> str:
    return Path(source).relative_to(TEST_CMAKE_ROOT).as_posix()


def check_contract(
    repo_root: Path = REPO_ROOT,
    manifest_rel: Path = MANIFEST_REL,
) -> list[str]:
    repo_root = repo_root.resolve()
    errors: list[str] = []
    manifest_path = repo_root / manifest_rel
    cmake_path = repo_root / TEST_CMAKE_REL
    if not manifest_path.is_file():
        return [f"test split inventory missing: {manifest_rel.as_posix()}"]
    if not cmake_path.is_file():
        return [f"test split inventory CMake source missing: {TEST_CMAKE_REL.as_posix()}"]

    manifest = load_jsonc(manifest_path)
    if not isinstance(manifest, dict) or manifest.get("schema_version") != 1:
        return ["test split inventory schema_version must be 1"]
    groups = manifest.get("groups")
    if not isinstance(groups, list) or not groups:
        return ["test split inventory groups must be a non-empty list"]

    memberships = cmake_test_membership(cmake_path.read_text(encoding="utf-8"))
    seen_sources: set[str] = set()
    for index, group in enumerate(groups):
        label = f"groups[{index}]"
        if not isinstance(group, dict):
            errors.append(f"{label} must be an object")
            continue
        sources = group.get("sources")
        expected_count = group.get("case_count")
        expected_hash = group.get("case_sha256")
        if not isinstance(sources, list) or not sources or not all(
            isinstance(source, str) and source.endswith(".cpp") for source in sources
        ):
            errors.append(f"{label}.sources must be a non-empty list of .cpp paths")
            continue
        if not isinstance(expected_count, int) or expected_count < 0:
            errors.append(f"{label}.case_count must be a non-negative integer")
            continue
        if not isinstance(expected_hash, str) or len(expected_hash) != 64:
            errors.append(f"{label}.case_sha256 must be a SHA-256 hex string")
            continue

        cases: list[str] = []
        group_memberships: list[set[str]] = []
        for source in sources:
            if source in seen_sources:
                errors.append(f"{label} duplicates source across split groups: {source}")
                continue
            seen_sources.add(source)
            source_path = repo_root / source
            if not source_path.is_file():
                errors.append(f"{label} source is missing: {source}")
                continue
            source_cases = case_inventory(source_path.read_text(encoding="utf-8"))
            for case_name in source_cases:
                if "DISABLED_" in case_name:
                    errors.append(f"{label} disables a preserved test case: {case_name}")
            cases.extend(source_cases)
            try:
                key = manifest_source_key(source)
            except ValueError:
                errors.append(f"{label} source is outside {TEST_CMAKE_ROOT.as_posix()}: {source}")
                continue
            membership = memberships.get(key, set())
            if not membership:
                errors.append(f"{label} source is absent from CMake target membership: {source}")
            group_memberships.append(membership)

        if len(cases) != expected_count:
            errors.append(f"{label} case count drifted: {len(cases)}, expected {expected_count}")
        if inventory_hash(cases) != expected_hash:
            errors.append(f"{label} ordered case inventory drifted")
        if group_memberships and any(
            membership != group_memberships[0] for membership in group_memberships[1:]
        ):
            errors.append(f"{label} split sources no longer share the same CMake target membership")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository-root", type=Path, default=REPO_ROOT)
    parser.add_argument("--manifest", type=Path, default=MANIFEST_REL)
    args = parser.parse_args()
    errors = check_contract(args.repository_root, args.manifest)
    if emit_errors(errors):
        return 1
    print("test split inventory checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
