#!/usr/bin/env python3
"""Native startup failure/isolation contracts; no Qt or privileged install needed."""

from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import check_packaged_runtime as runtime
import packaged_native_smoke as native


def write_frame_log(work):
    (work / "native-studio.log").write_text(
        '{"type":"ravo.native_startup","version":1,"first_frame":true}\n')


class NativeSmokeTests(unittest.TestCase):
    def test_explicit_native_plugin_and_clean_loader_environment(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            write_frame_log(work)
            polluted = {"QT_QPA_PLATFORM": "offscreen", "LD_PRELOAD": "bad.so",
                        "DYLD_INSERT_LIBRARIES": "bad.dylib", "QT_PLUGIN_PATH": "/dev/qt"}
            with mock.patch.dict(runtime.os.environ, polluted), \
                    mock.patch.object(native.sys, "platform", "darwin"), \
                    mock.patch.object(native, "run_logged") as run:
                env = runtime.cleaned_env(home=work, allow_offscreen=False)
                native.run_native_smoke(work / "a.dmg", work / "studio", work / "cli", work, env)
            command = run.call_args.args[0]
            self.assertEqual(command, [str(work / "studio"), "--startup-smoke", "-platform", "cocoa"])
            passed = run.call_args.kwargs["env"]
            for key in polluted:
                self.assertNotIn(key, passed)
            self.assertEqual(passed["QT_DEBUG_PLUGINS"], "1")

    def test_appimage_runs_final_image_without_extract_fallback(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            image = work / "a.AppImage"
            image.write_bytes(b"fixture")
            write_frame_log(work)
            with mock.patch.object(native.sys, "platform", "linux"), \
                    mock.patch.object(native, "run_logged") as run:
                native.run_native_smoke(image, work / "studio", work / "cli", work,
                                        {"DISPLAY": ":99", "APPIMAGE_EXTRACT_AND_RUN": "1"})
            command = run.call_args.args[0]
            self.assertEqual(Path(command[0]).read_bytes(), b"fixture")
            self.assertTrue(Path(command[0]).stat().st_mode & 0o100)
            self.assertEqual(command[1:], ["--startup-smoke", "-platform", "xcb"])
            self.assertNotIn("APPIMAGE_EXTRACT_AND_RUN", run.call_args.kwargs["env"])

    def test_no_display_is_failure(self):
        with mock.patch.object(native.sys, "platform", "linux"):
            with self.assertRaisesRegex(RuntimeError, "requires DISPLAY"):
                native.run_native_smoke(Path("a.deb"), Path("studio"), Path("cli"), Path("."), {})

    def test_exit_zero_without_frame_evidence_is_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            (work / "native-studio.log").write_text("started but no frame\n")
            with mock.patch.object(native.sys, "platform", "darwin"), \
                    mock.patch.object(native, "run_logged"):
                with self.assertRaisesRegex(RuntimeError, "without first-frame evidence"):
                    native.run_native_smoke(work / "a.dmg", work / "studio", work / "cli", work, {})

    def test_deb_failed_launch_still_purges(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            commands = []

            def run(command, **kwargs):
                commands.append(command)
                if command[0] == "/usr/bin/ravo_studio":
                    raise RuntimeError("missing library")

            with mock.patch.object(native.sys, "platform", "linux"), \
                    mock.patch.object(native.Path, "exists", return_value=False), \
                    mock.patch.object(native.subprocess, "check_output", return_value="ravostudio\n"), \
                    mock.patch.object(native, "run_logged", side_effect=run):
                with self.assertRaisesRegex(RuntimeError, "missing library"):
                    native.run_native_smoke(work / "a.deb", work / "studio", work / "cli", work,
                                            {"DISPLAY": ":99"})
            self.assertEqual(commands[0][:4], ["sudo", "-n", "dpkg", "--install"])
            self.assertEqual(commands[-1], ["sudo", "-n", "dpkg", "--purge", "ravostudio"])
            self.assertIn(["/usr/bin/ravo", "--help"], commands)

    def test_nonzero_exit_has_log_and_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            with self.assertRaisesRegex(RuntimeError, "exited 7"):
                native.run_logged([sys.executable, "-c", "print('missing library'); exit(7)"],
                                  env=dict(os.environ), work=work, name="failure")
            self.assertIn("missing library", (work / "failure.log").read_text())

    def test_timeout_propagates_with_log(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            with mock.patch.object(native.subprocess, "run",
                                   side_effect=subprocess.TimeoutExpired("studio", 300)):
                with self.assertRaises(subprocess.TimeoutExpired):
                    native.run_logged(["studio"], env={}, work=work, name="timeout")
            self.assertTrue((work / "timeout.log").is_file())


if __name__ == "__main__":
    unittest.main()
