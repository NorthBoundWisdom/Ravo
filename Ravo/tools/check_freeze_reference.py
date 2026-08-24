#!/usr/bin/env python3
"""Verify that Ravo development has not changed the frozen 0.9 source or fixtures."""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


DEFAULT_FREEZE_COMMIT = "320970bf7c9cbbc6611cfc3eb60f8f2b0424b782"
# Current path -> path at the freeze commit. The 0.9 application sources were
# renamed from src/ to legacy-0.9/; the freeze object identity is still src/.
PROTECTED_PATHS: tuple[tuple[str, str], ...] = (
    ("legacy-0.9", "src"),
    ("darktable-tests", "darktable-tests"),
    ("cmake", "cmake"),
    ("data", "data"),
    ("packaging", "packaging"),
)


class FreezeCheckError(Exception):
    """Raised when a protected path differs from the recorded freeze reference."""


def run_git(repository_root: Path, *arguments: str) -> str:
    completed = subprocess.run(
        ("git", *arguments),
        cwd=repository_root,
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise FreezeCheckError(f"git {' '.join(arguments)} failed: {detail}")
    return completed.stdout.strip()


def verify(repository_root: Path, freeze_commit: str) -> dict[str, str]:
    freeze = run_git(repository_root, "rev-parse", f"{freeze_commit}^{{commit}}")
    head = run_git(repository_root, "rev-parse", "HEAD^{commit}")
    run_git(repository_root, "merge-base", "--is-ancestor", freeze, head)

    trees: dict[str, str] = {}
    current_paths: list[str] = []
    for current_path, freeze_path in PROTECTED_PATHS:
        frozen_tree = run_git(repository_root, "rev-parse", f"{freeze}:{freeze_path}")
        current_tree = run_git(repository_root, "rev-parse", f"HEAD:{current_path}")
        if frozen_tree != current_tree:
            raise FreezeCheckError(
                f"committed {current_path} object differs from freeze path {freeze_path}: "
                f"{frozen_tree} != {current_tree}"
            )
        trees[current_path] = frozen_tree
        current_paths.append(current_path)
    worktree_changes = run_git(
        repository_root,
        "status",
        "--porcelain",
        "--untracked-files=all",
        "--",
        *current_paths,
    )
    if worktree_changes:
        raise FreezeCheckError(
            "working-tree changes exist under frozen paths: "
            + ", ".join(line[3:] for line in worktree_changes.splitlines())
        )
    return trees


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repository-root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="DarkTableNext repository root (default: inferred from this script)",
    )
    parser.add_argument(
        "--freeze-commit",
        default=DEFAULT_FREEZE_COMMIT,
        help="immutable 0.9 freeze commit (default: %(default)s)",
    )
    arguments = parser.parse_args()
    try:
        trees = verify(arguments.repository_root.resolve(), arguments.freeze_commit)
    except FreezeCheckError as error:
        print(f"freeze reference check failed: {error}")
        return 1
    print(
        f"freeze reference verified: commit={arguments.freeze_commit} "
        f"legacy-0.9={trees['legacy-0.9']} darktable-tests={trees['darktable-tests']} "
        f"protected_paths={len(PROTECTED_PATHS)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
