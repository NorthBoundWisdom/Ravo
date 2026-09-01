from __future__ import annotations

import hashlib
import json
import tempfile
import unittest
from pathlib import Path

import check_test_split_inventory as inventory_check


def write_file(root: Path, relative: str, text: str) -> None:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def inventory_hash(cases: list[str]) -> str:
    return hashlib.sha256("\n".join(cases).encode("utf-8")).hexdigest()


def make_fixture(root: Path, second_source: bool = True) -> Path:
    write_file(
        root,
        "Ravo/tests/CMakeLists.txt",
        "add_executable(ravo_sample_tests\n"
        "  sample_a.cpp\n"
        + ("  sample_b.cpp\n" if second_source else "")
        + ")\n",
    )
    write_file(root, "Ravo/tests/sample_a.cpp", "TEST(Sample, First) {}\n")
    if second_source:
        write_file(root, "Ravo/tests/sample_b.cpp", "TEST(Sample, Second) {}\n")
    cases = ["Sample.First"] + (["Sample.Second"] if second_source else [])
    manifest = {
        "schema_version": 1,
        "groups": [
            {
                "sources": ["Ravo/tests/sample_a.cpp"]
                + (["Ravo/tests/sample_b.cpp"] if second_source else []),
                "case_count": len(cases),
                "case_sha256": inventory_hash(cases),
            }
        ],
    }
    write_file(root, "configs/test_split_inventory.jsonc", json.dumps(manifest))
    return Path("configs/test_split_inventory.jsonc")


class TestSplitInventoryTest(unittest.TestCase):
    def test_current_repository_passes(self) -> None:
        self.assertEqual(inventory_check.check_contract(), [])

    def test_accepts_split_cases_in_the_same_target(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = make_fixture(root)
            self.assertEqual(inventory_check.check_contract(root, manifest), [])

    def test_rejects_case_inventory_drift(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = make_fixture(root)
            write_file(root, "Ravo/tests/sample_b.cpp", "TEST(Sample, Replaced) {}\n")
            errors = inventory_check.check_contract(root, manifest)
            self.assertTrue(any("ordered case inventory drifted" in error for error in errors))

    def test_rejects_target_membership_drift(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = make_fixture(root)
            write_file(
                root,
                "Ravo/tests/CMakeLists.txt",
                "add_executable(ravo_sample_tests sample_a.cpp)\n"
                "add_executable(ravo_other_tests sample_b.cpp)\n",
            )
            errors = inventory_check.check_contract(root, manifest)
            self.assertTrue(any("target membership" in error for error in errors))

    def test_rejects_disabled_case(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = make_fixture(root, second_source=False)
            write_file(root, "Ravo/tests/sample_a.cpp", "TEST(Sample, DISABLED_First) {}\n")
            errors = inventory_check.check_contract(root, manifest)
            self.assertTrue(any("disables a preserved" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
