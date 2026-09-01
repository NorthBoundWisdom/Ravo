#!/usr/bin/env python3
"""Shared helpers for repository checker scripts."""

from __future__ import annotations

import json
import sys
from collections.abc import Iterable
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[1]


def strip_jsonc(text: str) -> str:
    out: list[str] = []
    index = 0
    in_string = False
    while index < len(text):
        ch = text[index]
        next_ch = text[index + 1] if index + 1 < len(text) else ""
        if ch == '"' and (index == 0 or text[index - 1] != "\\"):
            in_string = not in_string
            out.append(ch)
            index += 1
            continue
        if not in_string and ch == "/" and next_ch == "/":
            while index < len(text) and text[index] != "\n":
                index += 1
            continue
        if not in_string and ch == "/" and next_ch == "*":
            index += 2
            while index + 1 < len(text) and not (
                text[index] == "*" and text[index + 1] == "/"
            ):
                index += 1
            index += 2
            continue
        out.append(ch)
        index += 1
    return "".join(out)


def load_jsonc(path: Path) -> Any:
    return json.loads(strip_jsonc(path.read_text(encoding="utf-8")))


def emit_errors(errors: Iterable[str]) -> bool:
    has_errors = False
    for error in errors:
        print(error, file=sys.stderr)
        has_errors = True
    return has_errors
