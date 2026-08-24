#!/usr/bin/env python3
"""Build a deterministic inventory of the legacy image-regression assets.

The manifest deliberately records the complete committed static reference set.
It never configures, builds, or invokes the frozen legacy project; runtime
acceptance evidence belongs to Ravo.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path
from typing import Any


OPERATION_PATTERN = re.compile(r'operation="([^"]+)"')
# The document version is an attribute on the rdf:Description element, not a
# <darktable> XML element. Record that actual legacy XMP version so a fixture
# hash cannot conceal a schema change.
SCHEMA_PATTERN = re.compile(r'darktable:xmp_version="([^"]+)"')
TEXT_FIXTURE_NAMES = frozenset({"cpugpu.maxpix"})


def canonical_fixture_bytes(path: Path) -> bytes:
    """Return fixture bytes with Git-style line endings for text references."""
    contents = path.read_bytes()
    if path.suffix == ".xmp" or path.name in TEXT_FIXTURE_NAMES:
        return contents.replace(b"\r\n", b"\n")
    return contents


def sha256(contents: bytes) -> str:
    return hashlib.sha256(contents).hexdigest()


def relative(root: Path, path: Path) -> str:
    return path.relative_to(root).as_posix()


def file_record(root: Path, path: Path) -> dict[str, Any]:
    contents = canonical_fixture_bytes(path)
    return {
        "path": relative(root, path),
        "sha256": sha256(contents),
        "size_bytes": len(contents),
    }


def xmp_metadata(path: Path) -> tuple[list[str], list[str]]:
    text = path.read_text(encoding="utf-8", errors="replace")
    return (
        sorted(set(OPERATION_PATTERN.findall(text))),
        sorted(set(SCHEMA_PATTERN.findall(text))),
    )


def build_manifest(repository_root: Path) -> dict[str, Any]:
    tests_root = repository_root / "legacy" / "tests"
    fixture_directories = sorted(
        path for path in tests_root.iterdir() if path.is_dir() and re.match(r"^\d{4}-", path.name)
    )

    fixtures: list[dict[str, Any]] = []
    all_operations: set[str] = set()
    all_schema_versions: set[str] = set()
    for directory in fixture_directories:
        xmp_files = sorted(directory.glob("*.xmp"))
        xmp_metadata_records: list[dict[str, Any]] = []
        for xmp_path in xmp_files:
            operations, schema_versions = xmp_metadata(xmp_path)
            all_operations.update(operations)
            all_schema_versions.update(schema_versions)
            xmp_metadata_records.append(
                {
                    **file_record(repository_root, xmp_path),
                    "operations": operations,
                    "schema_versions": schema_versions,
                }
            )

        references = [
            file_record(repository_root, path)
            for path in sorted(directory.glob("expected.png"))
        ]
        cpu_gpu_limits = [
            {
                **file_record(repository_root, path),
                "maximum_differing_pixels": path.read_text(encoding="ascii").strip(),
            }
            for path in sorted(directory.glob("cpugpu.maxpix"))
        ]
        fixtures.append(
            {
                "id": directory.name,
                "xmp": xmp_metadata_records,
                "expected_png": references,
                "cpu_gpu_limits": cpu_gpu_limits,
                "has_custom_driver": (directory / "test.sh").is_file(),
            }
        )

    image_records = [
        file_record(repository_root, path)
        for path in sorted((tests_root / "images").iterdir())
        if path.is_file()
    ]
    return {
        "manifest_schema_version": 1,
        "state": "frozen-static-reference",
        "notice": (
            "Hashes identify committed legacy assets. They do not certify that a legacy "
            "CPU run reproduced expected_png; consult Ravo/docs/phase0/legacy-baseline-2026-07-21.md."
        ),
        "fixture_count": len(fixtures),
        "legacy_xmp_schema_versions": sorted(all_schema_versions),
        "operations": sorted(all_operations),
        "source_images": image_records,
        "fixtures": fixtures,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repository-root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="DarkTableNext repository root (default: inferred from this script)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="manifest path (default: Ravo/tests/fixtures/legacy_manifest.json)",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify that the existing manifest matches the committed fixture assets without writing",
    )
    arguments = parser.parse_args()
    repository_root = arguments.repository_root.resolve()
    output = arguments.output or repository_root / "Ravo" / "tests" / "fixtures" / "legacy_manifest.json"
    manifest_text = json.dumps(build_manifest(repository_root), indent=2, sort_keys=True) + "\n"
    if arguments.check:
        if not output.is_file():
            print(f"legacy manifest is missing: {output}")
            return 1
        if output.read_text(encoding="utf-8") != manifest_text:
            print(f"legacy manifest differs from committed fixture assets: {output}")
            return 1
        return 0

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(manifest_text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
