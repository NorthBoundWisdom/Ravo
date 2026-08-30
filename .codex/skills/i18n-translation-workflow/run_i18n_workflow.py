#!/usr/bin/env python3
"""Run deterministic extraction and translation-memory application."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys

from _locales import load_locales, select_locales
from translation_memory import apply_memory, make_source_identity, sync_memory


def _run(command: list[str], repo_root: Path) -> None:
    subprocess.run(command, cwd=repo_root, check=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--part", choices=("1", "2", "all"), default="all")
    parser.add_argument("--locale", action="append", default=[])
    parser.add_argument("--lupdate", type=Path)
    parser.add_argument("--clean-ts", action="store_true")
    args = parser.parse_args()
    repo_root = args.repo_root.resolve()
    skill_dir = Path(__file__).resolve().parent
    locales = load_locales(repo_root)
    selected = select_locales(locales, args.locale)
    source = next(locale for locale in locales if locale.source)

    if args.clean_ts:
        for locale in locales:
            locale.ts_path(repo_root).unlink(missing_ok=True)

    if args.part in ("1", "all"):
        _run([sys.executable, str(skill_dir / "1_add_qstr_to_qml.py"),
              str(repo_root / "Ravo/desktop/qml")], repo_root)
        command = [sys.executable, str(skill_dir / "2_update_translations.py"),
                   "--repo-root", str(repo_root)]
        if args.lupdate:
            command.extend(["--lupdate", str(args.lupdate)])
        _run(command, repo_root)
        config_directory = repo_root / "Ravo/desktop/config"
        if config_directory.is_dir():
            _run([sys.executable, str(skill_dir / "3_add_json_strings_to_ts.py"),
                  str(config_directory)], repo_root)
        else:
            print(f"no desktop JSON translation owner at {config_directory}; skipping")
        make_source_identity(source.ts_path(repo_root))
        for locale in selected:
            memory_path = locale.memory_path(repo_root)
            assert memory_path is not None
            completed, total = sync_memory(locale.ts_path(repo_root), memory_path)
            print(f"synced {locale.code}: {completed}/{total}")

    if args.part in ("2", "all"):
        make_source_identity(source.ts_path(repo_root))
        for locale in selected:
            memory_path = locale.memory_path(repo_root)
            assert memory_path is not None
            completed, total = apply_memory(locale.ts_path(repo_root), memory_path)
            if completed != total:
                raise SystemExit(f"incomplete translation memory for {locale.code}: {completed}/{total}")
            print(f"applied {locale.code}: {completed}/{total}")
        checker = repo_root / "Ravo" / "tools" / "check_i18n.py"
        check_command = [sys.executable, str(checker), "--manifest",
                         str(repo_root / "Ravo/desktop/i18n/locales.json"), "--require-all"]
        for locale in locales:
            check_command.extend(["--ts", str(locale.ts_path(repo_root))])
        _run(check_command, repo_root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
