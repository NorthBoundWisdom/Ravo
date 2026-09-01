from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

import check_qml_file_size as size_check


def write_manifest(root: Path, debt: list[dict]) -> Path:
    path = root / "configs/qml_file_size_budget.jsonc"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps({"schema_version": 1, "line_limit": 2000, "debt": debt}),
        encoding="utf-8",
    )
    return path.relative_to(root)


def write_qml(root: Path, relative: str, lines: int) -> None:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"\n" * lines)


def debt_entry(path: str, baseline_lines: int) -> dict:
    return {
        "path": path,
        "baseline_lines": baseline_lines,
        "owner": "unit test",
        "removal_phase": "G0",
    }


class QmlFileSizeTest(unittest.TestCase):
    def test_current_repository_passes(self) -> None:
        self.assertEqual(size_check.check_contract(), [])

    def test_allows_existing_debt_to_shrink(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            relative = "Ravo/desktop/qml/existing.qml"
            manifest = write_manifest(root, [debt_entry(relative, 2002)])
            write_qml(root, relative, 2001)
            self.assertEqual(size_check.check_contract(root, manifest), [])

    def test_rejects_new_oversized_qml(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = write_manifest(root, [])
            write_qml(root, "Ravo/desktop/qml/new.qml", 2001)
            errors = size_check.check_contract(root, manifest)
            self.assertTrue(any("new oversized QML file" in error for error in errors))

    def test_rejects_debt_growth_and_retired_entries(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            relative = "Ravo/desktop/qml/debt.qml"
            manifest = write_manifest(root, [debt_entry(relative, 2001)])
            write_qml(root, relative, 2002)
            self.assertTrue(
                any("QML debt grew" in error for error in size_check.check_contract(root, manifest))
            )
            write_qml(root, relative, 2000)
            self.assertTrue(
                any(
                    "retired QML debt" in error
                    for error in size_check.check_contract(root, manifest)
                )
            )


if __name__ == "__main__":
    unittest.main()
