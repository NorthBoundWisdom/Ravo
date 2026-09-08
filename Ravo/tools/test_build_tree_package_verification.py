#!/usr/bin/env python3
"""Build-tree CLI evidence for package verification orchestration.

This is intentionally labeled build-tree evidence, not packaged PASS.
"""

from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_packaged_runtime as cpr  # noqa: E402


def _resolve_cli() -> Path | None:
    override = os.environ.get("RAVO_CLI_EXECUTABLE")
    if override:
        path = Path(override)
        return path if path.is_file() else None
    candidates = [
        ROOT / "build/mac_clang_debug/Ravo/cli/ravo",
        ROOT / "build/linux_clang_debug/Ravo/cli/ravo",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


class BuildTreePackageVerificationTests(unittest.TestCase):
    def test_create_import_probe_reopen_with_artifact_contract(self) -> None:
        cli = _resolve_cli()
        if cli is None:
            self.skipTest("UNTESTED: build-tree ravo CLI not present")
        with tempfile.TemporaryDirectory(prefix="ravo-build-tree-evidence-") as tmp:
            work = Path(tmp)
            env = cpr.cleaned_env(home=work / "home", allow_offscreen=True)
            results: dict[str, str] = {}

            def record(name: str, status: cpr.Status, detail: str = "") -> None:
                results[name] = status.value
                if status == cpr.Status.FAIL:
                    self.fail(f"{name}: {detail}")

            cpr.run_catalog_workflow_stages(cli, env, work, record)
            for stage in (
                "catalog_create_open",
                "catalog_synthetic_import",
                "catalog_probe_or_render",
                "catalog_reopen_hash",
            ):
                self.assertEqual(results.get(stage), "PASS", results)
            # Explicitly label this matrix as build-tree, not packaged.
            self.assertTrue(str(cli).endswith("/ravo") or str(cli).endswith("\\ravo.exe"))


if __name__ == "__main__":
    unittest.main()
