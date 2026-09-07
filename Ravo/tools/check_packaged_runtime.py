#!/usr/bin/env python3
"""Validate a packaged Ravo Studio artifact outside the build tree.

Result vocabulary: PASS / FAIL / UNTESTED / NOT_APPLICABLE.
With --require-smoke, missing required stages fail closed (no UNTESTED success).
"""

from __future__ import annotations

import argparse
import enum
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile


class Status(enum.Enum):
    PASS = "PASS"
    FAIL = "FAIL"
    UNTESTED = "UNTESTED"
    NOT_APPLICABLE = "NOT_APPLICABLE"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def find_unique(root: Path, names: tuple[str, ...]) -> tuple[Path | None, list[Path]]:
    found: list[Path] = []
    for name in names:
        for path in root.rglob(name):
            if path.is_file() or (path.is_dir() and path.suffix == ".app"):
                found.append(path)
    # Also accept Ravo Studio.app layout
    for path in root.rglob("Ravo Studio.app"):
        mac = path / "Contents" / "MacOS" / "ravo_studio"
        if mac.is_file():
            found.append(mac)
    # De-dup
    uniq: list[Path] = []
    seen: set[str] = set()
    for path in found:
        key = str(path.resolve()) if path.exists() else str(path)
        if key in seen:
            continue
        seen.add(key)
        uniq.append(path)
    if len(uniq) == 1:
        return uniq[0], uniq
    return None, uniq


def find_cli(root: Path) -> tuple[Path | None, list[Path]]:
    return find_unique(root, ("ravo", "ravo.exe"))


def find_studio(root: Path) -> tuple[Path | None, list[Path]]:
    studio, all_hits = find_unique(root, ("ravo_studio", "ravo_studio.exe"))
    return studio, all_hits


def is_under(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except ValueError:
        return False


def unpack(artifact: Path, dest: Path) -> Path:
    suffix = "".join(artifact.suffixes).lower()
    if artifact.suffix.lower() == ".zip" or suffix.endswith(".zip"):
        with zipfile.ZipFile(artifact) as zf:
            zf.extractall(dest)
        return dest
    if artifact.suffix.lower() == ".dmg":
        mount = dest / "mnt"
        mount.mkdir()
        subprocess.run(
            ["hdiutil", "attach", "-nobrowse", "-readonly", "-mountpoint", str(mount), str(artifact)],
            check=True,
        )
        try:
            for child in mount.iterdir():
                target = dest / child.name
                if child.is_dir():
                    shutil.copytree(child, target, symlinks=True)
                else:
                    shutil.copy2(child, target)
        finally:
            subprocess.run(["hdiutil", "detach", str(mount)], check=False)
        return dest
    if ".AppImage" in artifact.name:
        # Structural copy only here; extraction is commit 13. Still place the file.
        shutil.copy2(artifact, dest / artifact.name)
        return dest
    if artifact.suffix.lower() == ".deb":
        subprocess.run(["dpkg-deb", "-x", str(artifact), str(dest)], check=True)
        return dest
    raise SystemExit(f"unsupported artifact type: {artifact}")


def cleaned_env() -> dict[str, str]:
    env = os.environ.copy()
    for key in list(env):
        if key.startswith(("QT_", "QML", "QSG_")):
            env.pop(key, None)
    env["QT_QPA_PLATFORM"] = "offscreen"
    env.setdefault("QSG_RHI_BACKEND", "software")
    env.setdefault("QT_QUICK_BACKEND", "software")
    return env


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact", type=Path)
    parser.add_argument("--workdir", type=Path, default=None)
    parser.add_argument("--require-smoke", action="store_true")
    parser.add_argument("--repo-root", type=Path, default=None,
                        help="Reject workdirs inside this build/source tree")
    parser.add_argument("--evidence-json", type=Path, default=None)
    args = parser.parse_args(argv)
    artifact = args.artifact.resolve()
    results: dict[str, str] = {}
    residuals: list[str] = []

    def record(name: str, status: Status, detail: str = "") -> None:
        results[name] = status.value
        line = f"{status.value}: {name}" + (f" ({detail})" if detail else "")
        print(line)
        if status == Status.UNTESTED:
            residuals.append(line)

    if not artifact.is_file():
        print(f"ERROR: artifact missing: {artifact}", file=sys.stderr)
        return 1
    digest = sha256_file(artifact)
    print(f"artifact={artifact}")
    print(f"sha256={digest}")

    # Unique workdir: reject reuse of non-empty caller dirs; never under build tree.
    tmp_ctx = None
    if args.workdir is None:
        tmp_ctx = tempfile.TemporaryDirectory(prefix="ravo-packaged-")
        work = Path(tmp_ctx.name) / "out"
        work.mkdir(parents=True, exist_ok=False)
    else:
        work = args.workdir.resolve()
        if work.exists() and any(work.iterdir()):
            print(f"ERROR: workdir is not empty (refuse polluted directory): {work}", file=sys.stderr)
            return 1
        work.mkdir(parents=True, exist_ok=True)
        repo = args.repo_root.resolve() if args.repo_root else None
        if repo is not None and is_under(work, repo):
            print(f"ERROR: workdir is inside repo/build tree: {work}", file=sys.stderr)
            return 1

    exit_code = 0
    try:
        try:
            root = unpack(artifact, work)
            record("unpack", Status.PASS)
        except Exception as exc:  # noqa: BLE001
            print(f"ERROR: unpack failed: {exc}", file=sys.stderr)
            record("unpack", Status.FAIL, str(exc))
            return 1

        cli, cli_hits = find_cli(root)
        if cli is None:
            detail = f"candidates={len(cli_hits)}"
            record("cli_payload", Status.FAIL, detail)
            print("ERROR: CLI binary not found or not unique in payload", file=sys.stderr)
            return 1
        record("cli_payload", Status.PASS, str(cli))
        print(f"cli={cli}")

        studio, studio_hits = find_studio(root)
        if studio is None:
            if args.require_smoke:
                record("studio_payload", Status.FAIL, f"candidates={len(studio_hits)}")
                print("ERROR: Studio binary not located (required by --require-smoke)", file=sys.stderr)
                return 1
            record("studio_payload", Status.UNTESTED, f"candidates={len(studio_hits)}")
        else:
            if len(studio_hits) > 1:
                # Unique identity required for smoke.
                record("studio_payload", Status.FAIL, f"ambiguous candidates={len(studio_hits)}")
                if args.require_smoke:
                    return 1
            else:
                record("studio_payload", Status.PASS, str(studio))
                print(f"studio={studio}")

        env = cleaned_env()
        try:
            help_proc = subprocess.run(
                [str(cli), "--help"],
                env=env,
                capture_output=True,
                text=True,
                timeout=60,
                check=False,
            )
            print(f"cli_help_exit={help_proc.returncode}")
            if help_proc.returncode != 0:
                record("cli_help", Status.FAIL, f"exit={help_proc.returncode}")
                if args.require_smoke:
                    print(help_proc.stderr, file=sys.stderr)
                    return 1
            else:
                record("cli_help", Status.PASS)
        except Exception as exc:  # noqa: BLE001
            record("cli_help", Status.FAIL if args.require_smoke else Status.UNTESTED, str(exc))
            if args.require_smoke:
                return 1

        smoke = Path(__file__).resolve().parent / "smoke_ravo_studio.py"
        if studio is not None and results.get("studio_payload") == Status.PASS.value:
            if not smoke.is_file():
                record("studio_smoke", Status.FAIL if args.require_smoke else Status.UNTESTED,
                       "smoke_ravo_studio.py missing")
                if args.require_smoke:
                    return 1
            else:
                try:
                    proc = subprocess.run(
                        [sys.executable, str(smoke), str(studio)],
                        env=env,
                        capture_output=True,
                        text=True,
                        timeout=120,
                        check=False,
                    )
                    print(f"studio_smoke_exit={proc.returncode}")
                    if proc.returncode != 0:
                        record("studio_smoke", Status.FAIL, f"exit={proc.returncode}")
                        if args.require_smoke:
                            print(proc.stderr, file=sys.stderr)
                            return 1
                    else:
                        record("studio_smoke", Status.PASS)
                except Exception as exc:  # noqa: BLE001
                    record("studio_smoke", Status.FAIL if args.require_smoke else Status.UNTESTED, str(exc))
                    if args.require_smoke:
                        return 1
        elif args.require_smoke:
            # studio missing already returned; defensive
            record("studio_smoke", Status.FAIL, "studio unavailable")
            return 1
        else:
            record("studio_smoke", Status.UNTESTED, "studio unavailable")

        record("native_display_session", Status.UNTESTED)
        if artifact.suffix.lower() == ".deb":
            record("dpkg_install_launcher", Status.UNTESTED, "unpack ≠ install")
        if ".AppImage" in artifact.name:
            record("appimage_fuse_or_extract", Status.UNTESTED)

        # Fail closed: --require-smoke forbids any FAIL and forbids missing required PASS.
        required = ("unpack", "cli_payload", "cli_help", "studio_payload", "studio_smoke")
        if args.require_smoke:
            for name in required:
                status = results.get(name)
                if status != Status.PASS.value:
                    print(f"ERROR: required stage not PASS under --require-smoke: {name}={status}",
                          file=sys.stderr)
                    exit_code = 1
                    break
        if any(v == Status.FAIL.value for v in results.values()):
            exit_code = 1

    finally:
        if args.evidence_json:
            payload = {
                "artifact": str(artifact),
                "sha256": digest,
                "results": results,
                "residuals": residuals,
                "require_smoke": bool(args.require_smoke),
            }
            args.evidence_json.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
        if tmp_ctx is not None:
            tmp_ctx.cleanup()

    for line in residuals:
        print(line)
    if exit_code == 0:
        print("RESULT: packaged-runtime checks completed without required failures")
    else:
        print("RESULT: packaged-runtime checks FAILED", file=sys.stderr)
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
