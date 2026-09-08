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


def _require_string(payload: dict[str, object], key: str, *, min_len: int = 1) -> str | None:
    value = payload.get(key)
    if not isinstance(value, str) or len(value) < min_len:
        return f"missing/invalid {key}"
    return None


def validate_json_files(paths: list[Path]) -> None:
    failures: list[str] = []
    seen_digests: dict[str, Path] = {}
    for path in paths:
        if not path.is_file():
            failures.append(f"missing: {path}")
            continue
        try:
            payload = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as exc:
            failures.append(f"{path}: {exc}")
            continue
        if not isinstance(payload, dict):
            failures.append(f"{path}: evidence payload must be an object")
            continue
        name = path.name
        if name.endswith(".meta.json"):
            for key, minimum in (
                ("artifact", 1),
                ("digest_sha256", 64),
                ("source_sha", 7),
                ("run_id", 1),
                ("run_attempt", 1),
            ):
                error = _require_string(payload, key, min_len=minimum)
                if error:
                    failures.append(f"{path}: {error}")
            digest = payload.get("digest_sha256")
            if isinstance(digest, str):
                previous = seen_digests.get(digest)
                if previous is not None and previous != path:
                    failures.append(
                        f"{path}: digest_sha256 duplicates {previous.name}"
                    )
                seen_digests[digest] = path
        elif name.endswith(".evidence.json"):
            error = _require_string(payload, "status")
            if error:
                failures.append(f"{path}: {error}")
            # When checker evidence includes identity fields, keep them consistent.
            for key in ("artifact", "digest_sha256", "source_sha", "run_id", "run_attempt"):
                if key in payload:
                    error = _require_string(payload, key)
                    if error:
                        failures.append(f"{path}: {error}")
            stages = payload.get("stages")
            if stages is not None:
                if not isinstance(stages, dict) or not stages:
                    failures.append(f"{path}: stages must be a non-empty object when present")
                else:
                    names = list(stages.keys())
                    if len(names) != len(set(names)):
                        failures.append(f"{path}: stages keys must be unique")
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
