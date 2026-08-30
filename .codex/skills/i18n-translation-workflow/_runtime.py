#!/usr/bin/env python3
"""Runtime for Ravo's desktop i18n workflow."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path
from typing import Iterable


def find_repo_root() -> Path:
    """Find repository root by searching marker files from current path upward."""
    current = Path(__file__).resolve()
    for parent in [current, *current.parents]:
        if (parent / "AGENTS.md").is_file() and (parent / "Ravo" / "desktop").is_dir():
            return parent
    raise RuntimeError("未能定位 Ravo 仓库根目录。请在仓库内运行或传入 --repo-root。")


def workflow_script_path(repo_root: Path, script_name: str) -> Path:
    del repo_root
    return Path(__file__).resolve().parent / script_name


def run_legacy_script(
    script_name: str, args: Iterable[str] | None = None, repo_root: Path | None = None
) -> int:
    """Run skill script through Python subprocess."""
    repo_root = repo_root or find_repo_root()
    script_path = workflow_script_path(repo_root, script_name)

    if not script_path.exists():
        print(f"[ERROR] 找不到脚本: {script_path}")
        return 1

    cmd = [sys.executable, str(script_path)]
    if args:
        cmd.extend(args)

    env = os.environ.copy()
    env.setdefault("PYTHONUTF8", "1")
    result = subprocess.run(cmd, cwd=str(script_path.parent), env=env)
    return int(result.returncode)
