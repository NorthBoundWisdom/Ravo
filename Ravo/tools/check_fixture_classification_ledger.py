#!/usr/bin/env python3
"""Create or validate the non-acceptance classification ledger for legacy fixtures.

The ledger is deliberately separate from the hash manifest.  The manifest says
which immutable fixture assets exist; the ledger records whether a fixture has
enough evidence for a later Ravo disposition.  It never certifies a Ravo render.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any


LEDGER_SCHEMA_VERSION = 1
LEDGER_STATE = "classification-pending"
UNCLASSIFIED = "unclassified"
UNCLASSIFIED_RATIONALE = (
    "No product disposition or Ravo CPU acceptance evidence is recorded; static fixture presence is complete."
)
CLASSIFICATIONS = frozenset(
    {
        "frozen_fixture_reference",
        "missing_asset",
        "product_approved_incompatibility",
        "deferred_ravo_operation",
        UNCLASSIFIED,
    }
)


class LedgerError(Exception):
    """Raised when the ledger no longer faithfully covers the fixture manifest."""


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise LedgerError(f"cannot read JSON document {path}: {error}") from error
    if not isinstance(value, dict):
        raise LedgerError(f"expected a JSON object: {path}")
    return value


def manifest_fixture_ids(manifest: dict[str, Any]) -> list[str]:
    fixtures = manifest.get("fixtures")
    if not isinstance(fixtures, list):
        raise LedgerError("legacy manifest has no fixture list")
    fixture_ids = [fixture.get("id") for fixture in fixtures if isinstance(fixture, dict)]
    if len(fixture_ids) != len(fixtures) or not all(isinstance(fixture_id, str) for fixture_id in fixture_ids):
        raise LedgerError("legacy manifest contains a fixture without a string id")
    if len(set(fixture_ids)) != len(fixture_ids):
        raise LedgerError("legacy manifest contains duplicate fixture ids")
    return sorted(fixture_ids)


def fixture_index_sha256(fixture_ids: list[str]) -> str:
    canonical = "".join(f"{fixture_id}\n" for fixture_id in fixture_ids).encode("utf-8")
    return hashlib.sha256(canonical).hexdigest()


def unclassified_entry(fixture_id: str) -> dict[str, Any]:
    return {
        "classification": UNCLASSIFIED,
        "evidence": [],
        "id": fixture_id,
        "rationale": UNCLASSIFIED_RATIONALE,
    }


def build_initial_ledger(manifest: dict[str, Any]) -> dict[str, Any]:
    fixture_ids = manifest_fixture_ids(manifest)
    return {
        "classification_ledger_schema_version": LEDGER_SCHEMA_VERSION,
        "entry_count": len(fixture_ids),
        "fixture_index_sha256": fixture_index_sha256(fixture_ids),
        "ledger_state": LEDGER_STATE,
        "legacy_manifest_schema_version": manifest.get("manifest_schema_version"),
        "notice": (
            "This ledger is pre-acceptance evidence only. A classification does not certify "
            "a legacy CPU result or an accepted Ravo image result."
        ),
        "entries": [unclassified_entry(fixture_id) for fixture_id in fixture_ids],
    }


def require_exact_keys(value: dict[str, Any], expected: set[str], context: str) -> None:
    if set(value) != expected:
        raise LedgerError(f"{context} must contain exactly: {', '.join(sorted(expected))}")


def validate_evidence(entry: dict[str, Any], fixture_id: str) -> None:
    evidence = entry.get("evidence")
    if not isinstance(evidence, list):
        raise LedgerError(f"fixture {fixture_id} evidence must be a list")
    if entry["classification"] == UNCLASSIFIED:
        if evidence:
            raise LedgerError(f"unclassified fixture {fixture_id} must not carry acceptance evidence")
        if entry["rationale"] != UNCLASSIFIED_RATIONALE:
            raise LedgerError(f"unclassified fixture {fixture_id} must use the standard rationale")
        return
    if not evidence:
        raise LedgerError(f"classified fixture {fixture_id} requires at least one evidence record")
    for evidence_index, record in enumerate(evidence):
        if not isinstance(record, dict):
            raise LedgerError(f"fixture {fixture_id} evidence {evidence_index} must be an object")
        require_exact_keys(record, {"reference", "summary"}, f"fixture {fixture_id} evidence {evidence_index}")
        if not all(isinstance(record[field], str) and record[field] for field in ("reference", "summary")):
            raise LedgerError(f"fixture {fixture_id} evidence {evidence_index} fields must be non-empty strings")


def validate_ledger(ledger: dict[str, Any], manifest: dict[str, Any]) -> None:
    require_exact_keys(
        ledger,
        {
            "classification_ledger_schema_version",
            "entry_count",
            "fixture_index_sha256",
            "ledger_state",
            "legacy_manifest_schema_version",
            "notice",
            "entries",
        },
        "classification ledger",
    )
    if ledger["classification_ledger_schema_version"] != LEDGER_SCHEMA_VERSION:
        raise LedgerError("unsupported classification ledger schema version")
    if ledger["ledger_state"] != LEDGER_STATE:
        raise LedgerError("classification ledger must remain pre-acceptance evidence")
    if ledger["legacy_manifest_schema_version"] != manifest.get("manifest_schema_version"):
        raise LedgerError("classification ledger references a different legacy manifest schema")

    fixture_ids = manifest_fixture_ids(manifest)
    if ledger["entry_count"] != len(fixture_ids):
        raise LedgerError("classification ledger entry count does not match the legacy manifest")
    if ledger["fixture_index_sha256"] != fixture_index_sha256(fixture_ids):
        raise LedgerError("classification ledger fixture index does not match the legacy manifest")

    entries = ledger["entries"]
    if not isinstance(entries, list):
        raise LedgerError("classification ledger entries must be a list")
    ledger_ids: list[str] = []
    for entry in entries:
        if not isinstance(entry, dict):
            raise LedgerError("classification ledger entry must be an object")
        require_exact_keys(entry, {"classification", "evidence", "id", "rationale"}, "classification ledger entry")
        fixture_id = entry["id"]
        if not isinstance(fixture_id, str) or not fixture_id:
            raise LedgerError("classification ledger entry id must be a non-empty string")
        ledger_ids.append(fixture_id)
        classification = entry["classification"]
        if not isinstance(classification, str):
            raise LedgerError(f"fixture {fixture_id} classification must be a string")
        if classification not in CLASSIFICATIONS:
            raise LedgerError(f"fixture {fixture_id} has an unsupported classification: {classification}")
        if not isinstance(entry["rationale"], str) or not entry["rationale"]:
            raise LedgerError(f"fixture {fixture_id} rationale must be a non-empty string")
        validate_evidence(entry, fixture_id)

    if sorted(ledger_ids) != fixture_ids or len(set(ledger_ids)) != len(ledger_ids):
        raise LedgerError("classification ledger fixture ids must exactly match the legacy manifest")


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
        "--ledger",
        type=Path,
        default=None,
        help="classification ledger path (default: Ravo/tests/fixtures/fixture_classification_ledger.json)",
    )
    parser.add_argument(
        "--initialize",
        action="store_true",
        help="write a new all-unclassified ledger; fails if the ledger already exists",
    )
    arguments = parser.parse_args()
    repository_root = arguments.repository_root.resolve()
    manifest_path = arguments.manifest or repository_root / "Ravo" / "tests" / "fixtures" / "legacy_manifest.json"
    ledger_path = arguments.ledger or repository_root / "Ravo" / "tests" / "fixtures" / "fixture_classification_ledger.json"

    try:
        manifest = load_json(manifest_path)
        if arguments.initialize:
            if ledger_path.exists():
                raise LedgerError(f"classification ledger already exists: {ledger_path}")
            ledger_path.write_text(
                json.dumps(build_initial_ledger(manifest), indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            return 0
        validate_ledger(load_json(ledger_path), manifest)
    except LedgerError as error:
        print(f"fixture classification ledger check failed: {error}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
