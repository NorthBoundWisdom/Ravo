#!/usr/bin/env python3
"""Unit tests for check_changed_whitespace.py using temporary Git repositories."""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

import check_changed_whitespace as whitespace


def run(repo: Path, args: list[str]) -> str:
    result = subprocess.run(
        ["git", "-C", str(repo), *args],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return result.stdout.strip()


def init_repo(path: Path) -> None:
    run(path, ["init", "-b", "main"])
    run(path, ["config", "user.email", "whitespace-test@example.com"])
    run(path, ["config", "user.name", "Whitespace Test"])


def commit_file(repo: Path, relative: str, content: str, message: str) -> str:
    path = repo / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    run(repo, ["add", relative])
    run(repo, ["commit", "-m", message])
    return run(repo, ["rev-parse", "HEAD"])


def write_event(repo: Path, payload: dict) -> Path:
    path = repo / "event.json"
    path.write_text(json.dumps(payload), encoding="utf-8")
    return path


class ChangedWhitespaceTest(unittest.TestCase):
    def test_push_two_commits_detects_trailing_whitespace(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            init_repo(repo)
            first = commit_file(repo, "a.txt", "one\n", "first")
            dirty = commit_file(repo, "b.txt", "two  \n", "second with trail")
            event = write_event(
                repo,
                {"before": first, "after": dirty, "ref": "refs/heads/main"},
            )
            code = whitespace.check(
                repo=repo, event_path=event, event_name="push"
            )
            self.assertEqual(code, 1)

    def test_push_two_commits_clean_passes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            init_repo(repo)
            first = commit_file(repo, "a.txt", "one\n", "first")
            second = commit_file(repo, "b.txt", "two\n", "second")
            event = write_event(
                repo,
                {"before": first, "after": second, "ref": "refs/heads/main"},
            )
            code = whitespace.check(
                repo=repo, event_path=event, event_name="push"
            )
            self.assertEqual(code, 0)

    def test_pull_request_uses_merge_base_range(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            init_repo(repo)
            base = commit_file(repo, "a.txt", "base\n", "base")
            run(repo, ["checkout", "-b", "feature"])
            head = commit_file(repo, "f.txt", "feature  \n", "feature trail")
            event = write_event(
                repo,
                {
                    "pull_request": {
                        "base": {"sha": base},
                        "head": {"sha": head},
                    }
                },
            )
            code = whitespace.check(
                repo=repo, event_path=event, event_name="pull_request"
            )
            self.assertEqual(code, 1)

    def test_new_branch_push_against_default_tip(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            init_repo(repo)
            commit_file(repo, "a.txt", "main\n", "main")
            run(repo, ["checkout", "-b", "feature"])
            tip = commit_file(repo, "f.txt", "feature  \n", "feature")
            event = write_event(
                repo,
                {
                    "before": whitespace.ZERO_SHA,
                    "after": tip,
                    "ref": "refs/heads/feature",
                },
            )
            code = whitespace.check(
                repo=repo, event_path=event, event_name="push"
            )
            self.assertEqual(code, 1)

    def test_root_commit_push_checks_against_empty_tree(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            init_repo(repo)
            tip = commit_file(repo, "a.txt", "root  \n", "root")
            event = write_event(
                repo,
                {
                    "before": whitespace.ZERO_SHA,
                    "after": tip,
                    "ref": "refs/heads/main",
                },
            )
            # Only one commit on main: treated as root when before is zeros and
            # default tip equals after with parents? Actually after has no... wait
            # after has no parents only for the true first commit. Here tip has
            # no parents. default tip is tip itself -> push_new_branch with
            # parents[0] would fail. Our code: parents empty -> push_root.
            code = whitespace.check(
                repo=repo, event_path=event, event_name="push"
            )
            self.assertEqual(code, 1)

    def test_merge_commit_push_range(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            init_repo(repo)
            base = commit_file(repo, "a.txt", "base\n", "base")
            run(repo, ["checkout", "-b", "side"])
            commit_file(repo, "s.txt", "side\n", "side")
            run(repo, ["checkout", "main"])
            commit_file(repo, "m.txt", "mainline\n", "mainline")
            run(repo, ["merge", "--no-ff", "side", "-m", "merge"])
            merge = run(repo, ["rev-parse", "HEAD"])
            # Introduce trailing whitespace on merge commit via amend-like new file commit after merge
            dirty = commit_file(repo, "d.txt", "dirty  \n", "after merge")
            event = write_event(
                repo,
                {"before": merge, "after": dirty, "ref": "refs/heads/main"},
            )
            code = whitespace.check(
                repo=repo, event_path=event, event_name="push"
            )
            self.assertEqual(code, 1)

    def test_missing_before_object_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            init_repo(repo)
            tip = commit_file(repo, "a.txt", "ok\n", "ok")
            missing = "1234567890abcdef1234567890abcdef12345678"
            event = write_event(
                repo,
                {"before": missing, "after": tip, "ref": "refs/heads/main"},
            )
            with self.assertRaises(whitespace.WhitespaceCheckError):
                whitespace.check(repo=repo, event_path=event, event_name="push")

    def test_tag_push_uses_default_branch_baseline(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            init_repo(repo)
            first = commit_file(repo, "a.txt", "one\n", "one")
            tip = commit_file(repo, "b.txt", "two  \n", "two")
            run(repo, ["tag", "v1", tip])
            event = write_event(
                repo,
                {
                    "before": whitespace.ZERO_SHA,
                    "after": tip,
                    "ref": "refs/tags/v1",
                },
            )
            code = whitespace.check(
                repo=repo, event_path=event, event_name="push"
            )
            self.assertEqual(code, 1)
            self.assertTrue(first)

    def test_manual_dispatch_uses_parent_when_on_default(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            init_repo(repo)
            commit_file(repo, "a.txt", "one\n", "one")
            tip = commit_file(repo, "b.txt", "two  \n", "two")
            event = write_event(repo, {"inputs": {}})
            code = whitespace.check(
                repo=repo, event_path=event, event_name="workflow_dispatch"
            )
            self.assertEqual(code, 1)
            self.assertEqual(run(repo, ["rev-parse", "HEAD"]), tip)

    def test_local_dirty_and_cached_checks(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            init_repo(repo)
            commit_file(repo, "a.txt", "clean\n", "clean")
            (repo / "a.txt").write_text("dirty  \n", encoding="utf-8")
            code = whitespace.check(repo=repo, event_path=None, event_name=None)
            self.assertEqual(code, 1)
            run(repo, ["checkout", "--", "a.txt"])
            (repo / "b.txt").write_text("staged  \n", encoding="utf-8")
            run(repo, ["add", "b.txt"])
            code = whitespace.check(repo=repo, event_path=None, event_name=None)
            self.assertEqual(code, 1)

    def test_main_returns_2_on_missing_before(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            init_repo(repo)
            tip = commit_file(repo, "a.txt", "ok\n", "ok")
            event = write_event(
                repo,
                {
                    "before": "1234567890abcdef1234567890abcdef12345678",
                    "after": tip,
                    "ref": "refs/heads/main",
                },
            )
            code = whitespace.main(
                [
                    "--repository-root",
                    str(repo),
                    "--event-path",
                    str(event),
                    "--event-name",
                    "push",
                ]
            )
            self.assertEqual(code, 2)


if __name__ == "__main__":
    unittest.main()
