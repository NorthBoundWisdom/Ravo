#!/usr/bin/env python3
"""Build-tree CLI evidence for package verification orchestration.

This matrix is intentionally labeled build-tree evidence, not packaged PASS.
Static CI runs only the unit helpers below. The create/import/probe/reopen
workflow is registered in CTest with an explicit $<TARGET_FILE:ravo> path.
"""

from __future__ import annotations

import argparse
import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_packaged_runtime as cpr  # noqa: E402

BUILD_TREE_MATRIX_LABEL = "build-tree"
PACKAGED_MATRIX_LABEL = "packaged"


def build_tree_env(*, home: Path) -> dict[str, str]:
    """Env for build-tree CLI runs.

    Unlike packaged cleaned_env(), keep the process loader/Qt paths so the
    just-built binary can resolve its build-tree dependencies. Still isolate
    HOME and force offscreen for headless smoke.
    """
    env = os.environ.copy()
    env["HOME"] = str(home)
    env["XDG_CONFIG_HOME"] = str(home / ".config")
    env["XDG_DATA_HOME"] = str(home / ".local" / "share")
    env["XDG_CACHE_HOME"] = str(home / ".cache")
    env["APPDATA"] = str(home / "AppData" / "Roaming")
    env["LOCALAPPDATA"] = str(home / "AppData" / "Local")
    env["QT_QPA_PLATFORM"] = "offscreen"
    env.setdefault("QSG_RHI_BACKEND", "software")
    env.setdefault("QT_QUICK_BACKEND", "software")
    return env


def resolve_explicit_cli(
    *,
    cli_arg: str | None = None,
    environ: dict[str, str] | None = None,
) -> Path:
    """Resolve the CLI from an explicit path only.

    Missing, non-file, or non-executable paths fail closed. Never skip, and
    never probe alternate Debug presets or sibling checkouts.
    """
    env = environ if environ is not None else os.environ
    raw = cli_arg if cli_arg is not None else env.get("RAVO_CLI_EXECUTABLE")
    if raw is None or not str(raw).strip():
        raise FileNotFoundError(
            "explicit CLI required via --cli or RAVO_CLI_EXECUTABLE "
            "(build-tree workflow must not skip or guess another preset)"
        )
    path = Path(raw)
    if not path.is_file():
        raise FileNotFoundError(f"explicit CLI path is not a file: {path}")
    mode = path.stat().st_mode
    if not (mode & (stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)):
        raise PermissionError(f"explicit CLI path is not executable: {path}")
    return path


def matrix_label_for_cli(cli: Path) -> str:
    """Keep build-tree and packaged evidence vocabularies distinct."""
    text = str(cli)
    if "packaged" in text.lower() or "/install/" in text.replace("\\", "/"):
        return PACKAGED_MATRIX_LABEL
    return BUILD_TREE_MATRIX_LABEL


class BuildTreePackageVerificationUnitTests(unittest.TestCase):
    """No binary required — safe for Static. Must not skip a workflow PASS."""

    def test_missing_explicit_cli_fails_not_skips(self) -> None:
        with self.assertRaises(FileNotFoundError) as raised:
            resolve_explicit_cli(cli_arg=None, environ={})
        self.assertIn("explicit CLI required", str(raised.exception))

    def test_nonexistent_explicit_cli_fails(self) -> None:
        missing = "/no/such/ravo/cli/binary"
        with self.assertRaises(FileNotFoundError) as raised:
            resolve_explicit_cli(cli_arg=missing, environ={})
        message = str(raised.exception)
        reported = Path(missing)
        # Path() normalizes separators (Windows -> \no\such\...), so accept either
        # str(Path) or as_posix while still requiring the missing path to appear.
        self.assertTrue(
            str(reported) in message or reported.as_posix() in message,
            msg=f"expected path form of {missing!r} in {message!r}",
        )

    def test_non_executable_explicit_cli_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "ravo"
            path.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            # Intentionally leave without execute bit.
            path.chmod(stat.S_IRUSR | stat.S_IWUSR)
            with self.assertRaises(PermissionError):
                resolve_explicit_cli(cli_arg=str(path), environ={})

    def test_matrix_label_is_build_tree_not_packaged(self) -> None:
        cli = Path("/repo/build/mac_clang_debug/Ravo/cli/ravo")
        self.assertEqual(matrix_label_for_cli(cli), BUILD_TREE_MATRIX_LABEL)
        self.assertNotEqual(BUILD_TREE_MATRIX_LABEL, PACKAGED_MATRIX_LABEL)


class BuildTreeCliWorkflowTests(unittest.TestCase):
    def test_create_import_probe_reopen_with_artifact_contract(self) -> None:
        cli = resolve_explicit_cli()
        self.assertEqual(matrix_label_for_cli(cli), BUILD_TREE_MATRIX_LABEL)
        with tempfile.TemporaryDirectory(prefix="ravo-build-tree-evidence-") as tmp:
            work = Path(tmp)
            env = build_tree_env(home=work / "home")
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
            # Binary identity is the explicit path, not a trailing-name guess alone.
            self.assertTrue(cli.is_file())
            self.assertTrue(os.access(cli, os.X_OK))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--cli",
        default=None,
        help="Explicit built ravo CLI path (also set as RAVO_CLI_EXECUTABLE)",
    )
    parser.add_argument(
        "--unit-only",
        action="store_true",
        help="Run Static-safe unit helpers only (no CLI workflow)",
    )
    args, remaining = parser.parse_known_args(argv)
    if args.cli:
        os.environ["RAVO_CLI_EXECUTABLE"] = args.cli
    if args.unit_only:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(
            BuildTreePackageVerificationUnitTests
        )
    else:
        suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    # Preserve unittest argv compatibility when invoked via -m unittest.
    if remaining:
        sys.argv = [sys.argv[0], *remaining]
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    # Allow `python -m unittest ...` discovery and direct --cli/--unit-only use.
    if any(arg.startswith("--cli") or arg == "--unit-only" for arg in sys.argv[1:]):
        raise SystemExit(main())
    unittest.main()
