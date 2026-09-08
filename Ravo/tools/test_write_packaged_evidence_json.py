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
            reason = 'checker exited without evidence'
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
                    "status=\"FAIL\"",
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

    def test_validate_accepts_written_files(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "ok.json"
            proc = self._run(
                ["--output", str(out), "--field", "artifact=a.dmg", "--field", "status=PASS"]
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            proc = self._run(["--validate", str(out)])
            self.assertEqual(proc.returncode, 0, proc.stderr)


if __name__ == "__main__":
    unittest.main()
