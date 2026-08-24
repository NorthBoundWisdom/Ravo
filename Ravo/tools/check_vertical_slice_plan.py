#!/usr/bin/env python3
"""Verify that the Phase 0 vertical-slice candidate matches the fixture manifest."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


class PlanError(Exception):
    """Raised when the candidate plan no longer matches the frozen inventory."""


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise PlanError(f"cannot read JSON document {path}: {error}") from error
    if not isinstance(value, dict):
        raise PlanError(f"expected a JSON object: {path}")
    return value


def require_equal(name: str, actual: Any, expected: Any) -> None:
    if actual != expected:
        raise PlanError(f"{name} does not match legacy_manifest.json")


def validate_plan(plan: dict[str, Any], manifest: dict[str, Any]) -> None:
    require_equal("plan_schema_version", plan.get("plan_schema_version"), 1)
    require_equal("state", plan.get("state"), "candidate-not-frozen")

    source_images = {record["path"]: record for record in manifest.get("source_images", [])}
    source = plan.get("source_image")
    if not isinstance(source, dict) or source.get("path") not in source_images:
        raise PlanError("source_image is not recorded by legacy_manifest.json")
    require_equal("source_image", source, source_images[source["path"]])

    fixture_index = {record["id"]: record for record in manifest.get("fixtures", [])}
    fixtures = plan.get("fixtures")
    if not isinstance(fixtures, list) or len(fixtures) != 2:
        raise PlanError("candidate plan must name exactly neutral and visible-operation fixtures")

    expected_roles = {"0000-nop": "neutral", "0001-exposure": "visible-operation"}
    for candidate in fixtures:
        if not isinstance(candidate, dict):
            raise PlanError("candidate fixture is not an object")
        fixture_id = candidate.get("id")
        if fixture_id not in expected_roles:
            raise PlanError(f"unexpected candidate fixture: {fixture_id}")
        require_equal(f"role for {fixture_id}", candidate.get("role"), expected_roles[fixture_id])
        require_equal(
            f"legacy CPU status for {fixture_id}",
            candidate.get("legacy_cpu_status"),
            "not-run-by-policy",
        )

        fixture = fixture_index.get(fixture_id)
        if fixture is None or len(fixture.get("xmp", [])) != 1 or len(fixture.get("expected_png", [])) != 1:
            raise PlanError(f"fixture inventory is incomplete for {fixture_id}")
        expected_xmp = {
            key: fixture["xmp"][0][key] for key in ("path", "sha256", "size_bytes")
        }
        require_equal(f"XMP record for {fixture_id}", candidate.get("xmp"), expected_xmp)
        require_equal(
            f"expected PNG record for {fixture_id}",
            candidate.get("expected_png"),
            fixture["expected_png"][0],
        )

    mapping = plan.get("ravo_mapping", {}).get("ravo.core.exposure")
    if not isinstance(mapping, dict):
        raise PlanError("ravo.core.exposure mapping is missing")
    require_equal("exposure legacy operation", mapping.get("legacy_operation"), "exposure")
    require_equal("exposure mapping state", mapping.get("status"), "restricted-proven")
    require_equal("exposure canonical recipe", mapping.get("canonical_recipe"), None)
    require_equal(
        "exposure mapping constraints",
        mapping.get("constraints"),
        [
            "legacy XMP schema 6",
            "exposure module version 5",
            "manual mode",
            "zero black-level correction",
            "no blend data",
            "no mask history",
            "singleton exposure history",
        ],
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repository-root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="Ravo repository root (default: inferred from this script)",
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        default=None,
        help="legacy manifest path (default: Ravo/tests/fixtures/legacy_manifest.json)",
    )
    parser.add_argument(
        "--plan",
        type=Path,
        default=None,
        help="vertical-slice plan path (default: Ravo/tests/fixtures/vertical_slice_plan.json)",
    )
    arguments = parser.parse_args()
    repository_root = arguments.repository_root.resolve()
    manifest_path = arguments.manifest or repository_root / "Ravo" / "tests" / "fixtures" / "legacy_manifest.json"
    plan_path = arguments.plan or repository_root / "Ravo" / "tests" / "fixtures" / "vertical_slice_plan.json"
    try:
        validate_plan(load_json(plan_path), load_json(manifest_path))
    except PlanError as error:
        print(f"vertical-slice plan check failed: {error}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
