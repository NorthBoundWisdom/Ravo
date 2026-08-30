#!/usr/bin/env python3
"""Load and validate the versioned Ravo Studio locale manifest."""

from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path


@dataclass(frozen=True)
class LocaleSpec:
    code: str
    native_name: str
    translation_target: str
    provider_language: str
    catalog: str
    memory: str | None
    aliases: tuple[str, ...]
    source: bool

    def ts_path(self, repo_root: Path) -> Path:
        return repo_root / "Ravo" / "desktop" / "i18n" / self.catalog

    def memory_path(self, repo_root: Path) -> Path | None:
        if self.memory is None:
            return None
        return repo_root / "Ravo" / "desktop" / "i18n" / self.memory


def load_locales(repo_root: Path) -> tuple[LocaleSpec, ...]:
    path = repo_root / "Ravo" / "desktop" / "i18n" / "locales.json"
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schema") != "ravo-studio-locales/v1":
        raise ValueError(f"unsupported locale manifest schema: {data.get('schema')!r}")

    result: list[LocaleSpec] = []
    codes: set[str] = set()
    catalogs: set[str] = set()
    source_locale = data.get("sourceLocale")
    source_count = 0
    for item in data.get("locales", []):
        locale = LocaleSpec(
            code=item["code"],
            native_name=item["nativeName"],
            translation_target=item["translationTarget"],
            provider_language=item["providerCode"],
            catalog=item["catalog"],
            memory=item.get("memory"),
            aliases=tuple(item.get("aliases", [])),
            source=item["code"] == source_locale,
        )
        if locale.code in codes or locale.catalog in catalogs:
            raise ValueError(f"duplicate locale or catalog: {locale.code}")
        if not locale.catalog.endswith(".ts"):
            raise ValueError(f"catalog must be a .ts file: {locale.catalog}")
        if locale.source:
            source_count += 1
            if locale.memory is not None:
                raise ValueError("source locale must not declare translation memory")
        elif locale.memory is None:
            raise ValueError(f"translated locale has no memory: {locale.code}")
        result.append(locale)
        codes.add(locale.code)
        catalogs.add(locale.catalog)

    if not result or source_count != 1:
        raise ValueError("locale manifest must contain exactly one source locale")
    return tuple(result)


def select_locales(locales: tuple[LocaleSpec, ...], requested: list[str]) -> tuple[LocaleSpec, ...]:
    translated = tuple(locale for locale in locales if not locale.source)
    if not requested:
        return translated
    by_code = {locale.code: locale for locale in translated}
    unknown = sorted(set(requested) - by_code.keys())
    if unknown:
        raise ValueError(f"unknown translated locales: {', '.join(unknown)}")
    return tuple(by_code[code] for code in requested)
