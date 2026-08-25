#!/usr/bin/env python3
"""Launch Ravo Studio, confirm QML loaded, then exit.

Used as a ravo_studio POST_BUILD smoke check. The binary must accept --smoke.
"""

from __future__ import annotations

import os
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: smoke_ravo_studio.py <ravo_studio_binary>", file=sys.stderr)
        return 2
    binary = sys.argv[1]
    env = os.environ.copy()
    env.setdefault("QT_QPA_PLATFORM", "offscreen")
    try:
        completed = subprocess.run(
            (binary, "--smoke"),
            env=env,
            check=False,
            timeout=30,
        )
    except subprocess.TimeoutExpired:
        print(f"Ravo Studio smoke timed out: {binary}", file=sys.stderr)
        return 1
    except OSError as error:
        print(f"Ravo Studio smoke failed to start {binary}: {error}", file=sys.stderr)
        return 1
    if completed.returncode != 0:
        print(
            f"Ravo Studio smoke failed: {binary} --smoke exited {completed.returncode}",
            file=sys.stderr,
        )
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
