#!/usr/bin/env python3
"""Unit tests for check_packaged_runtime fail-closed behavior (no Qt required)."""

from __future__ import annotations

import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
CHECKER = ROOT / "tools" / "check_packaged_runtime.py"

# Import module helpers for focused extract/env cases.
sys.path.insert(0, str(ROOT / "tools"))
import check_packaged_runtime as cpr  # noqa: E402


def _write_fake_cli(path: Path, exit_code: int = 0) -> None:
    path.write_text(
        "#!/bin/sh\n"
        f"exit {exit_code}\n",
        encoding="utf-8",
    )
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


class CheckPackagedRuntimeTests(unittest.TestCase):
    def test_require_smoke_fails_when_studio_missing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            payload = tmp_path / "payload"
            payload.mkdir()
            _write_fake_cli(payload / "ravo")
            artifact = tmp_path / "ravo.zip"
            with zipfile.ZipFile(artifact, "w") as zf:
                zf.write(payload / "ravo", arcname="ravo")
            work = tmp_path / "work"
            proc = subprocess.run(
                [sys.executable, str(CHECKER), str(artifact), "--workdir", str(work), "--require-smoke"],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertNotEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertIn("studio", (proc.stdout + proc.stderr).lower())

    def test_polluted_workdir_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            payload = tmp_path / "payload"
            payload.mkdir()
            _write_fake_cli(payload / "ravo")
            (payload / "ravo_studio").write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            (payload / "ravo_studio").chmod(0o755)
            artifact = tmp_path / "ravo.zip"
            with zipfile.ZipFile(artifact, "w") as zf:
                zf.write(payload / "ravo", arcname="ravo")
                zf.write(payload / "ravo_studio", arcname="ravo_studio")
            work = tmp_path / "work"
            work.mkdir()
            (work / "pollution.txt").write_text("x", encoding="utf-8")
            proc = subprocess.run(
                [sys.executable, str(CHECKER), str(artifact), "--workdir", str(work)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertNotEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertIn("not empty", proc.stderr.lower())

    def test_dual_cli_candidates_fail(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            payload = tmp_path / "payload"
            (payload / "a").mkdir(parents=True)
            (payload / "b").mkdir(parents=True)
            _write_fake_cli(payload / "a" / "ravo")
            _write_fake_cli(payload / "b" / "ravo")
            artifact = tmp_path / "ravo.zip"
            with zipfile.ZipFile(artifact, "w") as zf:
                zf.write(payload / "a" / "ravo", arcname="a/ravo")
                zf.write(payload / "b" / "ravo", arcname="b/ravo")
            work = tmp_path / "work"
            proc = subprocess.run(
                [sys.executable, str(CHECKER), str(artifact), "--workdir", str(work), "--require-smoke"],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertNotEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def test_appimage_extract_nonzero_exit_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            artifact = tmp_path / "Ravo-x.AppImage"
            artifact.write_text("#!/bin/sh\necho extract-failed >&2\nexit 7\n", encoding="utf-8")
            artifact.chmod(0o755)
            dest = tmp_path / "out"
            dest.mkdir()
            with self.assertRaises(RuntimeError) as raised:
                cpr.unpack(artifact, dest)
            self.assertIn("extract", str(raised.exception).lower())

    def test_appimage_extract_missing_apprun_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            artifact = tmp_path / "Ravo-y.AppImage"
            # Succeeds but writes nothing resembling AppDir/AppRun.
            artifact.write_text("#!/bin/sh\nmkdir -p empty-dir\nexit 0\n", encoding="utf-8")
            artifact.chmod(0o755)
            dest = tmp_path / "out"
            dest.mkdir()
            with self.assertRaises(RuntimeError) as raised:
                cpr.unpack(artifact, dest)
            self.assertIn("apprun", str(raised.exception).lower())

    def test_cleaned_env_strips_dev_qt_and_library_paths(self) -> None:
        home = Path(tempfile.mkdtemp())
        try:
            os.environ["LD_LIBRARY_PATH"] = "/opt/Qt/6.8/lib"
            os.environ["DYLD_LIBRARY_PATH"] = "/Users/dev/Qt/6.8/macos/lib"
            os.environ["PATH"] = "/opt/Qt/6.8/bin" + os.pathsep + "/usr/bin"
            os.environ["QT_PLUGIN_PATH"] = "/opt/Qt/plugins"
            env = cpr.cleaned_env(home=home, allow_offscreen=True)
            self.assertNotIn("LD_LIBRARY_PATH", env)
            self.assertNotIn("DYLD_LIBRARY_PATH", env)
            self.assertNotIn("QT_PLUGIN_PATH", env)
            self.assertNotIn("/opt/Qt/6.8/bin", env.get("PATH", ""))
            self.assertIn("/usr/bin", env.get("PATH", ""))
            self.assertEqual(env.get("HOME"), str(home))
            self.assertEqual(env.get("QT_QPA_PLATFORM"), "offscreen")
        finally:
            for key in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH", "QT_PLUGIN_PATH"):
                os.environ.pop(key, None)



if __name__ == "__main__":
    unittest.main()
