#!/usr/bin/env python3
"""Validate whitespace on the GitHub event change range (or local dirty tree).

Reads GITHUB_EVENT_PATH when present and invokes git via an argv array only —
never interpolates PR titles, branch names, or JSON into a shell command.

Modes:
  pull_request     merge-base(base.sha, head.sha) -> head.sha
  push             before -> after when before resolves
  push_new_branch  merge-base(default tip, after) -> after when before is zeros
  push_root        empty tree -> after when after has no parents
  tag / manual     default-branch merge-base (or first parent / empty tree)
  local            git diff --check and git diff --cached --check

Missing objects, unsupported events, and unavailable baselines fail closed.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any


ZERO_SHA = "0" * 40
EMPTY_TREE = "4b825dc642cb6eb9a060e54bf8d69288fbee4904"


class WhitespaceCheckError(Exception):
    """Raised when the change-range whitespace check cannot succeed."""


def run_git(git_executable: str, repo: Path, args: list[str]) -> subprocess.CompletedProcess[str]:
    command = [git_executable, "-C", str(repo), *args]
    return subprocess.run(
        command,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def require_git_ok(result: subprocess.CompletedProcess[str], *, context: str) -> str:
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        raise WhitespaceCheckError(f"{context}: git exited {result.returncode}: {detail}")
    return result.stdout


def object_exists(git_executable: str, repo: Path, sha: str) -> bool:
    result = run_git(git_executable, repo, ["cat-file", "-e", f"{sha}^{{object}}"])
    return result.returncode == 0


def rev_parse(git_executable: str, repo: Path, rev: str) -> str:
    result = run_git(git_executable, repo, ["rev-parse", "--verify", rev])
    return require_git_ok(result, context=f"rev-parse {rev}").strip()


def merge_base(git_executable: str, repo: Path, left: str, right: str) -> str:
    result = run_git(git_executable, repo, ["merge-base", left, right])
    return require_git_ok(result, context=f"merge-base {left} {right}").strip()


def commit_parents(git_executable: str, repo: Path, sha: str) -> list[str]:
    result = run_git(git_executable, repo, ["rev-list", "--parents", "-n", "1", sha])
    line = require_git_ok(result, context=f"parents for {sha}").strip()
    parts = line.split()
    if not parts or parts[0] != sha:
        raise WhitespaceCheckError(f"unexpected rev-list parents output for {sha}: {line!r}")
    return parts[1:]


def is_zero_sha(sha: str) -> bool:
    return bool(sha) and set(sha) == {"0"}


def default_branch_tip(git_executable: str, repo: Path) -> str | None:
    for candidate in (
        "refs/remotes/origin/HEAD",
        "refs/remotes/origin/main",
        "refs/remotes/origin/master",
        "refs/heads/main",
        "refs/heads/master",
    ):
        result = run_git(git_executable, repo, ["rev-parse", "--verify", candidate])
        if result.returncode == 0 and result.stdout.strip():
            return result.stdout.strip()
    return None


def load_event(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        raise WhitespaceCheckError(f"cannot read GITHUB_EVENT_PATH: {error}") from error
    except json.JSONDecodeError as error:
        raise WhitespaceCheckError(f"GITHUB_EVENT_PATH is not valid JSON: {error}") from error
    if not isinstance(data, dict):
        raise WhitespaceCheckError("GITHUB_EVENT_PATH JSON root must be an object")
    return data


def infer_event_name(event: dict[str, Any], explicit: str | None) -> str:
    if explicit:
        return explicit
    if "pull_request" in event and isinstance(event.get("pull_request"), dict):
        return "pull_request"
    if "before" in event and "after" in event:
        return "push"
    if "workflow_dispatch" in event or event.get("inputs") is not None:
        return "workflow_dispatch"
    raise WhitespaceCheckError("unable to infer GitHub event name; set GITHUB_EVENT_NAME")


def resolve_range(
    event: dict[str, Any] | None,
    *,
    git_executable: str,
    repo: Path,
    event_name: str | None,
) -> tuple[str, str | None, str | None]:
    """Return (mode, base, head). base/head None means local dirty-tree mode."""
    if event is None:
        return "local", None, None

    name = infer_event_name(event, event_name)

    if name == "pull_request":
        pr = event.get("pull_request")
        if not isinstance(pr, dict):
            raise WhitespaceCheckError("pull_request event missing pull_request object")
        base_obj = pr.get("base") if isinstance(pr.get("base"), dict) else {}
        head_obj = pr.get("head") if isinstance(pr.get("head"), dict) else {}
        base_sha = str(base_obj.get("sha") or "")
        head_sha = str(head_obj.get("sha") or "")
        if len(base_sha) < 7 or len(head_sha) < 7:
            raise WhitespaceCheckError("pull_request base/head sha missing")
        if not object_exists(git_executable, repo, base_sha):
            raise WhitespaceCheckError(f"pull_request base sha not in repository: {base_sha}")
        if not object_exists(git_executable, repo, head_sha):
            raise WhitespaceCheckError(f"pull_request head sha not in repository: {head_sha}")
        base = merge_base(git_executable, repo, base_sha, head_sha)
        return "pull_request", base, head_sha

    if name == "push":
        before = str(event.get("before") or "")
        after = str(event.get("after") or "")
        ref = str(event.get("ref") or "")
        if len(after) < 7:
            raise WhitespaceCheckError("push event missing after sha")
        if not object_exists(git_executable, repo, after):
            raise WhitespaceCheckError(f"push after sha not in repository: {after}")

        if ref.startswith("refs/tags/"):
            # For annotated tags the push event's after SHA names the tag object,
            # while rev-list/diff ranges require the peeled commit.
            tag_commit = rev_parse(git_executable, repo, f"{after}^{{commit}}")
            tip = default_branch_tip(git_executable, repo)
            parents = commit_parents(git_executable, repo, tag_commit)
            if tip is None:
                if not parents:
                    return "tag_root", EMPTY_TREE, tag_commit
                raise WhitespaceCheckError(
                    "tag push has no default-branch baseline; refusing silent empty-diff success"
                )
            if tip == tag_commit:
                if not parents:
                    return "tag_root", EMPTY_TREE, tag_commit
                return "tag", parents[0], tag_commit
            base = merge_base(git_executable, repo, tip, tag_commit)
            return "tag", base, tag_commit

        if is_zero_sha(before):
            parents = commit_parents(git_executable, repo, after)
            if not parents:
                return "push_root", EMPTY_TREE, after
            tip = default_branch_tip(git_executable, repo)
            if tip is None:
                raise WhitespaceCheckError(
                    "new-branch push has no default-branch baseline; refusing silent empty range"
                )
            if tip == after:
                return "push_new_branch", parents[0], after
            base = merge_base(git_executable, repo, tip, after)
            return "push_new_branch", base, after

        if not before:
            raise WhitespaceCheckError("push event missing before sha")
        if not object_exists(git_executable, repo, before):
            raise WhitespaceCheckError(f"push before sha not in repository: {before}")
        return "push", before, after

    if name in {"workflow_dispatch", "schedule"}:
        head = rev_parse(git_executable, repo, "HEAD")
        parents = commit_parents(git_executable, repo, head)
        if not parents:
            return "manual_root", EMPTY_TREE, head
        tip = default_branch_tip(git_executable, repo)
        if tip is None:
            raise WhitespaceCheckError(
                f"{name} has no default-branch baseline; refusing silent empty-diff success"
            )
        if tip == head:
            return "manual", parents[0], head
        base = merge_base(git_executable, repo, tip, head)
        return "manual", base, head

    raise WhitespaceCheckError(f"unsupported GitHub event name: {name!r}")


def diff_check(
    git_executable: str,
    repo: Path,
    *,
    base: str | None,
    head: str | None,
) -> subprocess.CompletedProcess[str]:
    if base is None and head is None:
        unstaged = run_git(git_executable, repo, ["diff", "--check"])
        if unstaged.returncode != 0:
            return unstaged
        return run_git(git_executable, repo, ["diff", "--cached", "--check"])
    assert base is not None and head is not None
    return run_git(git_executable, repo, ["diff", "--check", base, head])


def check(
    *,
    repo: Path,
    git_executable: str = "git",
    event_path: Path | None = None,
    event_name: str | None = None,
) -> int:
    """Run the whitespace check.

    Explicit ``event_path=None`` means local dirty-tree mode and does **not**
    fall back to ``GITHUB_EVENT_*`` environment variables. Callers that want
    Actions event resolution (notably ``main()``) must pass the path/name
    themselves after reading the environment.
    """
    event: dict[str, Any] | None = None
    if event_path is not None:
        event = load_event(event_path)

    mode, base, head = resolve_range(
        event,
        git_executable=git_executable,
        repo=repo,
        event_name=event_name,
    )
    print(f"whitespace_check mode={mode} base={base} head={head}")

    result = diff_check(git_executable, repo, base=base, head=head)
    if result.stdout:
        sys.stdout.write(result.stdout)
        if not result.stdout.endswith("\n"):
            sys.stdout.write("\n")
    if result.stderr:
        sys.stderr.write(result.stderr)
        if not result.stderr.endswith("\n"):
            sys.stderr.write("\n")
    # git diff --check returns non-zero on whitespace problems; the exact code
    # varies by Git version (commonly 1 or 2). Treat any non-zero as failure.
    return 1 if result.returncode != 0 else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repository-root",
        type=Path,
        default=Path.cwd(),
        help="Git repository root (default: cwd)",
    )
    parser.add_argument(
        "--git",
        dest="git_executable",
        default="git",
        help="git executable path (argv only; never shell-interpolated)",
    )
    parser.add_argument(
        "--event-path",
        type=Path,
        default=None,
        help="Override GITHUB_EVENT_PATH",
    )
    parser.add_argument(
        "--event-name",
        default=None,
        help="Override GITHUB_EVENT_NAME",
    )
    args = parser.parse_args(argv)
    event_path = args.event_path
    if event_path is None and os.environ.get("GITHUB_EVENT_PATH"):
        event_path = Path(os.environ["GITHUB_EVENT_PATH"])
    event_name = args.event_name or os.environ.get("GITHUB_EVENT_NAME")
    try:
        return check(
            repo=args.repository_root.resolve(),
            git_executable=args.git_executable,
            event_path=event_path,
            event_name=event_name,
        )
    except WhitespaceCheckError as error:
        print(f"whitespace_check error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
