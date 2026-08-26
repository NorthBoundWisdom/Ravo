#!/usr/bin/env python3
"""Verify leftover 0.9 trees still match the freeze, minus retired owners."""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


DEFAULT_FREEZE_COMMIT = "320970bf7c9cbbc6611cfc3eb60f8f2b0424b782"
# Current path -> path at the freeze commit.
PROTECTED_PATHS: tuple[tuple[str, str], ...] = (
    ("legacy/src", "src"),
    ("legacy/tests", "darktable-tests"),
    ("legacy/host/cmake", "cmake"),
    ("legacy/host/data", "data"),
    ("legacy/host/packaging", "packaging"),
)
RETIRED_SRC_LIST = Path("DevDocs/phase0/legacy-retired-src-paths.txt")
# Leftover CMake registries may drop retired add_iop / add_library lines.
# Their blobs are not freeze-identical after an accepted retirement.
MUTABLE_LEFTOVER_SRC_PATHS = {
    "iop/CMakeLists.txt",
    "libs/CMakeLists.txt",
}


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
    return completed.stdout.rstrip("\n")


def load_retired_src_paths(repository_root: Path) -> set[str]:
    path = repository_root / RETIRED_SRC_LIST
    if not path.is_file():
        raise FreezeCheckError(f"missing retired-path list: {RETIRED_SRC_LIST}")
    retired: set[str] = set()
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        retired.add(line)
    return retired


def list_tree(repository_root: Path, spec: str) -> set[str]:
    output = run_git(repository_root, "ls-tree", "-r", "--name-only", spec)
    return {line for line in output.splitlines() if line}


def blob_id(repository_root: Path, spec: str) -> str:
    return run_git(repository_root, "rev-parse", spec)


def verify_exact_tree(
    repository_root: Path, freeze: str, current_path: str, freeze_path: str
) -> str:
    frozen_tree = blob_id(repository_root, f"{freeze}:{freeze_path}")
    current_tree = blob_id(repository_root, f"HEAD:{current_path}")
    if frozen_tree != current_tree:
        raise FreezeCheckError(
            f"committed {current_path} object differs from freeze path {freeze_path}: "
            f"{frozen_tree} != {current_tree}"
        )
    return frozen_tree


def verify_src_with_retirements(
    repository_root: Path, freeze: str, retired: set[str]
) -> str:
    frozen_files = list_tree(repository_root, f"{freeze}:src")
    current_files = list_tree(repository_root, "HEAD:legacy/src")
    unknown_retired = sorted(retired - frozen_files)
    if unknown_retired:
        raise FreezeCheckError(
            "retired src paths are not in the freeze tree: " + ", ".join(unknown_retired)
        )
    unexpected_deleted = sorted(frozen_files - current_files - retired)
    if unexpected_deleted:
        raise FreezeCheckError(
            "committed legacy/src is missing files that are not retired: "
            + ", ".join(unexpected_deleted)
        )
    unexpected_added = sorted(current_files - frozen_files)
    if unexpected_added:
        raise FreezeCheckError(
            "committed legacy/src has files that are not in the freeze tree: "
            + ", ".join(unexpected_added)
        )
    for relative in sorted(current_files):
        if relative in MUTABLE_LEFTOVER_SRC_PATHS:
            continue
        frozen_blob = blob_id(repository_root, f"{freeze}:src/{relative}")
        current_blob = blob_id(repository_root, f"HEAD:legacy/src/{relative}")
        if frozen_blob != current_blob:
            raise FreezeCheckError(
                f"committed leftover legacy/src/{relative} differs from freeze src/{relative}"
            )
    return blob_id(repository_root, f"{freeze}:src")


def porcelain_path(line: str) -> str:
    if len(line) < 4:
        return ""
    path = line[3:]
    if " -> " in path:
        path = path.split(" -> ", 1)[1]
    return path.strip().strip('"')


def verify_worktree(
    repository_root: Path, current_paths: list[str], retired: set[str]
) -> None:
    worktree_changes = run_git(
        repository_root,
        "status",
        "--porcelain",
        "--untracked-files=all",
        "--",
        *current_paths,
    )
    unexpected: list[str] = []
    for line in worktree_changes.splitlines():
        path = porcelain_path(line)
        prefix = line[:2] if len(line) >= 2 else ""
        relative = path[len("legacy/src/") :] if path.startswith("legacy/src/") else ""
        deleted = "D" in prefix
        if path.startswith("legacy/src/") and deleted and relative in retired:
            continue
        if path.startswith("legacy/src/") and relative in MUTABLE_LEFTOVER_SRC_PATHS:
            continue
        unexpected.append(path or line)
    if unexpected:
        raise FreezeCheckError(
            "working-tree changes exist under frozen paths: " + ", ".join(unexpected)
        )


def verify(repository_root: Path, freeze_commit: str) -> dict[str, str]:
    freeze = run_git(repository_root, "rev-parse", f"{freeze_commit}^{{commit}}")
    run_git(repository_root, "merge-base", "--is-ancestor", freeze, "HEAD^{commit}")
    retired = load_retired_src_paths(repository_root)

    trees: dict[str, str] = {}
    current_paths: list[str] = []
    for current_path, freeze_path in PROTECTED_PATHS:
        if current_path == "legacy/src":
            trees[current_path] = verify_src_with_retirements(
                repository_root, freeze, retired
            )
        else:
            trees[current_path] = verify_exact_tree(
                repository_root, freeze, current_path, freeze_path
            )
        current_paths.append(current_path)
    verify_worktree(repository_root, current_paths, retired)
    return trees


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repository-root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="Ravo repository root (default: inferred from this script)",
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
        f"legacy/src={trees['legacy/src']} legacy/tests={trees['legacy/tests']} "
        f"protected_paths={len(PROTECTED_PATHS)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
