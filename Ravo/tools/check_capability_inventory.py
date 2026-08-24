#!/usr/bin/env python3
"""Verify the Phase 0 IOP census against frozen CMake and fixture inputs."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any


IOP_PATTERN = re.compile(r"^\s*add_iop\(\s*([A-Za-z0-9_]+)", re.MULTILINE)
TABLE_HEADING = "## Legacy registry census"
TABLE_HEADER = ["Legacy IOP", "Fixture", "Phase 0 Ravo disposition"]


class InventoryError(Exception):
    """Raised when the decision input no longer matches a frozen source fact."""


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise InventoryError(f"cannot read JSON document {path}: {error}") from error
    if not isinstance(value, dict):
        raise InventoryError(f"expected a JSON object: {path}")
    return value


def registered_iops(path: Path) -> set[str]:
    try:
        names = IOP_PATTERN.findall(path.read_text(encoding="utf-8"))
    except OSError as error:
        raise InventoryError(f"cannot read IOP CMake registry {path}: {error}") from error
    if not names:
        raise InventoryError("IOP CMake registry contains no add_iop registrations")
    if len(set(names)) != len(names):
        raise InventoryError("IOP CMake registry contains a duplicate registration")
    return set(names)


def manifest_operations(manifest: dict[str, Any]) -> set[str]:
    fixtures = manifest.get("fixtures")
    if not isinstance(fixtures, list):
        raise InventoryError("legacy manifest has no fixture list")
    operations: set[str] = set()
    for fixture in fixtures:
        if not isinstance(fixture, dict) or not isinstance(fixture.get("xmp"), list):
            raise InventoryError("legacy manifest contains an invalid fixture XMP record")
        for xmp in fixture["xmp"]:
            if not isinstance(xmp, dict) or not isinstance(xmp.get("operations"), list):
                raise InventoryError("legacy manifest contains an invalid XMP operation record")
            for operation in xmp["operations"]:
                if not isinstance(operation, str) or not operation:
                    raise InventoryError("legacy manifest contains an invalid operation name")
                operations.add(operation)
    return operations


def cells(line: str) -> list[str]:
    return [cell.strip() for cell in line.strip().strip("|").split("|")]


def table_rows(path: Path) -> dict[str, tuple[str, str]]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise InventoryError(f"cannot read capability inventory {path}: {error}") from error
    try:
        heading_index = lines.index(TABLE_HEADING)
    except ValueError as error:
        raise InventoryError(f"capability inventory is missing {TABLE_HEADING!r}") from error
    table_lines = lines[heading_index + 1 :]
    if len(table_lines) < 3 or cells(table_lines[1]) != TABLE_HEADER:
        raise InventoryError("capability inventory table header changed")
    if not table_lines[2].startswith("|"):
        raise InventoryError("capability inventory table separator is missing")

    rows: dict[str, tuple[str, str]] = {}
    for line in table_lines[3:]:
        if not line.startswith("|"):
            break
        row = cells(line)
        if len(row) != 3:
            raise InventoryError("capability inventory table has a malformed row")
        operation = row[0].strip("`")
        fixture = row[1]
        disposition = row[2]
        if not operation or fixture not in {"yes", "no"} or not disposition:
            raise InventoryError(f"capability inventory row is invalid: {line}")
        if operation in rows:
            raise InventoryError(f"capability inventory repeats operation {operation}")
        rows[operation] = (fixture, disposition)
    if not rows:
        raise InventoryError("capability inventory contains no IOP rows")
    return rows


def verify(repository_root: Path) -> None:
    manifest_path = repository_root / "Ravo" / "tests" / "fixtures" / "legacy_manifest.json"
    inventory_path = repository_root / "Ravo" / "docs" / "phase0" / "capability-inventory.md"
    cmake_path = repository_root / "legacy" / "src" / "iop" / "CMakeLists.txt"
    registry = registered_iops(cmake_path)
    operations = manifest_operations(load_json(manifest_path))
    rows = table_rows(inventory_path)
    if set(rows) != registry:
        missing = sorted(registry - set(rows))
        extra = sorted(set(rows) - registry)
        details = []
        if missing:
            details.append("missing: " + ", ".join(missing))
        if extra:
            details.append("unexpected: " + ", ".join(extra))
        raise InventoryError("capability inventory registry set differs from legacy/src/iop/CMakeLists.txt; " + "; ".join(details))
    for operation, (fixture, _disposition) in rows.items():
        expected = "yes" if operation in operations else "no"
        if fixture != expected:
            raise InventoryError(
                f"capability inventory fixture flag for {operation} is {fixture}, expected {expected}"
            )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repository-root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="DarkTableNext repository root (default: inferred from this script)",
    )
    arguments = parser.parse_args()
    try:
        verify(arguments.repository_root.resolve())
    except InventoryError as error:
        print(f"capability inventory check failed: {error}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
