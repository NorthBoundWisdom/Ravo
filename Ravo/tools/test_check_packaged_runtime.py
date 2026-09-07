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

    def test_catalog_stages_are_untested_not_pass_without_real_cli(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            payload = tmp_path / "payload"
            payload.mkdir()
            _write_fake_cli(payload / "ravo")
            studio = payload / "ravo_studio"
            studio.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            studio.chmod(0o755)
            artifact = tmp_path / "ravo.zip"
            with zipfile.ZipFile(artifact, "w") as zf:
                zf.write(payload / "ravo", arcname="ravo")
                zf.write(studio, arcname="ravo_studio")
            work = tmp_path / "work"
            evidence = tmp_path / "evidence.json"
            # Without --require-smoke, structural path can complete with UNTESTED catalog stages.
            proc = subprocess.run(
                [
                    sys.executable,
                    str(CHECKER),
                    str(artifact),
                    "--workdir",
                    str(work),
                    "--evidence-json",
                    str(evidence),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            combined = proc.stdout + proc.stderr
            self.assertIn("UNTESTED: catalog_synthetic_import", combined)
            self.assertIn("UNTESTED: native_display_session", combined)
            self.assertTrue(evidence.is_file())
            payload_json = evidence.read_text(encoding="utf-8")
            self.assertIn("catalog_create_open", payload_json)
            # Must not claim PASS for synthetic import without real CLI surface.
            self.assertNotIn('"catalog_synthetic_import": "PASS"', payload_json)


class PackagedIdentityResolutionTests(unittest.TestCase):
    """CLI / Studio payload identity must stay role-separated (F1)."""

    def _touch(self, path: Path) -> Path:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("placeholder\n", encoding="utf-8")
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
        return path

    def test_macos_app_dual_binaries_resolve_by_role(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            macos = root / "Ravo Studio.app" / "Contents" / "MacOS"
            cli = self._touch(macos / "ravo")
            studio = self._touch(macos / "ravo_studio")
            found_cli, cli_hits = cpr.find_cli(root)
            found_studio, studio_hits = cpr.find_studio(root)
            self.assertEqual(found_cli, cli)
            self.assertEqual(cli_hits, [cli])
            self.assertEqual(found_studio, studio)
            self.assertEqual(studio_hits, [studio])

    def test_cli_only_layout(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cli = self._touch(root / "payload" / "ravo")
            found_cli, cli_hits = cpr.find_cli(root)
            found_studio, studio_hits = cpr.find_studio(root)
            self.assertEqual(found_cli, cli)
            self.assertEqual(cli_hits, [cli])
            self.assertIsNone(found_studio)
            self.assertEqual(studio_hits, [])

    def test_studio_only_never_accepted_as_cli(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            macos = root / "Ravo Studio.app" / "Contents" / "MacOS"
            studio = self._touch(macos / "ravo_studio")
            found_cli, cli_hits = cpr.find_cli(root)
            found_studio, studio_hits = cpr.find_studio(root)
            self.assertIsNone(found_cli)
            self.assertEqual(cli_hits, [])
            self.assertNotIn(studio, cli_hits)
            self.assertEqual(found_studio, studio)
            self.assertEqual(studio_hits, [studio])

    def test_dual_cli_candidates_ambiguous(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            a = self._touch(root / "a" / "ravo")
            b = self._touch(root / "b" / "ravo")
            found_cli, cli_hits = cpr.find_cli(root)
            self.assertIsNone(found_cli)
            self.assertEqual(set(cli_hits), {a, b})

    def test_dual_studio_candidates_ambiguous(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            a = self._touch(root / "a" / "ravo_studio")
            b = self._touch(root / "b" / "ravo_studio")
            found_studio, studio_hits = cpr.find_studio(root)
            self.assertIsNone(found_studio)
            self.assertEqual(set(studio_hits), {a, b})

    def test_no_binaries(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "empty").mkdir()
            found_cli, cli_hits = cpr.find_cli(root)
            found_studio, studio_hits = cpr.find_studio(root)
            self.assertIsNone(found_cli)
            self.assertEqual(cli_hits, [])
            self.assertIsNone(found_studio)
            self.assertEqual(studio_hits, [])

    def test_deb_launcher_layout_prefers_opt_payload(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            payload_cli = self._touch(root / "opt" / "RavoStudio" / "bin" / "ravo")
            payload_studio = self._touch(root / "opt" / "RavoStudio" / "bin" / "ravo_studio")
            launcher_cli = root / "usr" / "bin" / "ravo"
            launcher_cli.parent.mkdir(parents=True, exist_ok=True)
            launcher_cli.write_text(
                "#!/bin/sh\n"
                "PREFIX=/opt/RavoStudio\n"
                'exec "${PREFIX}/bin/ravo" "$@"\n',
                encoding="utf-8",
            )
            launcher_cli.chmod(0o755)
            launcher_studio = root / "usr" / "bin" / "ravo_studio"
            launcher_studio.write_text(
                "#!/bin/sh\n"
                "PREFIX=/opt/RavoStudio\n"
                'exec "${PREFIX}/bin/ravo_studio" "$@"\n',
                encoding="utf-8",
            )
            launcher_studio.chmod(0o755)
            found_cli, cli_hits = cpr.find_cli(root)
            found_studio, studio_hits = cpr.find_studio(root)
            self.assertEqual(found_cli, payload_cli)
            self.assertEqual(cli_hits, [payload_cli])
            self.assertNotIn(launcher_cli, cli_hits)
            self.assertEqual(found_studio, payload_studio)
            self.assertEqual(studio_hits, [payload_studio])
            self.assertNotIn(launcher_studio, studio_hits)

    def test_external_symlink_rejected_as_payload(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            outside = tmp_path / "outside"
            outside.mkdir()
            external = self._touch(outside / "ravo")
            root = tmp_path / "package"
            root.mkdir()
            link = root / "ravo"
            try:
                link.symlink_to(external)
            except OSError as exc:  # pragma: no cover - platform without symlink
                self.skipTest(f"symlink unavailable: {exc}")
            found_cli, cli_hits = cpr.find_cli(root)
            self.assertIsNone(found_cli)
            self.assertEqual(cli_hits, [])




if __name__ == "__main__":
    unittest.main()
