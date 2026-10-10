#!/usr/bin/env python3
"""Verify CI compilation used the cache directory that Actions restores/saves."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


def read_cache(path: Path) -> dict[str, str]:
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith(("#", "//")) or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.split(":", 1)[0]] = value
    return values


def parse_stats(text: str) -> dict[str, int]:
    stats = {}
    for line in text.splitlines():
        key, value = line.split("\t", 1)
        stats[key] = int(value)
    return stats


def verify(
    expected: Path, actual: str, cache: dict[str, str], stats: dict[str, int]
) -> dict:
    if expected.resolve() != Path(actual).resolve():
        raise ValueError(f"ccache directory mismatch: build={actual}, saved={expected}")
    for language in ("C", "CXX"):
        key = f"CMAKE_{language}_COMPILER_LAUNCHER"
        launcher = cache.get(key, "").split(";", 1)[0].replace("\\", "/")
        if Path(launcher).name.lower() not in ("ccache", "ccache.exe"):
            raise ValueError(f"{key} must invoke ccache, got {launcher!r}")
    counters = [
        stats[key]
        for key in ("direct_cache_hit", "preprocessed_cache_hit", "cache_miss")
    ]
    if any(value < 0 for value in counters) or sum(counters) == 0:
        raise ValueError(
            "ccache saw no cacheable compilations; inspect launcher and uncacheable reasons"
        )
    return {
        "type": "ravo.ci.ccache",
        "version": 1,
        "cache_dir": str(expected.resolve()),
        "cacheable_calls": sum(counters),
        "direct_hits": counters[0],
        "preprocessed_hits": counters[1],
        "misses": counters[2],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    args = parser.parse_args()
    try:
        expected = os.environ.get("CCACHE_DIR")
        if not expected:
            raise ValueError("CCACHE_DIR must be exported to the entire CI job")
        subprocess.run(["ccache", "--show-stats", "--verbose"], check=True)
        actual = subprocess.check_output(
            ["ccache", "--get-config", "cache_dir"], text=True
        ).strip()
        stats = parse_stats(
            subprocess.check_output(["ccache", "--print-stats"], text=True)
        )
        report = verify(
            Path(expected), actual, read_cache(args.build_dir / "CMakeCache.txt"), stats
        )
        print(json.dumps(report, sort_keys=True))
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"CI compiler cache validation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
