#!/usr/bin/env python3
"""Validate a packaged Ravo Studio artifact outside the build tree.

Checks (best-effort per host):
  - artifact exists and SHA256 is printed
  - CLI binary is present in the payload
  - optional: launch CLI --help and Studio smoke with cleaned Qt/QML env

Does not claim dpkg install or native desktop GUI success from unpack alone.
Missing host capabilities are reported as UNTESTED residuals (exit 0 only when
required structural checks pass; use --require-smoke to fail on launch).
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def find_cli(root: Path) -> Path | None:
    candidates = list(root.rglob("ravo")) + list(root.rglob("ravo.exe"))
    for path in candidates:
        if path.is_file() and os.access(path, os.X_OK if path.suffix != ".exe" else os.F_OK):
            return path
    return None


def find_studio(root: Path) -> Path | None:
    for name in ("ravo_studio", "ravo_studio.exe", "Ravo Studio"):
        for path in root.rglob(name):
            if path.is_file():
                return path
    apps = list(root.rglob("Ravo Studio.app"))
    if apps:
        mac = apps[0] / "Contents" / "MacOS" / "ravo_studio"
        if mac.is_file():
            return mac
    return None


def unpack(artifact: Path, dest: Path) -> Path:
    suffix = "".join(artifact.suffixes).lower()
    if artifact.suffix.lower() == ".zip" or suffix.endswith(".zip"):
        with zipfile.ZipFile(artifact) as zf:
            zf.extractall(dest)
        return dest
    if artifact.suffix.lower() == ".dmg":
        # Attach and copy payload; host-dependent.
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
        # Structural only unless --require-smoke; AppImage needs FUSE on many hosts.
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact", type=Path)
    parser.add_argument("--workdir", type=Path, default=None)
    parser.add_argument("--require-smoke", action="store_true")
    args = parser.parse_args()
    artifact = args.artifact.resolve()
    if not artifact.is_file():
        print(f"ERROR: artifact missing: {artifact}", file=sys.stderr)
        return 1
    digest = sha256_file(artifact)
    print(f"artifact={artifact}")
    print(f"sha256={digest}")

    residuals: list[str] = []
    with tempfile.TemporaryDirectory(prefix="ravo-packaged-") as tmp:
        work = Path(args.workdir) if args.workdir else Path(tmp) / "out"
        work.mkdir(parents=True, exist_ok=True)
        # Ensure work is outside the source/build tree when caller passes --workdir.
        try:
            root = unpack(artifact, work)
        except Exception as exc:  # noqa: BLE001 — report as residual/failure
            print(f"ERROR: unpack failed: {exc}", file=sys.stderr)
            return 1

        cli = find_cli(root)
        studio = find_studio(root)
        if cli is None:
            print("ERROR: CLI binary not found in payload", file=sys.stderr)
            return 1
        print(f"cli={cli}")
        if studio is None:
            residuals.append("UNTESTED: Studio binary not located in payload layout")
        else:
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
            if help_proc.returncode != 0 and args.require_smoke:
                print(help_proc.stderr, file=sys.stderr)
                return 1
        except Exception as exc:  # noqa: BLE001
            residuals.append(f"UNTESTED: CLI --help launch failed: {exc}")
            if args.require_smoke:
                return 1

        if studio is not None:
            smoke = Path(__file__).resolve().parent / "smoke_ravo_studio.py"
            if smoke.is_file():
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
                        residuals.append("UNTESTED/FAIL: Studio offscreen smoke non-zero")
                        if args.require_smoke:
                            print(proc.stderr, file=sys.stderr)
                            return 1
                except Exception as exc:  # noqa: BLE001
                    residuals.append(f"UNTESTED: Studio smoke failed: {exc}")
                    if args.require_smoke:
                        return 1
            else:
                residuals.append("UNTESTED: smoke_ravo_studio.py missing")

        # Explicit non-claims
        residuals.append("UNTESTED: native display / real installed desktop session")
        if artifact.suffix.lower() == ".deb":
            residuals.append("UNTESTED: dpkg install and /usr/bin launcher (unpack ≠ install)")
        if ".AppImage" in artifact.name:
            residuals.append("UNTESTED: AppImage FUSE execution on this host")

    for line in residuals:
        print(line)
    print("RESULT: structural packaged-runtime checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
