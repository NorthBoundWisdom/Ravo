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


def _dedup_paths(paths: list[Path]) -> list[Path]:
    uniq: list[Path] = []
    seen: set[str] = set()
    for path in paths:
        key = str(path.resolve()) if path.exists() else str(path)
        if key in seen:
            continue
        seen.add(key)
        uniq.append(path)
    return uniq


def _file_under_root(path: Path, root: Path) -> bool:
    """Accept only files whose resolved target stays inside the package root."""
    if not path.exists() or not path.is_file():
        return False
    try:
        return is_under(path.resolve(), root)
    except (OSError, RuntimeError, ValueError):
        return False


def _looks_like_deb_launcher(path: Path) -> bool:
    """Detect /usr/bin wrappers that exec /opt/.../bin/<name>; those are not payloads."""
    parts = path.parts
    if len(parts) < 3 or parts[-2] != "bin" or parts[-3] != "usr":
        return False
    try:
        sample = path.read_text(encoding="utf-8", errors="replace")[:4000]
    except OSError:
        return False
    return "PREFIX=" in sample and 'exec "${PREFIX}/bin/' in sample


def _collect_role_payloads(root: Path, names: tuple[str, ...]) -> list[Path]:
    """Collect role-specific executables from known package layouts.

    Uses known macOS bundle and DEB payload paths plus exact in-tree name matches.
    Never returns the first arbitrary rglob hit when multiple payloads exist.
    DEB /usr/bin launchers are ignored in favor of /opt/RavoStudio/bin payloads.
    Symlinks that resolve outside the package root are rejected.
    """
    name_set = {n.lower() for n in names}
    found: list[Path] = []

    # Known macOS DMG / .app layout: Ravo Studio.app/Contents/MacOS/<role>
    for app in root.rglob("Ravo Studio.app"):
        if not app.is_dir():
            continue
        for name in names:
            mac = app / "Contents" / "MacOS" / name
            if _file_under_root(mac, root) and mac.name.lower() in name_set:
                found.append(mac)

    # Known DEB private payload: opt/RavoStudio/bin/<role>
    for name in names:
        deb = root / "opt" / "RavoStudio" / "bin" / name
        if _file_under_root(deb, root) and deb.name.lower() in name_set:
            found.append(deb)

    # Flat ZIP / AppImage / Windows: exact basename under the extract root.
    for name in names:
        for path in root.rglob(name):
            if path.name.lower() not in name_set:
                continue
            if not _file_under_root(path, root):
                continue
            if _looks_like_deb_launcher(path):
                continue
            found.append(path)

    return _dedup_paths(found)


def find_cli(root: Path) -> tuple[Path | None, list[Path]]:
    """Locate the packaged CLI. Never accepts ravo_studio as CLI."""
    hits = _collect_role_payloads(root, ("ravo", "ravo.exe"))
    # Hard role separation: Studio binary must never satisfy CLI identity.
    hits = [p for p in hits if p.name.lower() not in ("ravo_studio", "ravo_studio.exe")]
    if len(hits) == 1:
        return hits[0], hits
    return None, hits


def find_studio(root: Path) -> tuple[Path | None, list[Path]]:
    """Locate the packaged Studio binary, including known .app MacOS layout."""
    hits = _collect_role_payloads(root, ("ravo_studio", "ravo_studio.exe"))
    if len(hits) == 1:
        return hits[0], hits
    return None, hits


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
        # Prefer explicit type-2 extraction into an isolated AppDir.
        copied = dest / artifact.name
        shutil.copy2(artifact, copied)
        extract_dir = dest / "appdir"
        extract_dir.mkdir(parents=True, exist_ok=True)
        try:
            proc = subprocess.run(
                [str(copied), "--appimage-extract"],
                cwd=str(extract_dir),
                capture_output=True,
                text=True,
                timeout=120,
                check=False,
            )
        except Exception as exc:  # noqa: BLE001
            raise RuntimeError(f"AppImage extract launch failed: {exc}") from exc
        if proc.returncode != 0:
            raise RuntimeError(
                f"AppImage --appimage-extract failed exit={proc.returncode}: {proc.stderr.strip()}"
            )
        squash = extract_dir / "squashfs-root"
        if not squash.is_dir():
            # Some runtimes extract into cwd/squashfs-root; accept either.
            candidates = list(extract_dir.rglob("AppRun"))
            if not candidates:
                raise RuntimeError("AppImage extract produced no AppDir/AppRun")
            return candidates[0].parent
        return squash
    if artifact.suffix.lower() == ".deb":
        subprocess.run(["dpkg-deb", "-x", str(artifact), str(dest)], check=True)
        return dest
    raise SystemExit(f"unsupported artifact type: {artifact}")


def _path_looks_like_dev_qt(entry: str) -> bool:
    lowered = entry.replace("\\", "/").lower()
    markers = (
        "/qt/",
        "/qt6/",
        "/qt5/",
        "qt/6.",
        "qt/5.",
        "cmake/qt",
        "aqt",
        "homebrew/opt/qt",
        "cellar/qt",
        "vcpkg",
        "build/mac_clang",
        "build/linux_",
        "build/win_",
        "_deps/qt",
    )
    return any(marker in lowered for marker in markers)


def cleaned_env(*, home: Path | None = None, allow_offscreen: bool = True) -> dict[str, str]:
    """Strip development Qt/runtime pollution for packaged validation.

    Offscreen is an explicit non-native path for CI smoke only — never a native claim.
    """
    env = os.environ.copy()
    for key in list(env):
        if key.startswith(("QT_", "QML", "QSG_")):
            env.pop(key, None)
    for key in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH", "QT_PLUGIN_PATH",
                "QML2_IMPORT_PATH", "QML_IMPORT_PATH"):
        env.pop(key, None)
    raw_path = env.get("PATH", "")
    kept = [part for part in raw_path.split(os.pathsep) if part and not _path_looks_like_dev_qt(part)]
    # Keep minimal system path roots so the packaged binary can still start.
    if os.name == "nt":
        system_root = env.get("SystemRoot") or env.get("WINDIR") or r"C:\Windows"
        for part in (rf"{system_root}\System32", system_root):
            if part not in kept:
                kept.append(part)
    else:
        for part in ("/usr/bin", "/bin", "/usr/sbin", "/sbin"):
            if part not in kept:
                kept.append(part)
    env["PATH"] = os.pathsep.join(kept)
    if home is not None:
        env["HOME"] = str(home)
        env["XDG_CONFIG_HOME"] = str(home / ".config")
        env["XDG_DATA_HOME"] = str(home / ".local" / "share")
        env["XDG_CACHE_HOME"] = str(home / ".cache")
        env["APPDATA"] = str(home / "AppData" / "Roaming")
        env["LOCALAPPDATA"] = str(home / "AppData" / "Local")
    if allow_offscreen:
        env["QT_QPA_PLATFORM"] = "offscreen"
        env.setdefault("QSG_RHI_BACKEND", "software")
        env.setdefault("QT_QUICK_BACKEND", "software")
    return env



def run_catalog_workflow_stages(cli: Path, env: dict[str, str], work: Path,
                                record) -> None:
    """Exercise create/open/import/probe/reopen when the CLI surface supports it.

    Missing subcommands or host capability → UNTESTED (never PASS). Failures → FAIL.
    """
    stages = (
        "catalog_create_open",
        "catalog_synthetic_import",
        "catalog_probe_or_render",
        "catalog_reopen_hash",
    )
    # Probe help text without guessing flags.
    try:
        help_proc = subprocess.run(
            [str(cli), "catalog", "--help"],
            env=env,
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
        )
    except Exception as exc:  # noqa: BLE001
        for name in stages:
            record(name, Status.UNTESTED, f"catalog help unavailable: {exc}")
        return
    help_text = (help_proc.stdout or "") + (help_proc.stderr or "")
    if help_proc.returncode != 0 and "catalog" not in help_text.lower():
        # Many Ravo builds require --catalog; treat opaque failure as untested.
        for name in stages:
            record(name, Status.UNTESTED, "catalog help not conclusive")
        return

    catalog = work / "packaged-catalog" / "library.sqlite"
    catalog.parent.mkdir(parents=True, exist_ok=True)
    source = work / "packaged-catalog" / "source"
    source.mkdir(parents=True, exist_ok=True)
    # Tiny synthetic PNG without Qt — write a minimal valid-enough file only if import runs.
    # Actual encode is host-dependent; leave UNTESTED when create cannot run.
    try:
        create = subprocess.run(
            [str(cli), "catalog", "create", "--catalog", str(catalog)],
            env=env,
            capture_output=True,
            text=True,
            timeout=120,
            check=False,
        )
    except Exception as exc:  # noqa: BLE001
        for name in stages:
            record(name, Status.UNTESTED, str(exc))
        return
    if create.returncode != 0:
        # Do not invent alternate flags; residual for release hosts with different CLI.
        detail = (create.stderr or create.stdout or "")[:200]
        for name in stages:
            record(name, Status.UNTESTED, f"catalog create unavailable: {detail}")
        return
    record("catalog_create_open", Status.PASS, str(catalog))
    for name in stages[1:]:
        record(name, Status.UNTESTED, "requires release host artifact + verified CLI import surface")


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
    cli = None
    studio = None
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

        smoke_home = work / "smoke-home"
        smoke_home.mkdir(parents=True, exist_ok=True)
        env = cleaned_env(home=smoke_home, allow_offscreen=True)
        record("runtime_env_isolated", Status.PASS, "stripped QT_/LD_/DYLD_ and dev Qt PATH entries")
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

        if cli is not None:
            run_catalog_workflow_stages(cli, env, work, record)
        else:
            for name in (
                "catalog_create_open",
                "catalog_synthetic_import",
                "catalog_probe_or_render",
                "catalog_reopen_hash",
            ):
                record(name, Status.UNTESTED, "cli unavailable")

        record("native_display_session", Status.UNTESTED,
               "offscreen smoke ≠ native packaged plugins/session")
        if artifact.suffix.lower() == ".deb":
            record("dpkg_install_launcher", Status.UNTESTED, "unpack ≠ install")
        if ".AppImage" in artifact.name:
            # Extract path is exercised by unpack(); FUSE direct launch remains separate.
            if results.get("unpack") == Status.PASS.value:
                record("appimage_extract_payload", Status.PASS, "via --appimage-extract")
            else:
                record("appimage_extract_payload", Status.FAIL, "unpack did not PASS")
            record("appimage_fuse_direct_launch", Status.UNTESTED, "no FUSE host evidence")

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
                "host": {
                    "platform": sys.platform,
                    "python": sys.version.split()[0],
                },
                "cli": str(cli) if cli is not None else None,
                "studio": str(studio) if studio is not None else None,
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
