from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

import check_translation_unit_size as size_check


def write_manifest(root: Path, debt: list[dict]) -> Path:
    path = root / "configs/translation_unit_size_budget.jsonc"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps({"schema_version": 1, "line_limit": 2000, "debt": debt}),
        encoding="utf-8",
    )
    return path.relative_to(root)


def write_cpp(root: Path, relative: str, lines: int) -> None:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"\n" * lines)


def debt_entry(path: str, baseline_lines: int) -> dict:
    return {
        "path": path,
        "baseline_lines": baseline_lines,
        "classification": "test" if path.startswith("Ravo/tests/") else "production",
        "owner": "unit test",
        "removal_phase": "G0",
    }


class TranslationUnitSizeTest(unittest.TestCase):
    def test_current_repository_passes(self) -> None:
        self.assertEqual(size_check.check_contract(), [])

    def test_allows_existing_debt_to_shrink(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            relative = "Ravo/engine/src/existing.cpp"
            manifest = write_manifest(root, [debt_entry(relative, 2002)])
            write_cpp(root, relative, 2001)
            self.assertEqual(size_check.check_contract(root, manifest), [])

    def test_rejects_new_oversized_translation_unit(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = write_manifest(root, [])
            write_cpp(root, "Ravo/tests/new.cpp", 2001)
            errors = size_check.check_contract(root, manifest)
            self.assertTrue(any("new oversized translation unit" in error for error in errors))

    def test_rejects_debt_growth(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            relative = "Ravo/engine/src/grown.cpp"
            manifest = write_manifest(root, [debt_entry(relative, 2001)])
            write_cpp(root, relative, 2002)
            errors = size_check.check_contract(root, manifest)
            self.assertTrue(any("translation-unit debt grew" in error for error in errors))

    def test_rejects_retired_entry_left_in_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            relative = "Ravo/engine/src/retired.cpp"
            manifest = write_manifest(root, [debt_entry(relative, 2001)])
            write_cpp(root, relative, 2000)
            errors = size_check.check_contract(root, manifest)
            self.assertTrue(any("retired translation-unit debt" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
