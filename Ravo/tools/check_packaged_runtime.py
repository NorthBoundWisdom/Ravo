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
import stat
import struct
import zlib
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

def sha256_file_bounded(path: Path, *, max_bytes: int) -> tuple[str, int]:
    """Stream a file under a hard byte cap; refuse unbounded read_bytes()."""
    digest = hashlib.sha256()
    total = 0
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                break
            total += len(chunk)
            if total > max_bytes:
                raise ValueError(f"file exceeds hard byte cap ({max_bytes} bytes)")
            digest.update(chunk)
    return digest.hexdigest(), total



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


def copy_dmg_top_level_entry(child: Path, dest: Path) -> str:
    """Copy one DMG mount child into dest. Returns action: copied|linked|skipped."""
    target = dest / child.name
    st = child.lstat()
    if stat.S_ISLNK(st.st_mode):
        link_target = os.readlink(child)
        if os.path.isabs(link_target) or child.name in {"Applications", "Trash"}:
            return "skipped"
        target.symlink_to(link_target)
        return "linked"
    if stat.S_ISDIR(st.st_mode):
        shutil.copytree(child, target, symlinks=True)
        return "copied"
    if stat.S_ISREG(st.st_mode):
        shutil.copy2(child, target)
        return "copied"
    return "skipped"


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
                copy_dmg_top_level_entry(child, dest)
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
            raise RuntimeError("AppImage extract produced no squashfs-root AppDir")
        apprun = squash / "AppRun"
        if apprun.is_symlink():
            resolved = apprun.resolve()
            if not is_under(resolved, squash):
                raise RuntimeError("AppImage AppRun symlink escapes squashfs-root")
        if not apprun.is_file():
            raise RuntimeError("AppImage AppDir missing AppRun at extract root")
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
    for key in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH", "DYLD_FALLBACK_LIBRARY_PATH",
                "QT_PLUGIN_PATH", "QML2_IMPORT_PATH", "QML_IMPORT_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH"):
        env.pop(key, None)
    # Minimal PATH: known system dirs only (do not inherit a filtered developer PATH).
    if os.name == "nt":
        system_root = env.get("SystemRoot") or env.get("WINDIR") or r"C:\Windows"
        kept = [rf"{system_root}\System32", system_root]
    else:
        kept = ["/usr/bin", "/bin", "/usr/sbin", "/sbin"]
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





def build_rgb_png_bytes(
    *,
    width: int = 8,
    height: int = 8,
    compression: int = 0,
    filter_method: int = 0,
    interlace: int = 0,
    include_srgb: bool = True,
    include_idat: bool = True,
    idat_payload: bytes | None = None,
    filter_bytes: list[int] | None = None,
    iccp_profile: bytes | None = None,
    cicp: tuple[int, int, int, int] | None = None,
) -> bytes:
    """Build a probe-shaped RGB8 PNG for checker regression fixtures."""

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    rows = []
    for y in range(height):
        filter_byte = 0 if filter_bytes is None else filter_bytes[y]
        row = bytearray([filter_byte])
        for x in range(width):
            row.extend([((x + y) * 17) % 256, 40, 80])
        rows.append(bytes(row))
    raw = b"".join(rows)
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, compression, filter_method, interlace)
    parts = [b"\x89PNG\r\n\x1a\n", chunk(b"IHDR", ihdr)]
    if include_srgb:
        parts.append(chunk(b"sRGB", bytes([0])))
    if cicp is not None:
        parts.append(chunk(b"cICP", bytes(cicp)))
    if iccp_profile is not None:
        compressed = zlib.compress(iccp_profile, 9)
        payload = b"icc\x00\x00" + compressed
        parts.append(chunk(b"iCCP", payload))
    if include_idat:
        payload = zlib.compress(raw, 9) if idat_payload is None else idat_payload
        parts.append(chunk(b"IDAT", payload))
    parts.append(chunk(b"IEND", b""))
    return b"".join(parts)



def write_minimal_png(path: Path, *, width: int = 8, height: int = 8) -> None:
    """Write a tiny valid RGB PNG using only the Python standard library."""
    path.write_bytes(build_rgb_png_bytes(width=width, height=height))


def _cli_json(proc: subprocess.CompletedProcess[str]) -> dict | None:
    """Backward-compatible raw JSON object parse (tests / diagnostics only)."""
    text = (proc.stdout or "").strip()
    if not text:
        return None
    try:
        payload = json.loads(text)
    except json.JSONDecodeError:
        return None
    return payload if isinstance(payload, dict) else None



# Probe-oriented budgets (fail closed before unbounded inflate/allocation).
_PROBE_PNG_MAX_FILE_BYTES = 64 * 1024 * 1024
_PROBE_PNG_MAX_DIMENSION = 16384
_PROBE_PNG_MAX_PIXELS = 64 * 1024 * 1024
_PROBE_PNG_MAX_INFLATE_BYTES = 256 * 1024 * 1024
_PROBE_PNG_MAX_ICC_BYTES = 16 * 1024 * 1024


def require_probe_artifact(probe_data: dict, probe_out: Path) -> tuple[str, int]:
    """Fail closed on the versioned nested artifact contract from catalog probe."""
    artifact = probe_data.get("artifact")
    if not isinstance(artifact, dict):
        raise ValueError("probe JSON missing nested artifact object")
    if artifact.get("type") != "ravo.image_artifact":
        raise ValueError(f"unexpected artifact type: {artifact.get('type')!r}")
    version = artifact.get("version")
    if version != 1 and version != "1":
        raise ValueError(f"unsupported artifact version: {version!r}")
    path_value = artifact.get("path")
    if not isinstance(path_value, str) or Path(path_value).resolve() != probe_out.resolve():
        raise ValueError("artifact.path must equal probe output path")
    if artifact.get("mime_type") != "image/png":
        raise ValueError(f"unexpected artifact mime_type: {artifact.get('mime_type')!r}")

    def _as_int(value: object) -> int | None:
        if isinstance(value, bool):
            return None
        if isinstance(value, int):
            return value
        if isinstance(value, str) and value.isdigit():
            return int(value)
        return None

    width = _as_int(artifact.get("width"))
    height = _as_int(artifact.get("height"))
    byte_count = _as_int(artifact.get("byte_count"))
    if width is None or height is None or width <= 0 or height <= 0:
        raise ValueError("artifact width/height invalid")
    if byte_count is None or byte_count <= 0:
        raise ValueError("artifact byte_count invalid")
    if byte_count > _PROBE_PNG_MAX_FILE_BYTES:
        raise ValueError("artifact byte_count exceeds probe budget")
    profile = artifact.get("color_profile")
    if not isinstance(profile, str) or not profile:
        raise ValueError("artifact color_profile missing")
    fingerprint = artifact.get("color_profile_fingerprint")
    if not isinstance(fingerprint, str) or not fingerprint:
        raise ValueError("artifact color_profile_fingerprint missing")
    digest = artifact.get("content_sha256")
    if not isinstance(digest, str) or len(digest) != 64 or any(
        ch not in "0123456789abcdef" for ch in digest.lower()
    ):
        raise ValueError("artifact content_sha256 invalid")
    top_w = _as_int(probe_data.get("width"))
    top_h = _as_int(probe_data.get("height"))
    if top_w != width or top_h != height:
        raise ValueError("top-level width/height disagree with artifact")
    file_digest, file_bytes = sha256_file_bounded(probe_out, max_bytes=_PROBE_PNG_MAX_FILE_BYTES)
    if file_bytes != byte_count:
        raise ValueError(f"probe file size {file_bytes} != artifact byte_count {byte_count}")
    if file_digest.lower() != digest.lower():
        raise ValueError("probe file sha256 disagrees with artifact content_sha256")
    if probe_out.stat().st_size != byte_count:
        raise ValueError("stat size disagrees with streamed byte count")
    return file_digest, file_bytes


# Mirror adapters/src/qt_raster_png.cpp cICP identity table.
_CICP_PROFILE_IDS: dict[tuple[int, int], str] = {
    (1, 13): "srgb",
    (1, 8): "linear_rec709",
    (1, 1): "rec709",
    (9, 8): "linear_rec2020",
    (9, 16): "pq_rec2020",
    (9, 18): "hlg_rec2020",
    (12, 13): "display_p3",
    (12, 16): "pq_p3",
    (12, 18): "hlg_p3",
}


def _is_ascii_path_byte(character: int) -> bool:
    return (
        (ord("A") <= character <= ord("Z"))
        or (ord("a") <= character <= ord("z"))
        or (ord("0") <= character <= ord("9"))
        or character in {ord("/"), ord("-"), ord("_"), ord("."), ord("~"), ord(":")}
    )


def percent_encode_path(path: str) -> str:
    """Mirror Ravo domain percent_encode_path for exact file URI equality."""
    out = bytearray()
    for byte in path.encode("utf-8"):
        if byte >= 0x80 or _is_ascii_path_byte(byte):
            out.append(byte)
        else:
            out.extend(f"%{byte:02X}".encode("ascii"))
    return out.decode("utf-8")


def expected_file_uri(path: Path) -> str:
    """Build the product file URI for an absolute local path (POSIX / Windows)."""
    resolved = path.resolve()
    generic = resolved.as_posix()
    if len(generic) >= 2 and generic[1] == ":":
        # Windows drive path already lacks a leading slash in as_posix sometimes;
        # pathlib on Windows yields like C:/...
        pass
    encoded = percent_encode_path(generic)
    if encoded.startswith("/"):
        return "file://" + encoded
    return "file:///" + encoded


def exact_catalog_asset_membership(
    assets: object, *, asset_id: str, asset_uri: str
) -> tuple[bool, str]:
    """Require exact id+uri membership with no extras/duplicates for a one-asset catalog."""
    if not isinstance(assets, list):
        return False, "list JSON missing assets array"
    matches = []
    for entry in assets:
        if not isinstance(entry, dict):
            return False, "assets entry is not an object"
        if entry.get("id") == asset_id and entry.get("uri") == asset_uri:
            matches.append(entry)
    if len(matches) != 1:
        return False, "reopen/list missing exact imported asset id/uri equality"
    if len(assets) != 1:
        return False, f"temporary catalog must contain exactly one asset, got {len(assets)}"
    return True, "ok"


def parse_cli_success_envelope(proc: subprocess.CompletedProcess[str]) -> dict | None:
    """Accept only the versioned ravo.cli.result success envelope with object data.

    Rejects bare data objects, wrong type/version, ok=false, and non-object data.
    """
    payload = _cli_json(proc)
    if payload is None:
        return None
    if payload.get("type") != "ravo.cli.result":
        return None
    version = payload.get("version")
    if version != 1 and version != "1":
        return None
    if payload.get("ok") is not True:
        return None
    data = payload.get("data")
    if not isinstance(data, dict):
        return None
    return data


def _run_cli(cli: Path, args: list[str], env: dict[str, str], timeout: int = 120) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(cli), *args],
        env=env,
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
    )


def run_catalog_workflow_stages(cli: Path, env: dict[str, str], work: Path,
                                record) -> None:
    """Exercise create/import/probe/reopen against real CLI JSON contracts.

    Missing host/simulator capability → UNTESTED. Product contract failure → FAIL.
    """
    stages = (
        "catalog_create_open",
        "catalog_synthetic_import",
        "catalog_probe_or_render",
        "catalog_reopen_hash",
    )
    catalog = work / "packaged-catalog" / "library.sqlite"
    catalog.parent.mkdir(parents=True, exist_ok=True)
    source_dir = work / "packaged-catalog" / "source"
    source_dir.mkdir(parents=True, exist_ok=True)
    png = source_dir / "synthetic.png"
    write_minimal_png(png)
    source_stat = png.stat()
    source_sha = sha256_file(png)

    try:
        create = _run_cli(cli, ["catalog", "create", "--path", str(catalog), "--json"], env)
    except Exception as exc:  # noqa: BLE001
        for name in stages:
            record(name, Status.UNTESTED, f"catalog create launch failed: {exc}")
        return
    create_data = parse_cli_success_envelope(create)
    if create.returncode != 0 or create_data is None:
        detail = ((create.stderr or create.stdout or "")[:240])
        if create.returncode != 0 and "QSQLITE" in (create.stderr or ""):
            for name in stages:
                record(name, Status.FAIL, f"missing QSQLITE: {detail}")
            return
        raw = _cli_json(create)
        if create.returncode == 0 and raw is not None and raw.get("ok") is False:
            record("catalog_create_open", Status.FAIL, "create envelope ok=false with exit 0")
            for name in stages[1:]:
                record(name, Status.FAIL, "catalog create rejected")
            return
        if create.returncode != 0 and create_data is None and not catalog.is_file():
            if create.returncode == 0:
                record("catalog_create_open", Status.FAIL, "create exited 0 without library.sqlite")
                for name in stages[1:]:
                    record(name, Status.FAIL, "catalog create did not produce a library")
                return
            for name in stages:
                record(name, Status.UNTESTED, f"catalog create unavailable: {detail}")
            return
        record("catalog_create_open", Status.FAIL, f"create failed or invalid envelope: {detail}")
        for name in stages[1:]:
            record(name, Status.FAIL, "catalog create failed")
        return
    if not catalog.is_file():
        record("catalog_create_open", Status.FAIL, "create reported success but library.sqlite missing")
        for name in stages[1:]:
            record(name, Status.FAIL, "catalog create did not produce a library")
        return
    catalog_id = create_data.get("catalog_id")
    if not isinstance(catalog_id, str) or not catalog_id:
        record("catalog_create_open", Status.FAIL, "create JSON missing catalog_id")
        for name in stages[1:]:
            record(name, Status.FAIL, "invalid create schema")
        return
    # Separate process reopen/list proves the catalog identity is durable.
    try:
        listed_after_create = _run_cli(cli, ["catalog", "list", "--catalog", str(catalog), "--json"], env)
    except Exception as exc:  # noqa: BLE001
        record("catalog_create_open", Status.FAIL, f"list after create failed: {exc}")
        for name in stages[1:]:
            record(name, Status.FAIL, "catalog reopen unavailable")
        return
    list_after_data = parse_cli_success_envelope(listed_after_create)
    if listed_after_create.returncode != 0 or list_after_data is None:
        record("catalog_create_open", Status.FAIL, "list after create rejected envelope")
        for name in stages[1:]:
            record(name, Status.FAIL, "catalog reopen failed")
        return
    record("catalog_create_open", Status.PASS, f"catalog_id={catalog_id}")

    try:
        imported = _run_cli(
            cli,
            ["catalog", "import", "--catalog", str(catalog), "--input", str(png), "--json"],
            env,
            timeout=180,
        )
    except Exception as exc:  # noqa: BLE001
        record("catalog_synthetic_import", Status.FAIL, str(exc))
        for name in stages[2:]:
            record(name, Status.FAIL, "import did not run")
        return
    import_data = parse_cli_success_envelope(imported)
    if imported.returncode != 0 or import_data is None:
        record("catalog_synthetic_import", Status.FAIL,
               ((imported.stderr or imported.stdout or "")[:240]))
        for name in stages[2:]:
            record(name, Status.FAIL, "import failed")
        return
    items = import_data.get("items")
    imported_count = import_data.get("imported")
    failed_count = import_data.get("failed")
    if not isinstance(items, list) or len(items) != 1:
        record("catalog_synthetic_import", Status.FAIL, "import JSON items must be exactly one")
        for name in stages[2:]:
            record(name, Status.FAIL, "import schema invalid")
        return
    if imported_count != 1 or failed_count != 0:
        record("catalog_synthetic_import", Status.FAIL,
               f"import counts imported={imported_count!r} failed={failed_count!r}")
        for name in stages[2:]:
            record(name, Status.FAIL, "import counts invalid")
        return
    item0 = items[0]
    if not isinstance(item0, dict) or item0.get("status") != "imported":
        record("catalog_synthetic_import", Status.FAIL, "import item status not imported")
        for name in stages[2:]:
            record(name, Status.FAIL, "import status invalid")
        return
    asset = item0.get("asset") if isinstance(item0, dict) else None
    asset_id = asset.get("id") if isinstance(asset, dict) else None
    asset_uri = asset.get("uri") if isinstance(asset, dict) else None
    if not isinstance(asset_id, str) or not asset_id:
        record("catalog_synthetic_import", Status.FAIL, "import JSON missing asset id")
        for name in stages[2:]:
            record(name, Status.FAIL, "import missing asset id")
        return
    if not isinstance(asset_uri, str) or not asset_uri:
        record("catalog_synthetic_import", Status.FAIL, "import JSON missing asset uri")
        for name in stages[2:]:
            record(name, Status.FAIL, "import missing asset uri")
        return
    expected_uri = expected_file_uri(png)
    if asset_uri != expected_uri:
        record("catalog_synthetic_import", Status.FAIL,
               f"import uri {asset_uri!r} != expected {expected_uri!r}")
        for name in stages[2:]:
            record(name, Status.FAIL, "import uri mismatch")
        return
    if sha256_file(png) != source_sha or png.stat().st_size != source_stat.st_size:
        record("catalog_synthetic_import", Status.FAIL, "import mutated source png hash/size")
        for name in stages[2:]:
            record(name, Status.FAIL, "source mutated")
        return
    if png.stat().st_mtime_ns != source_stat.st_mtime_ns:
        record("catalog_synthetic_import", Status.FAIL, "import mutated source mtime")
        for name in stages[2:]:
            record(name, Status.FAIL, "source mtime mutated")
        return
    record("catalog_synthetic_import", Status.PASS, f"asset_id={asset_id}")

    probe_out = work / "packaged-catalog" / "probe.png"
    try:
        probed = _run_cli(
            cli,
            [
                "catalog", "probe", "--catalog", str(catalog), "--asset-id", str(asset_id),
                "--output", str(probe_out), "--json",
            ],
            env,
            timeout=180,
        )
    except Exception as exc:  # noqa: BLE001
        record("catalog_probe_or_render", Status.FAIL, str(exc))
        record("catalog_reopen_hash", Status.FAIL, "probe did not run")
        return
    probe_data = parse_cli_success_envelope(probed)
    if probed.returncode != 0 or probe_data is None or not probe_out.is_file() or probe_out.stat().st_size <= 0:
        record("catalog_probe_or_render", Status.FAIL,
               ((probed.stderr or probed.stdout or "")[:240]))
        record("catalog_reopen_hash", Status.FAIL, "probe failed")
        return
    if probe_data.get("asset_id") != asset_id:
        record("catalog_probe_or_render", Status.FAIL, "probe asset_id mismatch")
        record("catalog_reopen_hash", Status.FAIL, "probe identity invalid")
        return
    try:
        probe_sha, _probe_bytes = require_probe_artifact(probe_data, probe_out)
    except ValueError as exc:
        record("catalog_probe_or_render", Status.FAIL, f"probe artifact contract failed: {exc}")
        record("catalog_reopen_hash", Status.FAIL, "probe artifact invalid")
        return
    if sha256_file(png) != source_sha or png.stat().st_mtime_ns != source_stat.st_mtime_ns:
        record("catalog_probe_or_render", Status.FAIL, "probe mutated source bytes/mtime")
        record("catalog_reopen_hash", Status.FAIL, "source mutated during probe")
        return
    record("catalog_probe_or_render", Status.PASS, f"{probe_out};sha256={probe_sha}")

    try:
        listed = _run_cli(cli, ["catalog", "list", "--catalog", str(catalog), "--json"], env)
    except Exception as exc:  # noqa: BLE001
        record("catalog_reopen_hash", Status.FAIL, str(exc))
        return
    list_data = parse_cli_success_envelope(listed)
    if listed.returncode != 0 or list_data is None:
        record("catalog_reopen_hash", Status.FAIL, ((listed.stderr or listed.stdout or "")[:240]))
        return
    assets = list_data.get("assets")
    ok, detail = exact_catalog_asset_membership(assets, asset_id=asset_id, asset_uri=asset_uri)
    if not ok:
        record("catalog_reopen_hash", Status.FAIL, detail)
        return
    # Defense in depth: reopen URI must also equal the expected source URI.
    if asset_uri != expected_file_uri(png):
        record("catalog_reopen_hash", Status.FAIL, "reopen uri drifted from expected source uri")
        return
    if sha256_file(png) != source_sha or png.stat().st_size != source_stat.st_size:
        record("catalog_reopen_hash", Status.FAIL, "source hash/size changed after reopen")
        return
    if png.stat().st_mtime_ns != source_stat.st_mtime_ns:
        record("catalog_reopen_hash", Status.FAIL, "source mtime changed after reopen")
        return
    record("catalog_reopen_hash", Status.PASS, f"asset_id={asset_id}")



def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact", type=Path)
    parser.add_argument("--workdir", type=Path, default=None)
    parser.add_argument("--require-smoke", action="store_true")
    parser.add_argument("--repo-root", type=Path, default=None,
                        help="Reject workdirs inside this build/source tree")
    parser.add_argument("--evidence-json", type=Path, default=None)
    parser.add_argument(
        "--artifact-basename",
        default=None,
        help="Archive basename written into evidence identity (defaults to artifact name)",
    )
    parser.add_argument("--source-sha", default=None, help="Source commit SHA for evidence pairing")
    parser.add_argument("--run-id", default=None, help="CI run id for evidence pairing")
    parser.add_argument("--run-attempt", default=None, help="CI run attempt for evidence pairing")
    args = parser.parse_args(argv)
    artifact = args.artifact.resolve()
    results: dict[str, str] = {}
    residuals: list[str] = []
    digest = ""
    artifact_basename = args.artifact_basename or args.artifact.name

    def record(name: str, status: Status, detail: str = "") -> None:
        results[name] = status.value
        line = f"{status.value}: {name}" + (f" ({detail})" if detail else "")
        try:
            print(line)
        except UnicodeEncodeError:
            # Windows CI consoles are often cp1252; keep evidence write path alive.
            print(line.encode("ascii", "replace").decode("ascii"))
        if status == Status.UNTESTED:
            residuals.append(line)

    def write_evidence(*, reason: str | None = None, status_override: str | None = None) -> None:
        if not args.evidence_json:
            return
        from packaged_evidence_schema import build_evidence_payload

        early_fail = not bool(digest)
        payload = build_evidence_payload(
            artifact_basename=artifact_basename,
            digest_sha256=digest if digest else ("0" * 64),
            results=results,
            residuals=residuals,
            require_smoke=bool(args.require_smoke),
            host={
                "platform": sys.platform,
                "python": sys.version.split()[0],
            },
            cli=str(cli) if cli is not None else None,
            studio=str(studio) if studio is not None else None,
            source_sha=args.source_sha,
            run_id=args.run_id,
            run_attempt=args.run_attempt,
            reason=reason,
            status_override=("FAIL" if early_fail else status_override),
        )
        if early_fail:
            payload.pop("digest_sha256", None)
            if reason is None and "reason" not in payload:
                payload["reason"] = "artifact missing or digest unavailable"
        args.evidence_json.parent.mkdir(parents=True, exist_ok=True)
        args.evidence_json.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    cli = None
    studio = None

    if not artifact.is_file():
        print(f"ERROR: artifact missing: {artifact}", file=sys.stderr)
        write_evidence(reason=f"artifact missing: {artifact}", status_override="FAIL")
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
            write_evidence(reason=f"workdir is not empty: {work}", status_override="FAIL")
            return 1
        work.mkdir(parents=True, exist_ok=True)
        repo = args.repo_root.resolve() if args.repo_root else None
        if repo is not None and is_under(work, repo):
            print(f"ERROR: workdir is inside repo/build tree: {work}", file=sys.stderr)
            write_evidence(reason=f"workdir inside repo/build tree: {work}", status_override="FAIL")
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
               "offscreen smoke != native packaged plugins/session")
        if artifact.suffix.lower() == ".deb":
            record("dpkg_install_launcher", Status.UNTESTED, "unpack != install")
        if ".AppImage" in artifact.name:
            # Extract path is exercised by unpack(); FUSE direct launch remains separate.
            if results.get("unpack") == Status.PASS.value:
                record("appimage_extract_payload", Status.PASS, "via --appimage-extract")
            else:
                record("appimage_extract_payload", Status.FAIL, "unpack did not PASS")
            record("appimage_fuse_direct_launch", Status.UNTESTED, "no FUSE host evidence")

        # Fail closed: --require-smoke forbids any FAIL and forbids missing required PASS.
        required = (
            "unpack",
            "cli_payload",
            "cli_help",
            "studio_payload",
            "studio_smoke",
            "catalog_create_open",
            "catalog_synthetic_import",
            "catalog_probe_or_render",
            "catalog_reopen_hash",
        )
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
        write_evidence()
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
