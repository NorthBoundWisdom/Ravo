#!/usr/bin/env python3
"""Extract every manifest-owned Ravo Studio Qt translation catalog."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess

from _locales import load_locales


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--lupdate", type=Path)
    args = parser.parse_args()
    repo_root = args.repo_root.resolve()
    configured = args.lupdate or (Path(os.environ["LUPDATE"]) if "LUPDATE" in os.environ else None)
    executable = str(configured) if configured else shutil.which("lupdate")
    if not executable:
        raise SystemExit("lupdate was not found; pass --lupdate or set LUPDATE")

    desktop = repo_root / "Ravo" / "desktop"
    ts_paths = [locale.ts_path(repo_root) for locale in load_locales(repo_root)]
    command = [executable, "-silent", "-locations", "none", "-no-obsolete",
               str(desktop / "src"), str(desktop / "qml"), "-ts",
               *(str(path) for path in ts_paths)]
    subprocess.run(command, check=True, cwd=repo_root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
