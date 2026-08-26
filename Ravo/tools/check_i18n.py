#!/usr/bin/env python3
"""Validate Ravo Studio Qt translation sources before lrelease."""

from __future__ import annotations

import argparse
from collections import Counter
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET


PLACEHOLDER_PATTERN = re.compile(r"%(?:L?\d+|n|%)")


def placeholders(text: str) -> Counter[str]:
    return Counter(PLACEHOLDER_PATTERN.findall(text))


def text_of(element: ET.Element | None) -> str:
    return "" if element is None or element.text is None else element.text


def validate_translation(path: Path) -> list[str]:
    try:
        tree = ET.parse(path)
    except (ET.ParseError, OSError) as error:
        return [f"{path}: invalid TS XML: {error}"]

    root = tree.getroot()
    errors: list[str] = []
    if root.tag != "TS":
        return [f"{path}: root element must be TS"]

    expected_language = "zh_CN" if path.stem.endswith("_zh_CN") else "en_US"
    if root.attrib.get("language") != expected_language:
        errors.append(
            f"{path}: language is {root.attrib.get('language')!r}, expected {expected_language!r}"
        )

    for context in root.findall("context"):
        context_name = text_of(context.find("name")) or "<unnamed>"
        for message in context.findall("message"):
            source = text_of(message.find("source"))
            translation = message.find("translation")
            if not source:
                errors.append(f"{path}: {context_name} contains a message without source text")
                continue
            if translation is None:
                errors.append(f"{path}: {context_name}::{source!r} has no translation")
                continue

            attrs = translation.attrib
            if attrs.get("type") in {"vanished", "obsolete"}:
                continue
            if attrs.get("type") == "unfinished":
                errors.append(f"{path}: unfinished {context_name}::{source!r}")
                continue

            translation_forms = translation.findall("numerusform")
            values = [text_of(form) for form in translation_forms] if translation_forms else [
                text_of(translation)
            ]
            if not all(values):
                errors.append(f"{path}: empty translation {context_name}::{source!r}")
                continue
            for value in values:
                if placeholders(source) != placeholders(value):
                    errors.append(
                        f"{path}: placeholder mismatch {context_name}::{source!r} -> {value!r}"
                    )
                if source.count("\n") != value.count("\n"):
                    errors.append(
                        f"{path}: newline mismatch {context_name}::{source!r} -> {value!r}"
                    )
                if expected_language == "en_US" and value != source:
                    errors.append(
                        f"{path}: English translation differs from source for "
                        f"{context_name}::{source!r}"
                    )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ts", type=Path, action="append", required=True)
    arguments = parser.parse_args()

    errors: list[str] = []
    for path in arguments.ts:
        errors.extend(validate_translation(path))
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
