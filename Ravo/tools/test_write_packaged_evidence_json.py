#!/usr/bin/env python3
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = Path(__file__).resolve().parent / "write_packaged_evidence_json.py"
CHECKER = Path(__file__).resolve().parent / "check_packaged_runtime.py"
SCHEMA = Path(__file__).resolve().parent / "packaged_evidence_schema.py"

sys.path.insert(0, str(Path(__file__).resolve().parent))
import packaged_evidence_schema as schema  # noqa: E402


DIGEST_A = "ab" * 32
DIGEST_B = "cd" * 32
SOURCE = "b283462d726f9cd075080d6a3879ed904f925c88"


class WritePackagedEvidenceJsonTests(unittest.TestCase):
    def _run(self, args: list[str]) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(TOOL), *args],
            cwd=str(ROOT),
            capture_output=True,
            text=True,
            check=False,
        )

    def test_writes_valid_json_with_spaces_unicode_and_quotes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "meta.json"
            artifact = 'Ravo "Unicode 测试" Package.dmg'
            reason = "checker exited without evidence"
            proc = self._run(
                [
                    "--output",
                    str(out),
                    "--field",
                    f"artifact={artifact}",
                    "--field",
                    "digest_sha256=0123456789abcdef",
                    "--field",
                    "source_sha=fb670f72",
                    "--field",
                    "run_id=34182841716",
                    "--field",
                    "run_attempt=1",
                    "--field",
                    f"reason={reason}",
                    "--field-json",
                    'status="FAIL"',
                ]
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            payload = json.loads(out.read_text(encoding="utf-8"))
            self.assertEqual(payload["artifact"], artifact)
            self.assertEqual(payload["reason"], reason)
            self.assertEqual(payload["status"], "FAIL")
            self.assertEqual(payload["run_id"], "34182841716")

    def test_validate_rejects_non_json(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "bad.meta.json"
            bad.write_text("{artifact:broken}\n", encoding="utf-8")
            proc = self._run(["--validate", str(bad)])
            self.assertNotEqual(proc.returncode, 0)

    def test_validate_accepts_written_meta_files(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "ok.meta.json"
            proc = self._run(
                [
                    "--output",
                    str(out),
                    "--field",
                    "artifact=a.dmg",
                    "--field",
                    f"digest_sha256={DIGEST_A}",
                    "--field",
                    "source_sha=fb670f72",
                    "--field",
                    "run_id=1",
                    "--field",
                    "run_attempt=1",
                ]
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            proc = self._run(["--validate", str(out)])
            self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_validate_rejects_meta_missing_source_sha(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "bad.meta.json"
            proc = self._run(
                [
                    "--output",
                    str(out),
                    "--field",
                    "artifact=a.dmg",
                    "--field",
                    f"digest_sha256={DIGEST_B}",
                    "--field",
                    "run_id=1",
                    "--field",
                    "run_attempt=1",
                ]
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            proc = self._run(["--validate", str(out)])
            self.assertNotEqual(proc.returncode, 0)


class PackagedEvidenceSchemaContractTests(unittest.TestCase):
    def _write(self, path: Path, payload: dict) -> None:
        path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    def _validate(self, *paths: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(TOOL), "--validate", *[str(p) for p in paths]],
            cwd=str(ROOT),
            capture_output=True,
            text=True,
            check=False,
        )

    def _pass_results(self) -> dict[str, str]:
        return {stage: "PASS" for stage in schema.REQUIRED_SMOKE_STAGES}

    def _meta(self, artifact: str = "pkg.zip", digest: str = DIGEST_A) -> dict:
        return {
            "artifact": artifact,
            "digest_sha256": digest,
            "source_sha": SOURCE,
            "run_id": "34217858766",
            "run_attempt": "1",
        }

    def _evidence(
        self,
        *,
        status: str,
        artifact: str = "pkg.zip",
        digest: str = DIGEST_A,
        results: dict[str, str] | None = None,
        require_smoke: bool = True,
        reason: str | None = None,
    ) -> dict:
        payload = {
            "type": schema.EVIDENCE_TYPE,
            "version": schema.EVIDENCE_VERSION,
            "status": status,
            "artifact": artifact,
            "digest_sha256": digest,
            "source_sha": SOURCE,
            "run_id": "34217858766",
            "run_attempt": "1",
            "require_smoke": require_smoke,
            "residuals": [],
            "host": {"platform": "test", "python": "3"},
            "cli": None,
            "studio": None,
        }
        if results is not None:
            payload["results"] = results
        if reason is not None:
            payload["reason"] = reason
        return payload

    def test_paired_pass_producer_shape_validates(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            meta = root / "pkg.zip.meta.json"
            evidence = root / "pkg.zip.evidence.json"
            self._write(meta, self._meta())
            self._write(
                evidence,
                self._evidence(status="PASS", results=self._pass_results()),
            )
            proc = self._validate(meta, evidence)
            self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_constant_status_pass_without_required_stages_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            evidence = root / "pkg.zip.evidence.json"
            self._write(
                evidence,
                {
                    "type": schema.EVIDENCE_TYPE,
                    "version": 1,
                    "status": "PASS",
                    "artifact": "pkg.zip",
                    "digest_sha256": DIGEST_A,
                    "source_sha": SOURCE,
                    "run_id": "1",
                    "run_attempt": "1",
                    "require_smoke": True,
                    "results": {"unpack": "PASS"},
                },
            )
            proc = self._validate(evidence)
            self.assertNotEqual(proc.returncode, 0)
            self.assertIn("not earned", proc.stderr.lower() + proc.stdout.lower())

    def test_legacy_producer_shape_without_type_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            evidence = Path(tmp) / "pkg.zip.evidence.json"
            self._write(
                evidence,
                {
                    "artifact": str(Path(tmp) / "pkg.zip"),
                    "sha256": DIGEST_A,
                    "results": self._pass_results(),
                    "residuals": [],
                    "require_smoke": True,
                    "host": {},
                    "cli": None,
                    "studio": None,
                },
            )
            proc = self._validate(evidence)
            self.assertNotEqual(proc.returncode, 0)
            combined = (proc.stderr + proc.stdout).lower()
            self.assertTrue("type" in combined or "status" in combined)

    def test_fail_evidence_structurally_valid(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            meta = root / "pkg.zip.meta.json"
            evidence = root / "pkg.zip.evidence.json"
            results = self._pass_results()
            results["studio_smoke"] = "FAIL"
            self._write(meta, self._meta())
            self._write(evidence, self._evidence(status="FAIL", results=results))
            proc = self._validate(meta, evidence)
            self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_identity_mismatch_between_meta_and_evidence_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            meta = root / "pkg.zip.meta.json"
            evidence = root / "pkg.zip.evidence.json"
            self._write(meta, self._meta(digest=DIGEST_A))
            self._write(
                evidence,
                self._evidence(
                    status="PASS",
                    digest=DIGEST_B,
                    results=self._pass_results(),
                ),
            )
            proc = self._validate(meta, evidence)
            self.assertNotEqual(proc.returncode, 0)
            self.assertIn("mismatch", (proc.stderr + proc.stdout).lower())

    def test_duplicate_json_keys_rejected_at_parse(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            evidence = Path(tmp) / "pkg.zip.evidence.json"
            # Duplicate "status" key — dict collapse cannot see this; parse must.
            evidence.write_text(
                '{"type":"ravo.packaged_evidence","version":1,'
                '"status":"FAIL","status":"PASS","reason":"x"}\n',
                encoding="utf-8",
            )
            proc = self._validate(evidence)
            self.assertNotEqual(proc.returncode, 0)
            self.assertIn("duplicate", (proc.stderr + proc.stdout).lower())

    def test_early_error_fail_without_identity_validates(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            evidence = Path(tmp) / "missing-artifacts.evidence.json"
            self._write(
                evidence,
                {
                    "type": schema.EVIDENCE_TYPE,
                    "version": 1,
                    "status": "FAIL",
                    "reason": "no artifacts matched",
                    "run_id": "1",
                    "run_attempt": "1",
                },
            )
            proc = self._validate(evidence)
            self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_load_json_object_detects_nested_duplicate_results_keys(self) -> None:
        text = '{"results":{"unpack":"PASS","unpack":"FAIL"}}'
        with self.assertRaises(schema.DuplicateKeyError):
            schema.load_json_object(text)


class ProducerPathEvidenceTests(unittest.TestCase):
    """Evidence must come from the real checker finalize path, not hand-shaped only."""

    def test_checker_evidence_validates_with_matching_meta(self) -> None:
        import stat
        import zipfile

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            payload = tmp_path / "payload"
            payload.mkdir()
            cli = payload / "ravo"
            cli.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            cli.chmod(cli.stat().st_mode | stat.S_IXUSR)
            artifact = tmp_path / "one.zip"
            with zipfile.ZipFile(artifact, "w") as zf:
                zf.write(cli, arcname="ravo")
            digest = __import__("hashlib").sha256(artifact.read_bytes()).hexdigest()
            evidence = tmp_path / "one.zip.evidence.json"
            meta = tmp_path / "one.zip.meta.json"
            meta.write_text(
                json.dumps(
                    {
                        "artifact": "one.zip",
                        "digest_sha256": digest,
                        "source_sha": SOURCE,
                        "run_id": "99",
                        "run_attempt": "1",
                    },
                    indent=2,
                )
                + "\n",
                encoding="utf-8",
            )
            proc = subprocess.run(
                [
                    sys.executable,
                    str(CHECKER),
                    str(artifact),
                    "--workdir",
                    str(tmp_path / "work"),
                    "--require-smoke",
                    "--evidence-json",
                    str(evidence),
                    "--artifact-basename",
                    "one.zip",
                    "--source-sha",
                    SOURCE,
                    "--run-id",
                    "99",
                    "--run-attempt",
                    "1",
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertNotEqual(proc.returncode, 0, "studio missing must fail job")
            self.assertTrue(evidence.is_file(), proc.stdout + proc.stderr)
            body = json.loads(evidence.read_text(encoding="utf-8"))
            self.assertEqual(body["type"], schema.EVIDENCE_TYPE)
            self.assertEqual(body["version"], schema.EVIDENCE_VERSION)
            self.assertEqual(body["status"], "FAIL")
            self.assertEqual(body["artifact"], "one.zip")
            self.assertEqual(body["digest_sha256"], digest)
            self.assertIn("results", body)
            self.assertNotIn("sha256", body)
            validate = subprocess.run(
                [sys.executable, str(TOOL), "--validate", str(meta), str(evidence)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(validate.returncode, 0, validate.stderr)

    def test_missing_artifact_writes_early_error_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            missing = tmp_path / "nope.zip"
            evidence = tmp_path / "nope.zip.evidence.json"
            proc = subprocess.run(
                [
                    sys.executable,
                    str(CHECKER),
                    str(missing),
                    "--evidence-json",
                    str(evidence),
                    "--artifact-basename",
                    "nope.zip",
                    "--source-sha",
                    SOURCE,
                    "--run-id",
                    "1",
                    "--run-attempt",
                    "1",
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertNotEqual(proc.returncode, 0)
            self.assertTrue(evidence.is_file())
            body = json.loads(evidence.read_text(encoding="utf-8"))
            self.assertEqual(body["status"], "FAIL")
            self.assertIn("reason", body)
            validate = subprocess.run(
                [sys.executable, str(TOOL), "--validate", str(evidence)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(validate.returncode, 0, validate.stderr)


if __name__ == "__main__":
    unittest.main()
