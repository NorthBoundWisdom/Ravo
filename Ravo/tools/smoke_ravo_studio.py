#!/usr/bin/env python3
"""Launch Ravo Studio offscreen for one or more UI locales."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary")
    parser.add_argument("--language", action="append", default=[])
    args = parser.parse_args()
    env = os.environ.copy()
    # Headless CI and POST_BUILD must not require a display or GPU.
    env["QT_QPA_PLATFORM"] = "offscreen"
    env.setdefault("QSG_RHI_BACKEND", "software")
    env.setdefault("QT_QUICK_BACKEND", "software")
    # Windows Debug instantiates the same complete root QML tree but is much
    # slower under the hosted runner. Fatal Qt messages still exit immediately
    # through main.cpp's smoke-only message handler.
    timeout_seconds = 300 if os.name == "nt" else 90
    languages: list[str | None] = args.language or [None]
    for language in languages:
        command = [args.binary, "--smoke"]
        if language is not None:
            command.extend(("--language", language))
        try:
            completed = subprocess.run(command, env=env, check=False, timeout=timeout_seconds)
        except subprocess.TimeoutExpired:
            print(
                f"Ravo Studio smoke timed out after {timeout_seconds}s: {' '.join(command)}",
                file=sys.stderr,
            )
            return 1
        except OSError as error:
            print(f"Ravo Studio smoke failed to start {args.binary}: {error}", file=sys.stderr)
            return 1
        if completed.returncode != 0:
            print(
                f"Ravo Studio smoke failed: {' '.join(command)} exited {completed.returncode}",
                file=sys.stderr,
            )
            return completed.returncode
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
