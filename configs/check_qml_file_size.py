#!/usr/bin/env python3
"""Enforce monotonic removal of oversized Ravo production QML debt."""

from __future__ import annotations

import argparse
from pathlib import Path

from repo_check_utils import REPO_ROOT, emit_errors, load_jsonc


MANIFEST_REL = Path("configs/qml_file_size_budget.jsonc")
QML_ROOT = Path("Ravo/desktop/qml")
LINE_LIMIT = 2000


def line_count(path: Path) -> int:
    return path.read_bytes().count(b"\n")


def scan_qml_files(repo_root: Path) -> dict[str, int]:
    directory = repo_root / QML_ROOT
    if not directory.is_dir():
        return {}
    return {
        path.relative_to(repo_root).as_posix(): line_count(path)
        for path in sorted(directory.rglob("*.qml"))
        if path.is_file()
    }


def validate_manifest(manifest: object) -> tuple[dict[str, dict], list[str]]:
    errors: list[str] = []
    entries: dict[str, dict] = {}
    if not isinstance(manifest, dict):
        return entries, ["QML size manifest must be an object"]
    if manifest.get("schema_version") != 1:
        errors.append("QML size manifest schema_version must be 1")
    if manifest.get("line_limit") != LINE_LIMIT:
        errors.append(f"QML size manifest line_limit must be {LINE_LIMIT}")
    debt = manifest.get("debt")
    if not isinstance(debt, list):
        return entries, errors + ["QML size manifest debt must be a list"]
    for index, entry in enumerate(debt):
        label = f"debt[{index}]"
        if not isinstance(entry, dict):
            errors.append(f"{label} must be an object")
            continue
        path = entry.get("path")
        baseline_lines = entry.get("baseline_lines")
        owner = entry.get("owner")
        removal_phase = entry.get("removal_phase")
        if not isinstance(path, str) or not path.endswith(".qml"):
            errors.append(f"{label}.path must name a .qml file")
            continue
        path_object = Path(path)
        if (
            path_object.is_absolute()
            or ".." in path_object.parts
            or not path_object.is_relative_to(QML_ROOT)
        ):
            errors.append(f"{label}.path must be below {QML_ROOT.as_posix()}")
            continue
        if path in entries:
            errors.append(f"QML size manifest duplicates {path}")
            continue
        if not isinstance(baseline_lines, int) or baseline_lines <= LINE_LIMIT:
            errors.append(f"{label}.baseline_lines must exceed {LINE_LIMIT}")
        if not isinstance(owner, str) or not owner:
            errors.append(f"{label}.owner must be non-empty")
        if not isinstance(removal_phase, str) or not removal_phase:
            errors.append(f"{label}.removal_phase must be non-empty")
        entries[path] = entry
    return entries, errors


def check_contract(
    repo_root: Path = REPO_ROOT,
    manifest_rel: Path = MANIFEST_REL,
) -> list[str]:
    resolved_root = repo_root.resolve()
    manifest_path = resolved_root / manifest_rel
    if not manifest_path.is_file():
        return [f"QML size manifest missing: {manifest_rel.as_posix()}"]
    entries, errors = validate_manifest(load_jsonc(manifest_path))
    actual = scan_qml_files(resolved_root)

    for path, entry in sorted(entries.items()):
        actual_lines = actual.get(path)
        baseline_lines = entry.get("baseline_lines")
        if actual_lines is None:
            errors.append(f"registered QML debt file is missing: {path}")
            continue
        if not isinstance(baseline_lines, int):
            continue
        if actual_lines <= LINE_LIMIT:
            errors.append(
                f"retired QML debt entry remains in manifest: {path} has "
                f"{actual_lines} lines; remove it"
            )
        elif actual_lines > baseline_lines:
            errors.append(
                f"QML debt grew: {path} has {actual_lines} lines; baseline is {baseline_lines}"
            )

    for path, actual_lines in sorted(actual.items()):
        if actual_lines > LINE_LIMIT and path not in entries:
            errors.append(
                f"new oversized QML file: {path} has {actual_lines} lines; "
                "split it instead of adding debt"
            )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository-root", type=Path, default=REPO_ROOT)
    parser.add_argument("--manifest", type=Path, default=MANIFEST_REL)
    args = parser.parse_args()
    errors = check_contract(args.repository_root, args.manifest)
    if emit_errors(errors):
        return 1
    print("QML file-size ratchet passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
