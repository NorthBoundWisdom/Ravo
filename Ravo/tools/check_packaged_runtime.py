#!/usr/bin/env python3
"""Validate a packaged Ravo Studio artifact outside the build tree.

Result vocabulary: PASS / FAIL / UNTESTED / NOT_APPLICABLE.
With --require-smoke, missing required stages fail closed (no UNTESTED success).
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
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



def write_minimal_png(path: Path, *, width: int = 8, height: int = 8) -> None:
    """Write a tiny valid RGB PNG using only the Python standard library."""
    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    rows = []
    for y in range(height):
        row = bytearray([0])
        for x in range(width):
            row.extend([((x + y) * 17) % 256, 40, 80])
        rows.append(bytes(row))
    raw = b"".join(rows)
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"sRGB", bytes([0]))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


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



@dataclass(frozen=True)
class DecodedPng:
    """Fully decoded PNG pixels plus color-chunk identity for packaged evidence checks.

    This is a packaging-acceptance decoder (stdlib zlib + PNG filters), not a product
    image algorithm. Product decode ownership remains in the Qt raster PNG adapter.
    """

    width: int
    height: int
    pixels: bytes  # RGB888, width*height*3
    color_profile_id: str | None
    bit_depth: int
    color_type: int


def _paeth_predictor(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def _reconstruct_png_scanlines(raw: bytes, width: int, height: int, bpp: int) -> bytes:
    stride = width * bpp
    expected = (stride + 1) * height
    if len(raw) < expected:
        raise ValueError("truncated PNG image data")
    if len(raw) != expected:
        # Extra trailing bytes after a complete image are not accepted for probe artifacts.
        raise ValueError("PNG image data length mismatch")
    out = bytearray(stride * height)
    prev = bytearray(stride)
    offset = 0
    for row in range(height):
        filter_type = raw[offset]
        offset += 1
        cur = bytearray(raw[offset : offset + stride])
        offset += stride
        if filter_type == 0:
            pass
        elif filter_type == 1:
            for i in range(stride):
                left = cur[i - bpp] if i >= bpp else 0
                cur[i] = (cur[i] + left) & 0xFF
        elif filter_type == 2:
            for i in range(stride):
                cur[i] = (cur[i] + prev[i]) & 0xFF
        elif filter_type == 3:
            for i in range(stride):
                left = cur[i - bpp] if i >= bpp else 0
                cur[i] = (cur[i] + ((left + prev[i]) // 2)) & 0xFF
        elif filter_type == 4:
            for i in range(stride):
                left = cur[i - bpp] if i >= bpp else 0
                up = prev[i]
                up_left = prev[i - bpp] if i >= bpp else 0
                cur[i] = (cur[i] + _paeth_predictor(left, up, up_left)) & 0xFF
        else:
            raise ValueError(f"unsupported PNG filter type {filter_type}")
        out[row * stride : (row + 1) * stride] = cur
        prev = cur
    return bytes(out)


def decode_png_pixels(path: Path) -> DecodedPng:
    """Fully decode a PNG by inflating IDAT and reconstructing pixels.

    Rejects signature-only payloads, missing IDAT (including the 45-byte
    signature+IHDR+IEND shape), truncated/corrupt zlib streams, bad CRCs, and
    unsupported probe color types. Returns RGB888 pixels and a color-profile
    identity derived from sRGB/iCCP chunks when present.
    """
    data = path.read_bytes()
    if len(data) < 8 or data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos = 8
    width = height = bit_depth = color_type = None
    idat = bytearray()
    saw_iend = False
    color_profile_id: str | None = None
    while pos + 12 <= len(data):
        length = int.from_bytes(data[pos : pos + 4], "big")
        pos += 4
        if pos + 4 + length + 4 > len(data):
            raise ValueError("truncated PNG chunk")
        tag = data[pos : pos + 4]
        pos += 4
        chunk = data[pos : pos + length]
        pos += length
        crc_expected = int.from_bytes(data[pos : pos + 4], "big")
        pos += 4
        crc_actual = zlib.crc32(tag + chunk) & 0xFFFFFFFF
        if crc_actual != crc_expected:
            raise ValueError(f"PNG chunk CRC mismatch for {tag!r}")
        if tag == b"IHDR":
            if width is not None:
                raise ValueError("duplicate IHDR")
            if length < 13:
                raise ValueError("truncated IHDR")
            width = int.from_bytes(chunk[0:4], "big")
            height = int.from_bytes(chunk[4:8], "big")
            bit_depth = chunk[8]
            color_type = chunk[9]
            if width <= 0 or height <= 0:
                raise ValueError("non-positive PNG dimensions")
            if bit_depth != 8 or color_type not in (2, 6):
                raise ValueError(f"unsupported PNG format depth={bit_depth} type={color_type}")
        elif tag == b"IDAT":
            if width is None:
                raise ValueError("IDAT before IHDR")
            idat.extend(chunk)
        elif tag == b"IEND":
            saw_iend = True
            break
        elif tag == b"sRGB":
            if length < 1:
                raise ValueError("malformed sRGB chunk")
            color_profile_id = "srgb"
        elif tag == b"iCCP":
            color_profile_id = "embedded_icc"
        # ignore other ancillary chunks
    if width is None or height is None or bit_depth is None or color_type is None:
        raise ValueError("missing or truncated IHDR")
    if not idat:
        raise ValueError("PNG missing IDAT (no pixel data)")
    if not saw_iend:
        raise ValueError("PNG missing IEND")
    try:
        raw = zlib.decompress(bytes(idat))
    except zlib.error as exc:
        raise ValueError(f"PNG IDAT inflate failed: {exc}") from exc
    bpp = 3 if color_type == 2 else 4
    reconstructed = _reconstruct_png_scanlines(raw, width, height, bpp)
    if color_type == 6:
        rgb = bytearray(width * height * 3)
        for i in range(width * height):
            rgb[i * 3 : i * 3 + 3] = reconstructed[i * 4 : i * 4 + 3]
        pixels = bytes(rgb)
    else:
        pixels = reconstructed
    return DecodedPng(
        width=width,
        height=height,
        pixels=pixels,
        color_profile_id=color_profile_id,
        bit_depth=bit_depth,
        color_type=color_type,
    )


def read_png_ihdr(path: Path) -> tuple[int, int]:
    """Compatibility wrapper: full decode then return dimensions."""
    decoded = decode_png_pixels(path)
    return decoded.width, decoded.height


def supported_probe_color_profile(value: object) -> bool:
    if not isinstance(value, str):
        return False
    return value.casefold() in {"srgb", "srgb-linear", "display-p3", "embedded_icc"}


def probe_profile_matches_artifact(json_profile: object, artifact_profile_id: str | None) -> bool:
    """Require JSON color_profile to correspond to PNG color chunks when present."""
    if not isinstance(json_profile, str) or not json_profile:
        return False
    json_id = json_profile.casefold()
    if not supported_probe_color_profile(json_id):
        return False
    if artifact_profile_id is None:
        # Encoder may omit an explicit chunk for the builtin sRGB path; accept only srgb*.
        return json_id in {"srgb", "srgb-linear"}
    art = artifact_profile_id.casefold()
    if art == "srgb":
        return json_id in {"srgb", "srgb-linear"}
    if art == "embedded_icc":
        return json_id == "embedded_icc"
    return json_id == art


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
    try:
        decoded = decode_png_pixels(probe_out)
    except ValueError as exc:
        record("catalog_probe_or_render", Status.FAIL, f"probe PNG decode failed: {exc}")
        record("catalog_reopen_hash", Status.FAIL, "probe artifact invalid")
        return
    if len(decoded.pixels) != decoded.width * decoded.height * 3:
        record("catalog_probe_or_render", Status.FAIL, "probe pixel buffer size mismatch")
        record("catalog_reopen_hash", Status.FAIL, "probe artifact invalid")
        return
    json_w = probe_data.get("width")
    json_h = probe_data.get("height")
    # CLI may emit width/height as JSON numbers or numeric strings.
    def _as_int(value: object) -> int | None:
        if isinstance(value, bool):
            return None
        if isinstance(value, int):
            return value
        if isinstance(value, str) and value.isdigit():
            return int(value)
        return None
    iw, ih = _as_int(json_w), _as_int(json_h)
    if iw != decoded.width or ih != decoded.height:
        record("catalog_probe_or_render", Status.FAIL,
               f"probe JSON size {json_w}x{json_h} != decoded {decoded.width}x{decoded.height}")
        record("catalog_reopen_hash", Status.FAIL, "probe dimensions mismatch")
        return
    if not probe_profile_matches_artifact(probe_data.get("color_profile"), decoded.color_profile_id):
        record("catalog_probe_or_render", Status.FAIL,
               f"color_profile JSON {probe_data.get('color_profile')!r} != artifact {decoded.color_profile_id!r}")
        record("catalog_reopen_hash", Status.FAIL, "probe profile invalid")
        return
    if probe_data.get("asset_id") != asset_id:
        record("catalog_probe_or_render", Status.FAIL, "probe asset_id mismatch")
        record("catalog_reopen_hash", Status.FAIL, "probe identity invalid")
        return
    # Local evidence digest only — protocol has no content_hash field to compare.
    probe_sha = sha256_file(probe_out)
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
