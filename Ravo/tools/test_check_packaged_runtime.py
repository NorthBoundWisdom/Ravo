#!/usr/bin/env python3
"""Unit tests for check_packaged_runtime fail-closed behavior (no Qt required)."""

from __future__ import annotations

import json
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
            artifact.write_text("#!/bin/sh\nmkdir -p squashfs-root\nexit 0\n", encoding="utf-8")
            artifact.chmod(0o755)
            dest = tmp_path / "out"
            dest.mkdir()
            with self.assertRaises(RuntimeError) as raised:
                cpr.unpack(artifact, dest)
            self.assertTrue("apprun" in str(raised.exception).lower() or "squashfs-root" in str(raised.exception).lower())

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
            self.assertTrue(evidence.is_file(), proc.stdout + proc.stderr)
            payload_json = evidence.read_text(encoding="utf-8")
            self.assertIn("catalog_create_open", payload_json)
            self.assertNotIn('"catalog_create_open": "PASS"', payload_json)
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




class CatalogWorkflowContractTests(unittest.TestCase):
    def test_write_minimal_png_is_valid_signature(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "t.png"
            cpr.write_minimal_png(path)
            self.assertTrue(path.is_file())
            self.assertEqual(path.read_bytes()[:8], b"\x89PNG\r\n\x1a\n")
            self.assertGreater(path.stat().st_size, 32)

    def test_fake_cli_exit0_without_db_fails_create(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            records: list[tuple[str, cpr.Status, str]] = []

            def record(name: str, status: cpr.Status, detail: str = "") -> None:
                records.append((name, status, detail))

            cli = tmp_path / "ravo"
            cli.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            cli.chmod(0o755)
            env = cpr.cleaned_env(home=tmp_path / "home", allow_offscreen=True)
            cpr.run_catalog_workflow_stages(cli, env, tmp_path / "work", record)
            by_name = {name: status for name, status, _ in records}
            self.assertEqual(by_name.get("catalog_create_open"), cpr.Status.FAIL)
            self.assertNotEqual(by_name.get("catalog_synthetic_import"), cpr.Status.PASS)



class PackagedRuntimeIsolationTests(unittest.TestCase):
    def test_cleaned_env_uses_minimal_path_only(self) -> None:
        home = Path(tempfile.mkdtemp())
        try:
            os.environ["PATH"] = "/opt/Qt/6.8/bin" + os.pathsep + "/usr/local/bin" + os.pathsep + "/usr/bin"
            os.environ["DYLD_FALLBACK_LIBRARY_PATH"] = "/opt/Qt/lib"
            env = cpr.cleaned_env(home=home, allow_offscreen=False)
            path_parts = env.get("PATH", "").split(os.pathsep)
            self.assertEqual(path_parts, ["/usr/bin", "/bin", "/usr/sbin", "/sbin"])
            self.assertNotIn("DYLD_FALLBACK_LIBRARY_PATH", env)
            self.assertNotIn("QT_QPA_PLATFORM", env)
        finally:
            os.environ.pop("DYLD_FALLBACK_LIBRARY_PATH", None)

    def test_appimage_requires_apprun_at_extract_root(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            artifact = tmp_path / "Ravo-z.AppImage"
            artifact.write_text(
                "#!/bin/sh\nmkdir -p squashfs-root/nested\n"
                "printf 'x' > squashfs-root/nested/AppRun\nexit 0\n",
                encoding="utf-8",
            )
            artifact.chmod(0o755)
            dest = tmp_path / "out"
            dest.mkdir()
            with self.assertRaises(RuntimeError) as raised:
                cpr.unpack(artifact, dest)
            msg = str(raised.exception).lower()
            self.assertTrue("apprun" in msg or "squashfs-root" in msg, msg)

    def test_evidence_json_written_on_failure(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            payload = tmp_path / "payload"
            payload.mkdir()
            _write_fake_cli(payload / "ravo")
            artifact = tmp_path / "ravo.zip"
            with zipfile.ZipFile(artifact, "w") as zf:
                zf.write(payload / "ravo", arcname="ravo")
            work = tmp_path / "work"
            evidence = tmp_path / "evidence.json"
            proc = subprocess.run(
                [
                    sys.executable,
                    str(CHECKER),
                    str(artifact),
                    "--workdir",
                    str(work),
                    "--require-smoke",
                    "--evidence-json",
                    str(evidence),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertNotEqual(proc.returncode, 0)
            self.assertTrue(evidence.is_file())
            body = evidence.read_text(encoding="utf-8")
            self.assertIn('"require_smoke": true', body)
            self.assertIn("studio_payload", body)



class ParseCliSuccessEnvelopeTests(unittest.TestCase):
    def _proc(self, stdout: str, returncode: int = 0):
        return subprocess.CompletedProcess(args=["ravo"], returncode=returncode, stdout=stdout, stderr="")

    def test_accepts_versioned_success_envelope(self) -> None:
        payload = {
            "type": "ravo.cli.result",
            "version": 1,
            "ok": True,
            "data": {"catalog_id": "cat_1"},
            "diagnostics": [],
        }
        data = cpr.parse_cli_success_envelope(self._proc(json.dumps(payload)))
        self.assertEqual(data, {"catalog_id": "cat_1"})

    def test_rejects_bare_data_object(self) -> None:
        self.assertIsNone(cpr.parse_cli_success_envelope(self._proc(json.dumps({"catalog_id": "x"}))))

    def test_rejects_ok_false_even_with_exit_zero(self) -> None:
        payload = {"type": "ravo.cli.result", "version": 1, "ok": False, "error": {}, "diagnostics": []}
        self.assertIsNone(cpr.parse_cli_success_envelope(self._proc(json.dumps(payload), 0)))

    def test_rejects_wrong_version_and_type(self) -> None:
        bad_version = {"type": "ravo.cli.result", "version": 2, "ok": True, "data": {}}
        bad_type = {"type": "other", "version": 1, "ok": True, "data": {}}
        self.assertIsNone(cpr.parse_cli_success_envelope(self._proc(json.dumps(bad_version))))
        self.assertIsNone(cpr.parse_cli_success_envelope(self._proc(json.dumps(bad_type))))

    def test_rejects_non_object_data(self) -> None:
        payload = {"type": "ravo.cli.result", "version": 1, "ok": True, "data": ["x"]}
        self.assertIsNone(cpr.parse_cli_success_envelope(self._proc(json.dumps(payload))))


class CatalogMembershipPredicateTests(unittest.TestCase):
    def test_count_only_fallback_no_longer_accepted(self) -> None:
        for count in (1, "1", True, -1, None):
            assets = [{"id": "wrong", "uri": "file:///x"}]
            ok, _ = cpr.exact_catalog_asset_membership(
                assets, asset_id="want", asset_uri="file:///expected/source/synthetic.png"
            )
            self.assertFalse(ok, msg=repr(count))

    def test_rejects_id_match_with_wrong_uri(self) -> None:
        ok, detail = cpr.exact_catalog_asset_membership(
            [{"id": "asset-1", "uri": "file:///different/wrong.png"}],
            asset_id="asset-1",
            asset_uri="file:///expected/source/synthetic.png",
        )
        self.assertFalse(ok)
        self.assertIn("exact", detail)

    def test_rejects_uri_match_with_wrong_id(self) -> None:
        ok, _ = cpr.exact_catalog_asset_membership(
            [{"id": "other", "uri": "file:///expected/source/synthetic.png"}],
            asset_id="asset-1",
            asset_uri="file:///expected/source/synthetic.png",
        )
        self.assertFalse(ok)

    def test_rejects_extra_or_duplicate_members(self) -> None:
        uri = "file:///expected/source/synthetic.png"
        ok, detail = cpr.exact_catalog_asset_membership(
            [{"id": "asset-1", "uri": uri}, {"id": "extra", "uri": "file:///y"}],
            asset_id="asset-1",
            asset_uri=uri,
        )
        self.assertFalse(ok)
        self.assertIn("exactly one", detail)
        ok, _ = cpr.exact_catalog_asset_membership(
            [{"id": "asset-1", "uri": uri}, {"id": "asset-1", "uri": uri}],
            asset_id="asset-1",
            asset_uri=uri,
        )
        self.assertFalse(ok)

    def test_accepts_exact_single_member(self) -> None:
        uri = "file:///expected/source/synthetic.png"
        ok, _ = cpr.exact_catalog_asset_membership(
            [{"id": "asset-1", "uri": uri}],
            asset_id="asset-1",
            asset_uri=uri,
        )
        self.assertTrue(ok)





class DmgTopLevelSymlinkTests(unittest.TestCase):
    def test_absolute_applications_symlink_is_skipped(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            mount = root / "mnt"
            out = root / "out"
            mount.mkdir()
            out.mkdir()
            external = root / "external"
            external.mkdir()
            marker = external / "marker.txt"
            marker.write_text("outside", encoding="utf-8")
            app = mount / "Ravo.app"
            app.mkdir()
            (app / "Contents").mkdir()
            (mount / "Applications").symlink_to(external)
            actions = {
                "app": cpr.copy_dmg_top_level_entry(app, out),
                "applications": cpr.copy_dmg_top_level_entry(mount / "Applications", out),
            }
            self.assertEqual(actions["app"], "copied")
            self.assertEqual(actions["applications"], "skipped")
            self.assertTrue((out / "Ravo.app" / "Contents").is_dir())
            self.assertFalse((out / "Applications").exists())
            self.assertFalse((out / "marker.txt").exists())

    def test_relative_in_package_symlink_preserved(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            mount = root / "mnt"
            out = root / "out"
            mount.mkdir()
            out.mkdir()
            (mount / "Payload").mkdir()
            (mount / "Alias").symlink_to("Payload")
            self.assertEqual(cpr.copy_dmg_top_level_entry(mount / "Alias", out), "linked")
            self.assertTrue((out / "Alias").is_symlink())
            self.assertEqual(os.readlink(out / "Alias"), "Payload")



class EvidenceCollectionOrderTests(unittest.TestCase):
    def test_failed_checker_still_writes_evidence_json(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            payload = tmp_path / "payload"
            payload.mkdir()
            _write_fake_cli(payload / "ravo")
            artifact = tmp_path / "one.zip"
            with zipfile.ZipFile(artifact, "w") as zf:
                zf.write(payload / "ravo", arcname="ravo")
            evidence_root = tmp_path / "evidence"
            evidence_root.mkdir()
            evidence = evidence_root / "one.zip.evidence.json"
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
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertNotEqual(proc.returncode, 0)
            self.assertTrue(evidence.is_file())
            self.assertIn("studio_payload", evidence.read_text(encoding="utf-8"))



class ProbeArtifactContractTests(unittest.TestCase):
    def test_require_probe_artifact_matches_streamed_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            probe_out = Path(tmp) / "probe.png"
            cpr.write_minimal_png(probe_out, width=8, height=8)
            digest, size = cpr.sha256_file_bounded(probe_out, max_bytes=cpr._PROBE_PNG_MAX_FILE_BYTES)
            probe_data = {
                "asset_id": "ast_1",
                "width": 8,
                "height": 8,
                "color_profile": "srgb",
                "artifact": {
                    "type": "ravo.image_artifact",
                    "version": 1,
                    "path": str(probe_out.resolve()),
                    "mime_type": "image/png",
                    "width": 8,
                    "height": 8,
                    "byte_count": size,
                    "color_profile": "embedded_icc",
                    "color_profile_fingerprint": "abcdef0123456789",
                    "content_sha256": digest,
                },
            }
            got, got_size = cpr.require_probe_artifact(probe_data, probe_out)
            self.assertEqual(got, digest)
            self.assertEqual(got_size, size)

    def test_require_probe_artifact_fails_closed_without_artifact(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            probe_out = Path(tmp) / "probe.png"
            cpr.write_minimal_png(probe_out, width=8, height=8)
            with self.assertRaisesRegex(ValueError, "missing nested artifact"):
                cpr.require_probe_artifact({"width": 8, "height": 8}, probe_out)

    def test_require_probe_artifact_rejects_digest_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            probe_out = Path(tmp) / "probe.png"
            cpr.write_minimal_png(probe_out, width=8, height=8)
            digest, size = cpr.sha256_file_bounded(probe_out, max_bytes=cpr._PROBE_PNG_MAX_FILE_BYTES)
            bad = "0" * 64
            self.assertNotEqual(bad, digest)
            probe_data = {
                "width": 8,
                "height": 8,
                "artifact": {
                    "type": "ravo.image_artifact",
                    "version": 1,
                    "path": str(probe_out.resolve()),
                    "mime_type": "image/png",
                    "width": 8,
                    "height": 8,
                    "byte_count": size,
                    "color_profile": "srgb",
                    "color_profile_fingerprint": "fp",
                    "content_sha256": bad,
                },
            }
            with self.assertRaisesRegex(ValueError, "sha256"):
                cpr.require_probe_artifact(probe_data, probe_out)

    def test_sha256_file_bounded_rejects_over_cap(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "big.bin"
            path.write_bytes(b"x" * 100)
            with self.assertRaisesRegex(ValueError, "hard byte cap"):
                cpr.sha256_file_bounded(path, max_bytes=16)

    def test_expected_file_uri_is_exact_not_lowercased(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "Case Sensitive.png"
            path.write_bytes(b"x")
            uri = cpr.expected_file_uri(path)
            self.assertIn("Case%20Sensitive.png", uri)
            self.assertTrue(uri.startswith("file://"))
            self.assertNotEqual(uri, uri.lower())



if __name__ == "__main__":
    unittest.main()
