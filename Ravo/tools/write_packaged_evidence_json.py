#!/usr/bin/env python3
"""Serialize packaged-runtime meta/evidence JSON without shell printf quoting bugs.

Fields are passed as argv --field key=value pairs (or --field-json key=<json>) so
values with spaces, quotes, and Unicode never enter Python/JSON source text.
Round-trips through json.loads before writing succeeds.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def parse_fields(raw_fields: list[str], raw_json_fields: list[str]) -> dict[str, object]:
    payload: dict[str, object] = {}
    for item in raw_fields:
        if "=" not in item:
            raise SystemExit(f"invalid --field (expected key=value): {item!r}")
        key, value = item.split("=", 1)
        if not key:
            raise SystemExit(f"invalid --field empty key: {item!r}")
        payload[key] = value
    for item in raw_json_fields:
        if "=" not in item:
            raise SystemExit(f"invalid --field-json (expected key=<json>): {item!r}")
        key, value = item.split("=", 1)
        if not key:
            raise SystemExit(f"invalid --field-json empty key: {item!r}")
        try:
            payload[key] = json.loads(value)
        except json.JSONDecodeError as exc:
            raise SystemExit(f"invalid --field-json for {key}: {exc}") from exc
    return payload


def write_json(path: Path, payload: dict[str, object]) -> None:
    text = json.dumps(payload, ensure_ascii=False, separators=(",", ":"), sort_keys=True)
    # Validate before write so a serializer bug cannot upload non-JSON.
    round_trip = json.loads(text)
    if round_trip != payload:
        raise SystemExit("json round-trip mismatch before write")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text + "\n", encoding="utf-8")
    loaded = json.loads(path.read_text(encoding="utf-8"))
    if loaded != payload:
        raise SystemExit(f"written JSON failed validation: {path}")


def validate_json_files(paths: list[Path]) -> None:
    from packaged_evidence_schema import (
        compare_meta_evidence_pair,
        load_json_object,
        pair_key_from_filename,
        validate_evidence_payload,
        validate_meta_payload,
    )

    failures: list[str] = []
    seen_digests: dict[str, Path] = {}
    metas: dict[str, tuple[Path, dict]] = {}
    evidences: dict[str, tuple[Path, dict]] = {}

    for path in paths:
        if not path.is_file():
            failures.append(f"missing: {path}")
            continue
        try:
            payload = load_json_object(path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as exc:
            failures.append(f"{path}: {exc}")
            continue
        name = path.name
        label = str(path)
        if name.endswith(".meta.json"):
            failures.extend(validate_meta_payload(payload, path_label=label))
            digest = payload.get("digest_sha256")
            if isinstance(digest, str):
                previous = seen_digests.get(digest)
                if previous is not None and previous != path:
                    failures.append(f"{path}: digest_sha256 duplicates {previous.name}")
                seen_digests[digest] = path
            pair_key = pair_key_from_filename(name)
            if pair_key is not None:
                metas[pair_key] = (path, payload)
        elif name.endswith(".evidence.json"):
            failures.extend(validate_evidence_payload(payload, path_label=label))
            pair_key = pair_key_from_filename(name)
            if pair_key is not None:
                evidences[pair_key] = (path, payload)
        else:
            failures.append(f"{path}: unrecognized evidence filename (want *.meta.json or *.evidence.json)")

    # Pair when both sides appear in the same validate batch (CI). Meta-only
    # batches remain valid for serializer unit checks. PASS evidence alone fails.
    if metas and evidences:
        for key, (meta_path, meta_payload) in metas.items():
            paired = evidences.get(key)
            if paired is None:
                failures.append(f"{meta_path}: missing paired evidence for artifact key {key!r}")
                continue
            evidence_path, evidence_payload = paired
            failures.extend(
                compare_meta_evidence_pair(
                    meta_payload,
                    evidence_payload,
                    pair_label=f"{meta_path.name}+{evidence_path.name}",
                )
            )
        for key, (evidence_path, evidence_payload) in evidences.items():
            if key in metas:
                continue
            if evidence_payload.get("status") == "PASS":
                failures.append(
                    f"{evidence_path}: PASS evidence missing paired meta for {key!r}"
                )
    else:
        for key, (evidence_path, evidence_payload) in evidences.items():
            if evidence_payload.get("status") == "PASS":
                failures.append(
                    f"{evidence_path}: PASS evidence missing paired meta for {key!r}"
                )

    if failures:
        raise SystemExit("JSON validation failed:\n" + "\n".join(failures))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="Write one JSON object to this path")
    parser.add_argument(
        "--field",
        action="append",
        default=[],
        help="String field as key=value (repeatable)",
    )
    parser.add_argument(
        "--field-json",
        action="append",
        default=[],
        help="JSON-typed field as key=<json> (repeatable)",
    )
    parser.add_argument(
        "--validate",
        nargs="+",
        type=Path,
        help="Validate existing JSON files with json.loads and exit",
    )
    args = parser.parse_args(argv)

    if args.validate:
        validate_json_files(list(args.validate))
        return 0
    if args.output is None:
        parser.error("--output is required unless --validate is used")
    payload = parse_fields(args.field, args.field_json)
    write_json(args.output, payload)
    return 0


if __name__ == "__main__":
    sys.exit(main())
