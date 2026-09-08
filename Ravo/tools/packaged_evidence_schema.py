#!/usr/bin/env python3
"""Versioned packaged-runtime evidence schema shared by producer and validator.

Contract (ravo.packaged_evidence v1):
- Evidence carries type/version, derived status, archive identity, and results.
- status is PASS only when required stages earn it; never a constant.
- FAIL evidence may be structurally valid for upload while the job still fails.
- Meta/evidence pairs must agree on artifact basename, digest, source SHA,
  run id, and attempt. Duplicate JSON object keys are rejected at parse time.
"""

from __future__ import annotations

import json
import re
from typing import Any

EVIDENCE_TYPE = "ravo.packaged_evidence"
EVIDENCE_VERSION = 1

ALLOWED_EVIDENCE_STATUS = frozenset({"PASS", "FAIL"})
ALLOWED_STAGE_STATUS = frozenset({"PASS", "FAIL", "UNTESTED", "NOT_APPLICABLE"})

REQUIRED_SMOKE_STAGES: tuple[str, ...] = (
    "unpack",
    "cli_payload",
    "cli_help",
    "studio_payload",
    "studio_smoke",
    "catalog_create_open",
    "catalog_synthetic_import",
    "catalog_probe_or_render",
    "catalog_reopen_hash",
)

_SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")
_SOURCE_SHA_RE = re.compile(r"^[0-9a-fA-F]{7,40}$")
_RUN_ATTEMPT_RE = re.compile(r"^[1-9][0-9]*$")


class DuplicateKeyError(ValueError):
    """Raised when a JSON object contains a duplicate key."""


def _reject_duplicate_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    seen: dict[str, Any] = {}
    for key, value in pairs:
        if key in seen:
            raise DuplicateKeyError(f"duplicate JSON key: {key}")
        seen[key] = value
    return seen


def load_json_object(text: str) -> dict[str, Any]:
    """Parse a JSON object, rejecting duplicate keys before dict collapse."""
    try:
        payload = json.loads(text, object_pairs_hook=_reject_duplicate_pairs)
    except DuplicateKeyError:
        raise
    except json.JSONDecodeError:
        raise
    if not isinstance(payload, dict):
        raise ValueError("JSON payload must be an object")
    return payload


def normalize_artifact_basename(value: str) -> str:
    """Normalize artifact identity to the archive basename (no directories)."""
    name = value.replace("\\", "/").rstrip("/")
    if "/" in name:
        name = name.rsplit("/", 1)[-1]
    return name


def pair_key_from_filename(path_name: str) -> str | None:
    """Return the artifact basename key for *.meta.json / *.evidence.json names."""
    if path_name.endswith(".meta.json"):
        return path_name[: -len(".meta.json")]
    if path_name.endswith(".evidence.json"):
        return path_name[: -len(".evidence.json")]
    return None


def derive_evidence_status(results: dict[str, str], *, require_smoke: bool) -> str:
    """Derive PASS/FAIL from stage results. Never invent PASS without evidence."""
    if any(status == "FAIL" for status in results.values()):
        return "FAIL"
    if require_smoke:
        for stage in REQUIRED_SMOKE_STAGES:
            if results.get(stage) != "PASS":
                return "FAIL"
    elif not results:
        return "FAIL"
    return "PASS"


def validate_identity_field(
    payload: dict[str, Any], key: str, *, required: bool = True
) -> str | None:
    value = payload.get(key)
    if value is None:
        return f"missing {key}" if required else None
    if not isinstance(value, str) or not value:
        return f"missing/invalid {key}"
    if key == "digest_sha256" and not _SHA256_RE.fullmatch(value):
        return "invalid digest_sha256 (want 64 hex)"
    if key == "source_sha" and not _SOURCE_SHA_RE.fullmatch(value):
        return "invalid source_sha"
    if key == "run_attempt" and not _RUN_ATTEMPT_RE.fullmatch(value):
        return "invalid run_attempt"
    if key == "artifact" and normalize_artifact_basename(value) != value:
        return "artifact must be a basename (no directories)"
    if key in {"artifact", "run_id"} and len(value) < 1:
        return f"missing/invalid {key}"
    return None


def build_evidence_payload(
    *,
    artifact_basename: str,
    digest_sha256: str,
    results: dict[str, str],
    residuals: list[str],
    require_smoke: bool,
    host: dict[str, Any],
    cli: str | None,
    studio: str | None,
    source_sha: str | None = None,
    run_id: str | None = None,
    run_attempt: str | None = None,
    reason: str | None = None,
    status_override: str | None = None,
) -> dict[str, Any]:
    """Build a versioned evidence object. status is derived unless early-error override."""
    if status_override is not None:
        status = status_override
        if status not in ALLOWED_EVIDENCE_STATUS:
            raise ValueError(f"invalid status_override: {status}")
    else:
        status = derive_evidence_status(results, require_smoke=require_smoke)

    payload: dict[str, Any] = {
        "type": EVIDENCE_TYPE,
        "version": EVIDENCE_VERSION,
        "status": status,
        "artifact": normalize_artifact_basename(artifact_basename),
        "digest_sha256": digest_sha256,
        "results": dict(results),
        "residuals": list(residuals),
        "require_smoke": bool(require_smoke),
        "host": host,
        "cli": cli,
        "studio": studio,
    }
    if source_sha is not None:
        payload["source_sha"] = source_sha
    if run_id is not None:
        payload["run_id"] = run_id
    if run_attempt is not None:
        payload["run_attempt"] = run_attempt
    if reason is not None:
        payload["reason"] = reason
    return payload


def validate_meta_payload(payload: dict[str, Any], *, path_label: str) -> list[str]:
    failures: list[str] = []
    for key in ("artifact", "digest_sha256", "source_sha", "run_id", "run_attempt"):
        error = validate_identity_field(payload, key, required=True)
        if error:
            failures.append(f"{path_label}: {error}")
    return failures


def validate_evidence_payload(payload: dict[str, Any], *, path_label: str) -> list[str]:
    """Structural validation for evidence. FAIL reports can be valid here."""
    failures: list[str] = []

    if payload.get("type") != EVIDENCE_TYPE:
        failures.append(f"{path_label}: missing/invalid type (want {EVIDENCE_TYPE})")
    version = payload.get("version")
    if version != EVIDENCE_VERSION and version != str(EVIDENCE_VERSION):
        failures.append(f"{path_label}: missing/invalid version (want {EVIDENCE_VERSION})")

    status = payload.get("status")
    if not isinstance(status, str) or status not in ALLOWED_EVIDENCE_STATUS:
        failures.append(f"{path_label}: missing/invalid status")

    # Identity: required for PASS; when present on FAIL must be well-formed.
    require_identity = status == "PASS"
    for key in ("artifact", "digest_sha256", "source_sha", "run_id", "run_attempt"):
        present = key in payload
        if require_identity or present:
            error = validate_identity_field(payload, key, required=require_identity)
            if error:
                failures.append(f"{path_label}: {error}")

    results = payload.get("results")
    require_smoke = payload.get("require_smoke")
    if status == "PASS":
        if not isinstance(require_smoke, bool):
            failures.append(f"{path_label}: require_smoke must be a boolean for PASS")
        if not isinstance(results, dict) or not results:
            failures.append(f"{path_label}: results must be a non-empty object for PASS")
        else:
            failures.extend(_validate_results_object(results, path_label=path_label))
            if isinstance(require_smoke, bool):
                derived = derive_evidence_status(
                    {k: v for k, v in results.items() if isinstance(k, str) and isinstance(v, str)},
                    require_smoke=require_smoke,
                )
                if derived != "PASS" or status != derived:
                    failures.append(
                        f"{path_label}: status=PASS is not earned by required stage results"
                    )
    else:
        # FAIL may be early-error (reason, optional/empty results) or full results.
        if results is not None:
            if not isinstance(results, dict):
                failures.append(f"{path_label}: results must be an object when present")
            else:
                failures.extend(_validate_results_object(results, path_label=path_label))
                if isinstance(require_smoke, bool) and results:
                    stage_map = {
                        k: v
                        for k, v in results.items()
                        if isinstance(k, str) and isinstance(v, str)
                    }
                    derived = derive_evidence_status(stage_map, require_smoke=require_smoke)
                    if status != derived:
                        failures.append(
                            f"{path_label}: status={status} disagrees with derived {derived}"
                        )
        reason = payload.get("reason")
        if results is None and (not isinstance(reason, str) or not reason):
            # Allow FAIL with empty results only when a reason explains early error.
            if results == {}:
                if not isinstance(reason, str) or not reason:
                    failures.append(f"{path_label}: FAIL without results requires reason")
            elif "results" not in payload:
                if not isinstance(reason, str) or not reason:
                    failures.append(f"{path_label}: early-error FAIL requires reason")

    if "residuals" in payload and not isinstance(payload.get("residuals"), list):
        failures.append(f"{path_label}: residuals must be a list when present")

    return failures


def _validate_results_object(results: dict[str, Any], *, path_label: str) -> list[str]:
    failures: list[str] = []
    names = list(results.keys())
    if len(names) != len(set(names)):
        failures.append(f"{path_label}: results keys must be unique")
    for name, stage_status in results.items():
        if not isinstance(name, str) or not name:
            failures.append(f"{path_label}: invalid results key")
            continue
        if not isinstance(stage_status, str) or stage_status not in ALLOWED_STAGE_STATUS:
            failures.append(f"{path_label}: invalid results[{name!r}] status")
    return failures


def compare_meta_evidence_pair(
    meta: dict[str, Any],
    evidence: dict[str, Any],
    *,
    pair_label: str,
) -> list[str]:
    """Require matching archive identity between meta and evidence."""
    failures: list[str] = []
    for key in ("artifact", "digest_sha256", "source_sha", "run_id", "run_attempt"):
        meta_value = meta.get(key)
        evidence_value = evidence.get(key)
        if evidence_value is None:
            # PASS evidence already required identity; FAIL may omit only if incomplete early error.
            if evidence.get("status") == "PASS":
                failures.append(f"{pair_label}: evidence missing {key} for pairing")
            continue
        if meta_value != evidence_value:
            if key == "artifact":
                meta_norm = (
                    normalize_artifact_basename(meta_value)
                    if isinstance(meta_value, str)
                    else meta_value
                )
                evidence_norm = (
                    normalize_artifact_basename(evidence_value)
                    if isinstance(evidence_value, str)
                    else evidence_value
                )
                if meta_norm == evidence_norm:
                    continue
            failures.append(
                f"{pair_label}: meta/evidence {key} mismatch "
                f"({meta_value!r} vs {evidence_value!r})"
            )
    return failures
