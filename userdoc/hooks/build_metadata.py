"""Inject validated repository build metadata into the handbook home page."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from html import escape
import os
from pathlib import Path
import re
import subprocess
from typing import Mapping
from urllib.parse import urlsplit

MARKER = "<!-- RAVO_DOCS_BUILD_METADATA -->"
REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
SHA_PATTERN = re.compile(r"^[0-9a-f]{40}$")
REPOSITORY_PATTERN = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
MAX_COMMIT_MESSAGE_BYTES = 64 * 1024
DISPLAY_TIMEZONE = timezone(timedelta(hours=8), name="UTC+08:00")


@dataclass(frozen=True)
class BuildMetadata:
    commit_sha: str
    commit_message: str
    built_at_iso: str
    built_at_display: str
    commit_url: str | None


def _git(*arguments: str) -> str:
    completed = subprocess.run(
        ["git", *arguments],
        cwd=REPOSITORY_ROOT,
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise RuntimeError(f"Could not read documentation Git metadata: {detail}")
    return completed.stdout.rstrip("\r\n")


def _commit_sha(environment: Mapping[str, str]) -> str:
    commit_sha = environment.get("GITHUB_SHA", "").strip() or _git("rev-parse", "HEAD")
    commit_sha = commit_sha.lower()
    if not SHA_PATTERN.fullmatch(commit_sha):
        raise RuntimeError(
            "Documentation commit SHA must contain exactly 40 hexadecimal digits"
        )
    return commit_sha


def _commit_message(commit_sha: str) -> str:
    message = _git("show", "-s", "--format=%B", commit_sha)
    if not message.strip():
        raise RuntimeError("Documentation commit message is empty")
    if "\x00" in message:
        raise RuntimeError("Documentation commit message contains a NUL byte")
    if len(message.encode("utf-8")) > MAX_COMMIT_MESSAGE_BYTES:
        raise RuntimeError(
            "Documentation commit message exceeds the 64 KiB display limit"
        )
    return message


def _built_at(environment: Mapping[str, str], now: datetime | None) -> tuple[str, str]:
    value = environment.get("RAVO_DOCS_BUILT_AT", "").strip()
    if not value:
        value = (now or datetime.now(timezone.utc)).isoformat()
    normalized = value[:-1] + "+00:00" if value.endswith("Z") else value
    try:
        parsed = datetime.fromisoformat(normalized)
    except ValueError as error:
        raise RuntimeError(
            f"Documentation build time is not ISO 8601: {value}"
        ) from error
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        raise RuntimeError("Documentation build time must include a UTC offset")
    utc = parsed.astimezone(timezone.utc).replace(microsecond=0)
    built_at_iso = utc.isoformat().replace("+00:00", "Z")
    built_at_display = utc.astimezone(DISPLAY_TIMEZONE).strftime(
        "%Y-%m-%d %H:%M:%S UTC+08:00"
    )
    return built_at_iso, built_at_display


def _commit_url(environment: Mapping[str, str], commit_sha: str) -> str | None:
    server_url = environment.get("GITHUB_SERVER_URL", "").strip()
    repository = environment.get("GITHUB_REPOSITORY", "").strip()
    if not server_url and not repository:
        return None
    if not server_url or not repository:
        raise RuntimeError(
            "GITHUB_SERVER_URL and GITHUB_REPOSITORY must be provided together"
        )
    parsed = urlsplit(server_url)
    if parsed.scheme not in {"http", "https"} or not parsed.netloc:
        raise RuntimeError("GITHUB_SERVER_URL must be an absolute HTTP(S) URL")
    if parsed.query or parsed.fragment or not REPOSITORY_PATTERN.fullmatch(repository):
        raise RuntimeError("GitHub repository metadata is malformed")
    return f"{server_url.rstrip('/')}/{repository}/commit/{commit_sha}"


def resolve_build_metadata(
    environment: Mapping[str, str] | None = None, now: datetime | None = None
) -> BuildMetadata:
    active_environment = os.environ if environment is None else environment
    commit_sha = _commit_sha(active_environment)
    built_at_iso, built_at_display = _built_at(active_environment, now)
    return BuildMetadata(
        commit_sha=commit_sha,
        commit_message=_commit_message(commit_sha),
        built_at_iso=built_at_iso,
        built_at_display=built_at_display,
        commit_url=_commit_url(active_environment, commit_sha),
    )


def render_build_metadata(metadata: BuildMetadata) -> str:
    short_sha = metadata.commit_sha[:12]
    commit_value = f"<code>{escape(short_sha)}</code>"
    if metadata.commit_url:
        commit_value = (
            '<a class="ravo-build-metadata__commit" '
            f'href="{escape(metadata.commit_url, quote=True)}" '
            f'title="{escape(metadata.commit_sha, quote=True)}">{commit_value}</a>'
        )
    return f"""<section class="ravo-build-metadata" aria-label="Documentation build information">
  <div class="ravo-build-metadata__grid">
    <div class="ravo-build-metadata__item">
      <span class="ravo-build-metadata__label">Commit</span>
      <span class="ravo-build-metadata__value">{commit_value}</span>
    </div>
    <div class="ravo-build-metadata__item">
      <span class="ravo-build-metadata__label">Documentation built at</span>
      <time class="ravo-build-metadata__value" datetime="{escape(metadata.built_at_iso, quote=True)}">{escape(metadata.built_at_display)}</time>
    </div>
  </div>
  <div class="ravo-build-metadata__message">
    <span class="ravo-build-metadata__label">Full commit message</span>
    <pre><code>{escape(metadata.commit_message)}</code></pre>
  </div>
</section>"""


def inject_build_metadata(markdown: str, metadata: BuildMetadata) -> str:
    if markdown.count(MARKER) != 1:
        raise RuntimeError(
            "Handbook home page must contain exactly one build metadata marker"
        )
    return markdown.replace(MARKER, render_build_metadata(metadata))


_resolved_metadata: BuildMetadata | None = None


def on_page_markdown(markdown: str, page, **_kwargs) -> str:
    if page.file.src_uri != "index.md":
        return markdown
    global _resolved_metadata
    if _resolved_metadata is None:
        _resolved_metadata = resolve_build_metadata()
    return inject_build_metadata(markdown, _resolved_metadata)
