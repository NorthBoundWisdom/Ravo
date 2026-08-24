#!/usr/bin/env python3
# Usage:
#   python3 Ravo/desktop/icons/build_app_icons.py
#   python3 Ravo/desktop/icons/build_app_icons.py --source Ravo/desktop/icons/AppIcon-artwork.png

"""Build padded Ravo Studio app icons from full-bleed artwork."""

from __future__ import annotations

import argparse
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

# Apple's macOS app-icon grid is 824x824 on a 1024 canvas (100px inset).
CANVAS = 1024
ARTWORK = 824
ICONSET_SPECS = (
    ("icon_16x16.png", 16),
    ("icon_16x16@2x.png", 32),
    ("icon_32x32.png", 32),
    ("icon_32x32@2x.png", 64),
    ("icon_128x128.png", 128),
    ("icon_128x128@2x.png", 256),
    ("icon_256x256.png", 256),
    ("icon_256x256@2x.png", 512),
    ("icon_512x512.png", 512),
    ("icon_512x512@2x.png", 1024),
)
ICO_SIZES = (16, 32, 48, 256)


def run(command: list[str]) -> None:
    completed = subprocess.run(command, check=False, capture_output=True, text=True)
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout or "").strip()
        raise RuntimeError(f"{' '.join(command)} failed: {detail}")


def read_png_rgba(path: Path) -> tuple[int, int, list[bytearray]]:
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"not a PNG: {path}")
    pos = 8
    width = height = 0
    color = 6
    raw = b""
    while pos + 8 <= len(data):
        length, ctype = struct.unpack(">I4s", data[pos : pos + 8])
        chunk = data[pos + 8 : pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            width, height, bit, color, _comp, _filt, inter = struct.unpack(">IIBBBBB", chunk)
            if bit != 8 or color not in {2, 6} or inter != 0:
                raise ValueError(f"unsupported PNG {path}: bit={bit} color={color} inter={inter}")
        elif ctype == b"IDAT":
            raw += chunk
        elif ctype == b"IEND":
            break
    raw = zlib.decompress(raw)
    bpp = 4 if color == 6 else 3
    stride = width * bpp
    rows: list[bytearray] = []
    prev = bytearray(stride)
    offset = 0
    for _ in range(height):
        filt = raw[offset]
        offset += 1
        row = bytearray(raw[offset : offset + stride])
        offset += stride
        if filt == 1:
            for x in range(stride):
                row[x] = (row[x] + (row[x - bpp] if x >= bpp else 0)) & 255
        elif filt == 2:
            for x in range(stride):
                row[x] = (row[x] + prev[x]) & 255
        elif filt == 3:
            for x in range(stride):
                left = row[x - bpp] if x >= bpp else 0
                row[x] = (row[x] + ((left + prev[x]) // 2)) & 255
        elif filt == 4:
            for x in range(stride):
                a = row[x - bpp] if x >= bpp else 0
                b = prev[x]
                c = prev[x - bpp] if x >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                row[x] = (row[x] + pred) & 255
        elif filt != 0:
            raise ValueError(f"unsupported PNG filter {filt} in {path}")
        if bpp == 3:
            rgba = bytearray(width * 4)
            for x in range(width):
                rgba[x * 4 : x * 4 + 3] = row[x * 3 : x * 3 + 3]
                rgba[x * 4 + 3] = 255
            row = rgba
        rows.append(row)
        prev = row
    return width, height, rows


def write_png_rgba(path: Path, width: int, height: int, rows: list[bytearray]) -> None:
    raw = bytearray()
    for row in rows:
        raw.append(0)
        raw.extend(row)
    compressed = zlib.compress(bytes(raw), 9)
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)

    def chunk(tag: bytes, payload: bytes) -> bytes:
        crc = zlib.crc32(tag + payload) & 0xFFFFFFFF
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", crc)

    path.write_bytes(
        b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", compressed) + chunk(b"IEND", b"")
    )


def pad_to_canvas(source: Path, destination: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="ravo-icon-") as tempdir:
        resized = Path(tempdir) / "artwork.png"
        run(["sips", "-z", str(ARTWORK), str(ARTWORK), str(source), "--out", str(resized)])
        width, height, rows = read_png_rgba(resized)
        if width != ARTWORK or height != ARTWORK:
            raise RuntimeError(f"sips wrote {width}x{height}, expected {ARTWORK}x{ARTWORK}")
        inset = (CANVAS - ARTWORK) // 2
        canvas = [bytearray(CANVAS * 4) for _ in range(CANVAS)]
        for y, row in enumerate(rows):
            canvas[y + inset][inset * 4 : (inset + ARTWORK) * 4] = row
        write_png_rgba(destination, CANVAS, CANVAS, canvas)


def write_ico(destination: Path, png_by_size: dict[int, bytes]) -> None:
    sizes = [size for size in ICO_SIZES if size in png_by_size]
    offset = 6 + 16 * len(sizes)
    entries = bytearray()
    payload = bytearray()
    for size in sizes:
        data = png_by_size[size]
        entries.extend(
            struct.pack(
                "<BBBBHHII",
                size if size < 256 else 0,
                size if size < 256 else 0,
                0,
                0,
                1,
                32,
                len(data),
                offset,
            )
        )
        payload.extend(data)
        offset += len(data)
    destination.write_bytes(struct.pack("<HHH", 0, 1, len(sizes)) + entries + payload)


def build_icons(source: Path, output_dir: Path) -> None:
    if not source.is_file():
        raise FileNotFoundError(source)
    output_dir.mkdir(parents=True, exist_ok=True)
    png_path = output_dir / "AppIcon.png"
    icns_path = output_dir / "AppIcon.icns"
    ico_path = output_dir / "AppIcon.ico"

    with tempfile.TemporaryDirectory(prefix="ravo-iconset-") as tempdir:
        padded = Path(tempdir) / "padded.png"
        pad_to_canvas(source, padded)
        shutil.copy2(padded, png_path)

        iconset = Path(tempdir) / "AppIcon.iconset"
        iconset.mkdir()
        png_by_size: dict[int, bytes] = {}
        for name, size in ICONSET_SPECS:
            target = iconset / name
            run(["sips", "-z", str(size), str(size), str(padded), "--out", str(target)])
            png_by_size[size] = target.read_bytes()
        run(["iconutil", "-c", "icns", "-o", str(icns_path), str(iconset)])

        ico_pngs: dict[int, bytes] = {}
        for size in ICO_SIZES:
            if size in png_by_size:
                ico_pngs[size] = png_by_size[size]
                continue
            sized = Path(tempdir) / f"ico-{size}.png"
            run(["sips", "-z", str(size), str(size), str(padded), "--out", str(sized)])
            ico_pngs[size] = sized.read_bytes()
        write_ico(ico_path, ico_pngs)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    icons_dir = Path(__file__).resolve().parent
    parser.add_argument(
        "--source",
        type=Path,
        default=icons_dir / "AppIcon-artwork.png",
        help="Full-bleed 1024x1024 artwork. The script applies the macOS 824 icon grid.",
    )
    parser.add_argument("--output-dir", type=Path, default=icons_dir)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(sys.argv[1:] if argv is None else argv)
    try:
        build_icons(args.source.expanduser().resolve(), args.output_dir.expanduser().resolve())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(f"wrote {args.output_dir / 'AppIcon.png'}")
    print(f"wrote {args.output_dir / 'AppIcon.icns'}")
    print(f"wrote {args.output_dir / 'AppIcon.ico'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
