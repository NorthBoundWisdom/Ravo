#!/usr/bin/env python3
"""Emit a C++ translation unit that embeds a binary blob as a byte array."""

from __future__ import annotations

import argparse
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--symbol", required=True)
    args = parser.parse_args()
    data = args.input.read_bytes()
    lines = [
        f"extern unsigned char const {args.symbol}[];",
        f"unsigned char const {args.symbol}[] = {{",
    ]
    if data:
        for offset in range(0, len(data), 16):
            chunk = data[offset : offset + 16]
            lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in chunk) + ",")
    else:
        lines.append("    0,")
    lines.append("};")
    lines.append(f"extern unsigned long long const {args.symbol}_size;")
    lines.append(f"unsigned long long const {args.symbol}_size = {len(data)}ULL;")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
