from __future__ import annotations

from datetime import datetime, timezone
import importlib.util
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest import mock

import yaml

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
HOOK_PATH = REPOSITORY_ROOT / "userdoc" / "hooks" / "build_metadata.py"
WORKFLOW_PATH = REPOSITORY_ROOT / ".github" / "workflows" / "userdoc-pages.yml"
MKDOCS_PATH = REPOSITORY_ROOT / "userdoc" / "mkdocs.yml"

spec = importlib.util.spec_from_file_location("ravo_userdoc_build_metadata", HOOK_PATH)
if spec is None or spec.loader is None:
    raise RuntimeError(f"Could not load build metadata hook: {HOOK_PATH}")
build_metadata = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = build_metadata
spec.loader.exec_module(build_metadata)


class BuildMetadataTest(unittest.TestCase):
    def setUp(self) -> None:
        build_metadata._resolved_metadata = None

    def test_resolve_uses_ci_identity_and_converts_build_time_to_utc_plus_eight(
        self,
    ) -> None:
        commit_sha = "a" * 40
        environment = {
            "GITHUB_SHA": commit_sha,
            "GITHUB_SERVER_URL": "https://github.com",
            "GITHUB_REPOSITORY": "NorthBoundWisdom/Ravo",
            "RAVO_DOCS_BUILT_AT": "2026-08-31T04:57:45Z",
        }
        with mock.patch.object(
            build_metadata, "_git", return_value="fix: preserve viewport\n\nFull body"
        ):
            metadata = build_metadata.resolve_build_metadata(environment)

        self.assertEqual(metadata.commit_sha, commit_sha)
        self.assertEqual(metadata.commit_message, "fix: preserve viewport\n\nFull body")
        self.assertEqual(metadata.built_at_iso, "2026-08-31T04:57:45Z")
        self.assertEqual(metadata.built_at_display, "2026-08-31 12:57:45 UTC+08:00")
        self.assertEqual(
            metadata.commit_url,
            f"https://github.com/NorthBoundWisdom/Ravo/commit/{commit_sha}",
        )

    def test_local_build_time_requires_an_offset(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "must include a UTC offset"):
            build_metadata._built_at(
                {"RAVO_DOCS_BUILT_AT": "2026-08-31T12:57:45"},
                datetime(2026, 8, 31, tzinfo=timezone.utc),
            )

    def test_render_escapes_full_commit_message_and_preserves_lines(self) -> None:
        metadata = build_metadata.BuildMetadata(
            commit_sha="b" * 40,
            commit_message="fix: <unsafe>\n\nbody & details",
            built_at_iso="2026-08-31T04:57:45Z",
            built_at_display="2026-08-31 12:57:45 UTC+08:00",
            commit_url="https://github.com/NorthBoundWisdom/Ravo/commit/" + "b" * 40,
        )
        rendered = build_metadata.render_build_metadata(metadata)

        self.assertIn("Full commit message", rendered)
        self.assertIn("fix: &lt;unsafe&gt;\n\nbody &amp; details", rendered)
        self.assertNotIn("<unsafe>", rendered)
        self.assertIn("b" * 12, rendered)
        self.assertIn('datetime="2026-08-31T04:57:45Z"', rendered)

    def test_home_marker_is_required_exactly_once(self) -> None:
        metadata = build_metadata.BuildMetadata(
            commit_sha="c" * 40,
            commit_message="docs: metadata",
            built_at_iso="2026-08-31T04:57:45Z",
            built_at_display="2026-08-31 12:57:45 UTC+08:00",
            commit_url=None,
        )
        injected = build_metadata.inject_build_metadata(
            f"before\n{build_metadata.MARKER}\nafter", metadata
        )
        self.assertIn("Documentation built at", injected)
        self.assertNotIn(build_metadata.MARKER, injected)
        for invalid in ("missing", build_metadata.MARKER * 2):
            with self.assertRaisesRegex(RuntimeError, "exactly one"):
                build_metadata.inject_build_metadata(invalid, metadata)

    def test_non_home_page_does_not_resolve_or_inject_metadata(self) -> None:
        page = SimpleNamespace(file=SimpleNamespace(src_uri="guides/develop.md"))
        with mock.patch.object(build_metadata, "resolve_build_metadata") as resolver:
            self.assertEqual(
                build_metadata.on_page_markdown("unchanged", page), "unchanged"
            )
        resolver.assert_not_called()


class UserdocWorkflowTest(unittest.TestCase):
    def test_main_pushes_always_publish_and_capture_build_time(self) -> None:
        workflow = yaml.load(
            WORKFLOW_PATH.read_text(encoding="utf-8"), Loader=yaml.BaseLoader
        )
        triggers = workflow["on"]
        self.assertEqual(triggers["push"], {"branches": ["main"]})
        self.assertIn("paths", triggers["pull_request"])

        steps = workflow["jobs"]["build"]["steps"]
        metadata_step = next(
            step
            for step in steps
            if step.get("name") == "Capture documentation build metadata"
        )
        self.assertIn("RAVO_DOCS_BUILT_AT", metadata_step["run"])
        self.assertIn("GITHUB_ENV", metadata_step["run"])

    def test_mkdocs_registers_the_hook_and_stylesheet(self) -> None:
        config = yaml.load(
            MKDOCS_PATH.read_text(encoding="utf-8"), Loader=yaml.BaseLoader
        )
        self.assertIn("hooks/build_metadata.py", config["hooks"])
        self.assertIn("stylesheets/build-metadata.css", config["extra_css"])


if __name__ == "__main__":
    unittest.main()
