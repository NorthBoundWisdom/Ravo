#!/usr/bin/env python3
"""Validate Ravo Studio's manifest and Qt translation sources."""

from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET


PLACEHOLDER_PATTERN = re.compile(r"%(?:L?\d+|n|%)")
URL_PATTERN = re.compile(r"https?://[^\s<]+")
RESOURCE_PATTERN = re.compile(r"(?:qrc:|:/)[^\s<]+")
EXTENSION_PATTERN = re.compile(r"(?<!\w)\.[A-Za-z][A-Za-z0-9]{0,7}\b")
SHORTCUT_PATTERN = re.compile(r"\b(?:Cmd|Ctrl|Alt|Shift)(?:\+[A-Za-z0-9]+)+\b")
PROTECTED_TERMS = (
    "Ravo Studio", "RAW", "ICC", "D50", "sRGB", "JPEG", "PNG", "TIFF",
    "XMP", "JSON", "QML", "C++", "API",
)


def text_of(element: ET.Element | None) -> str:
    return "" if element is None or element.text is None else element.text


def load_manifest(path: Path) -> tuple[str, dict[str, str], list[str]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schema") != "ravo-studio-locales/v1":
        raise ValueError("unsupported locale manifest schema")
    source_locale = data.get("sourceLocale")
    locales = data.get("locales")
    if not isinstance(source_locale, str) or not isinstance(locales, list) or not locales:
        raise ValueError("locale manifest is missing sourceLocale or locales")
    catalogs: dict[str, str] = {}
    order: list[str] = []
    for item in locales:
        if not isinstance(item, dict):
            raise ValueError("locale manifest entry must be an object")
        code = item.get("code")
        catalog = item.get("catalog")
        if not isinstance(code, str) or not code or not isinstance(catalog, str):
            raise ValueError("locale manifest entry is missing code or catalog")
        if Path(catalog).name != catalog or not catalog.endswith(".ts"):
            raise ValueError(f"invalid catalog path for {code}: {catalog}")
        if code in catalogs or catalog in catalogs.values():
            raise ValueError(f"duplicate locale or catalog: {code}")
        catalogs[code] = catalog
        order.append(code)
    if source_locale not in catalogs:
        raise ValueError("sourceLocale is not declared")
    return source_locale, catalogs, order


def tokens(pattern: re.Pattern[str], text: str) -> Counter[str]:
    return Counter(pattern.findall(text))


def protected_terms(text: str) -> Counter[str]:
    return Counter(term for term in PROTECTED_TERMS if term in text)


def validate_translation(path: Path, expected_language: str, source_locale: str) -> list[str]:
    try:
        tree = ET.parse(path)
    except (ET.ParseError, OSError) as error:
        return [f"{path}: invalid TS XML: {error}"]
    root = tree.getroot()
    errors: list[str] = []
    if root.tag != "TS":
        return [f"{path}: root element must be TS"]
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
            if translation.attrib.get("type") in {"vanished", "obsolete"}:
                continue
            if translation.attrib.get("type") == "unfinished":
                errors.append(f"{path}: unfinished {context_name}::{source!r}")
                continue
            forms = translation.findall("numerusform")
            values = [text_of(form) for form in forms] if forms else [text_of(translation)]
            if not all(values):
                errors.append(f"{path}: empty translation {context_name}::{source!r}")
                continue
            for value in values:
                for label, pattern in (
                    ("placeholder", PLACEHOLDER_PATTERN),
                    ("URL", URL_PATTERN),
                    ("resource", RESOURCE_PATTERN),
                    ("extension", EXTENSION_PATTERN),
                    ("shortcut", SHORTCUT_PATTERN),
                ):
                    if tokens(pattern, source) != tokens(pattern, value):
                        errors.append(
                            f"{path}: {label} mismatch {context_name}::{source!r} -> {value!r}"
                        )
                if protected_terms(source) != protected_terms(value):
                    errors.append(
                        f"{path}: protected term mismatch {context_name}::{source!r} -> {value!r}"
                    )
                if source.count("\n") != value.count("\n"):
                    errors.append(
                        f"{path}: newline mismatch {context_name}::{source!r} -> {value!r}"
                    )
                if expected_language == source_locale and value != source:
                    errors.append(
                        f"{path}: source-locale translation differs for {context_name}::{source!r}"
                    )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--ts", type=Path, action="append", required=True)
    parser.add_argument("--require-all", action="store_true")
    args = parser.parse_args()
    errors: list[str] = []
    try:
        source_locale, catalogs, _ = load_manifest(args.manifest)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"{args.manifest}: invalid locale manifest: {error}", file=sys.stderr)
        return 1
    expected_by_name = {name: code for code, name in catalogs.items()}
    provided_names = [path.name for path in args.ts]
    for path in args.ts:
        expected = expected_by_name.get(path.name)
        if expected is None:
            errors.append(f"{path}: catalog is not declared in the locale manifest")
            continue
        errors.extend(validate_translation(path, expected, source_locale))
    if args.require_all:
        if Counter(provided_names) != Counter(expected_by_name.keys()):
            errors.append("provided TS files do not exactly match the locale manifest")
        discovered = {path.name for path in args.manifest.parent.glob("RavoStudio_*.ts")}
        if discovered != set(expected_by_name):
            errors.append("tracked TS files do not exactly match the locale manifest")
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
