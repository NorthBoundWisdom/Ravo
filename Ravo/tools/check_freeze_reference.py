#!/usr/bin/env python3
"""Verify leftover 0.9 trees still match the freeze, minus retired owners."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path
from typing import Any


DEFAULT_FREEZE_COMMIT = "320970bf7c9cbbc6611cfc3eb60f8f2b0424b782"
# Current path -> path at the freeze commit.
PROTECTED_PATHS: tuple[tuple[str, str], ...] = (
    ("legacy/src", "src"),
    ("legacy/tests", "darktable-tests"),
    ("legacy/host/cmake", "cmake"),
    ("legacy/host/data", "data"),
    ("legacy/host/packaging", "packaging"),
)
RETIRED_SRC_LIST = Path("DevDocs/phase0/legacy-retired-src-paths.txt")
JPEG_WRAPPER_CONSUMERS = Path(
    "Ravo/tests/fixtures/legacy_jpeg_wrapper_consumers.json"
)
PNG_WRAPPER_CONSUMERS = Path(
    "Ravo/tests/fixtures/legacy_png_wrapper_consumers.json"
)
TIFF_WRAPPER_CONSUMERS = Path(
    "Ravo/tests/fixtures/legacy_tiff_wrapper_consumers.json"
)
QOI_WRAPPER_CONSUMERS = Path(
    "Ravo/tests/fixtures/legacy_qoi_wrapper_consumers.json"
)
RGBE_WRAPPER_CONSUMERS = Path(
    "Ravo/tests/fixtures/legacy_rgbe_wrapper_consumers.json"
)
LIBRAW_WRAPPER_CONSUMERS = Path(
    "Ravo/tests/fixtures/legacy_libraw_wrapper_consumers.json"
)
DNG_OPCODE_WRAPPER_CONSUMERS = Path(
    "Ravo/tests/fixtures/legacy_dng_opcode_wrapper_consumers.json"
)
DNG_WRITER_CONSUMERS = Path(
    "Ravo/tests/fixtures/legacy_dng_writer_consumers.json"
)
# Leftover CMake registries may drop retired add_iop / add_library lines.
# Their blobs are not freeze-identical after an accepted retirement.
MUTABLE_LEFTOVER_SRC_PATHS = {
    "iop/CMakeLists.txt",
    "libs/CMakeLists.txt",
}
CPP_SOURCE_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".m",
    ".mm",
}
JPEG_WRAPPER_INCLUDE = re.compile(
    r'^\s*#\s*include\s*(?:<(?:[^<>\r\n/]+/)*imageio_jpeg\.h>'
    r'|"(?:[^"\r\n/]+/)*imageio_jpeg\.h")',
    re.MULTILINE,
)
PNG_WRAPPER_INCLUDE = re.compile(
    r'^\s*#\s*include\s*(?:<(?:[^<>\r\n/]+/)*imageio_png\.h>'
    r'|"(?:[^"\r\n/]+/)*imageio_png\.h")',
    re.MULTILINE,
)
TIFF_WRAPPER_INCLUDE = re.compile(
    r'^\s*#\s*include\s*(?:<(?:[^<>\r\n/]+/)*imageio_tiff\.h>'
    r'|"(?:[^"\r\n/]+/)*imageio_tiff\.h")',
    re.MULTILINE,
)
QOI_WRAPPER_INCLUDE = re.compile(
    r'^\s*#\s*include\s*(?:<(?:[^<>\r\n/]+/)*(?:imageio_qoi|qoi)\.h>'
    r'|"(?:[^"\r\n/]+/)*(?:imageio_qoi|qoi)\.h")',
    re.MULTILINE,
)
RGBE_WRAPPER_INCLUDE = re.compile(
    r'^\s*#\s*include\s*(?:<(?:[^<>\r\n/]+/)*imageio_rgbe\.h>'
    r'|"(?:[^"\r\n/]+/)*imageio_rgbe\.h")',
    re.MULTILINE,
)
LIBRAW_WRAPPER_INCLUDE = re.compile(
    r'^\s*#\s*include\s*(?:<(?:[^<>\r\n/]+/)*imageio_libraw\.h>'
    r'|"(?:[^"\r\n/]+/)*imageio_libraw\.h")',
    re.MULTILINE,
)
DNG_OPCODE_WRAPPER_INCLUDE = re.compile(
    r'^\s*#\s*include\s*(?:<(?:[^<>\r\n/]+/)*dng_opcode\.h>'
    r'|"(?:[^"\r\n/]+/)*dng_opcode\.h")',
    re.MULTILINE,
)
DNG_WRITER_INCLUDE = re.compile(
    r'^\s*#\s*include\s*(?:<(?:[^<>\r\n/]+/)*imageio_dng\.h>'
    r'|"(?:[^"\r\n/]+/)*imageio_dng\.h")',
    re.MULTILINE,
)


class FreezeCheckError(Exception):
    """Raised when a protected path differs from the recorded freeze reference."""


def _read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as error:
        raise FreezeCheckError(f"cannot read {path}: {error}") from error


def _strings(value: Any, field: str, label: str) -> list[str]:
    if not isinstance(value, list) or any(
        not isinstance(item, str) or not item for item in value
    ):
        raise FreezeCheckError(f"{label} manifest {field} must be a string list")
    if len(value) != len(set(value)):
        raise FreezeCheckError(f"{label} manifest {field} contains duplicates")
    return value


def _entries(
    document: dict[str, Any],
    key: str,
    strings: tuple[str, ...],
    lists: tuple[str, ...] = (),
    label: str = "wrapper",
) -> list[dict[str, Any]]:
    value = document.get(key)
    if not isinstance(value, list):
        raise FreezeCheckError(f"{label} manifest {key} must be a list")
    for index, entry in enumerate(value):
        if not isinstance(entry, dict) or any(
            not isinstance(entry.get(field), str) or not entry[field]
            for field in strings
        ):
            raise FreezeCheckError(f"{label} manifest {key}[{index}] is malformed")
        for field in lists:
            _strings(entry.get(field), f"{key}[{index}].{field}", label)
    paths = [entry["path"] for entry in value]
    if len(paths) != len(set(paths)):
        raise FreezeCheckError(f"{label} manifest {key} repeats a path")
    return value


def load_wrapper_manifest(
    repository_root: Path, manifest_path: Path, label: str
) -> dict[str, Any]:
    path = repository_root / manifest_path
    try:
        document = json.loads(_read_text(path))
    except json.JSONDecodeError as error:
        raise FreezeCheckError(f"cannot parse {path}: {error}") from error
    if not isinstance(document, dict) or document.get("schema_version") != 1:
        raise FreezeCheckError(f"{label} wrapper manifest must use schema version 1")
    wrapper = document.get("wrapper")
    registration = wrapper.get("cmake_registration") if isinstance(wrapper, dict) else None
    if not isinstance(wrapper, dict) or not isinstance(registration, dict):
        raise FreezeCheckError(f"{label} wrapper manifest wrapper is malformed")
    for field in ("todo_owner", "source", "header"):
        if not isinstance(wrapper.get(field), str) or not wrapper[field]:
            raise FreezeCheckError(f"{label} manifest wrapper.{field} is malformed")
    for field in ("path", "needle"):
        if not isinstance(registration.get(field), str) or not registration[field]:
            raise FreezeCheckError(
                f"{label} manifest wrapper.cmake_registration.{field} is malformed"
            )
    exclusive_resources = _strings(
        wrapper.get("exclusive_resources", []),
        "wrapper.exclusive_resources",
        label,
    )
    owner_paths = [wrapper["source"], wrapper["header"], *exclusive_resources]
    if len(owner_paths) != len(set(owner_paths)):
        raise FreezeCheckError(f"{label} manifest repeats a wrapper owner path")
    public = set(_strings(document.get("public_symbols"), "public_symbols", label))
    consumers = _entries(
        document,
        "known_consumers",
        ("path", "todo_owner", "why_reachable"),
        ("symbols",),
        label,
    )
    includes = _entries(
        document,
        "known_include_only",
        ("path", "todo_owner", "why_reachable"),
        label=label,
    )
    separate = _entries(
        document,
        "separate_owners",
        ("path", "todo_owner", "classification", "why_not_a_consumer"),
        ("local_identifier_collisions",),
        label,
    )
    if not public or not consumers:
        raise FreezeCheckError(f"{label} manifest must name public symbols and consumers")
    if any(not set(entry["symbols"]) <= public for entry in consumers):
        raise FreezeCheckError(f"{label} manifest consumer contains a non-public symbol")
    classified = [entry["path"] for entry in consumers + includes + separate]
    if len(classified) != len(set(classified)):
        raise FreezeCheckError(f"{label} manifest classifies a path more than once")
    if any(set(entry["local_identifier_collisions"]) & public for entry in separate):
        raise FreezeCheckError(f"{label} manifest confuses a public symbol with a local name")
    return document


def load_jpeg_wrapper_manifest(repository_root: Path) -> dict[str, Any]:
    return load_wrapper_manifest(repository_root, JPEG_WRAPPER_CONSUMERS, "JPEG")


def load_png_wrapper_manifest(repository_root: Path) -> dict[str, Any]:
    return load_wrapper_manifest(repository_root, PNG_WRAPPER_CONSUMERS, "PNG")


def load_tiff_wrapper_manifest(repository_root: Path) -> dict[str, Any]:
    return load_wrapper_manifest(repository_root, TIFF_WRAPPER_CONSUMERS, "TIFF")


def load_qoi_wrapper_manifest(repository_root: Path) -> dict[str, Any]:
    return load_wrapper_manifest(repository_root, QOI_WRAPPER_CONSUMERS, "QOI")


def load_rgbe_wrapper_manifest(repository_root: Path) -> dict[str, Any]:
    return load_wrapper_manifest(repository_root, RGBE_WRAPPER_CONSUMERS, "RGBE")


def load_libraw_wrapper_manifest(repository_root: Path) -> dict[str, Any]:
    return load_wrapper_manifest(repository_root, LIBRAW_WRAPPER_CONSUMERS, "LibRaw")


def load_dng_opcode_wrapper_manifest(repository_root: Path) -> dict[str, Any]:
    return load_wrapper_manifest(
        repository_root, DNG_OPCODE_WRAPPER_CONSUMERS, "DNG opcode"
    )


def load_dng_writer_manifest(repository_root: Path) -> dict[str, Any]:
    return load_wrapper_manifest(repository_root, DNG_WRITER_CONSUMERS, "DNG writer")


def _files(repository_root: Path, *, cmake: bool) -> list[Path]:
    result: list[Path] = []
    for base in (repository_root / "legacy/src", repository_root / "Ravo"):
        if not base.is_dir():
            continue
        result.extend(
            path
            for path in base.rglob("*")
            if path.is_file()
            and (
                path.name == "CMakeLists.txt" or path.suffix.lower() == ".cmake"
                if cmake
                else path.suffix.lower() in CPP_SOURCE_SUFFIXES
            )
        )
    return sorted(result)


def verify_wrapper_consumers(
    repository_root: Path,
    manifest: dict[str, Any],
    include_pattern: re.Pattern[str],
    label: str,
) -> dict[str, Any]:
    wrapper = manifest["wrapper"]
    source, header = wrapper["source"], wrapper["header"]
    exclusive_resources = wrapper.get("exclusive_resources", [])
    owner_paths = {source, header, *exclusive_resources}
    registration = wrapper["cmake_registration"]
    pattern = re.compile(
        r"\b(?:" + "|".join(map(re.escape, manifest["public_symbols"])) + r")\b"
    )
    symbols: dict[str, set[str]] = {}
    includes: set[str] = set()
    for path in _files(repository_root, cmake=False):
        relative = path.relative_to(repository_root).as_posix()
        if relative in owner_paths:
            continue
        text = _read_text(path)
        found = {match.group() for match in pattern.finditer(text)}
        if found:
            symbols[relative] = found
        if include_pattern.search(text):
            includes.add(relative)

    known = {entry["path"]: entry for entry in manifest["known_consumers"]}
    include_only = {entry["path"]: entry for entry in manifest["known_include_only"]}
    unknown = sorted(
        f"{path}:{symbol}"
        for path, found in symbols.items()
        for symbol in found
        if path not in known or symbol not in known[path]["symbols"]
    )
    if unknown:
        raise FreezeCheckError(
            f"unclassified {label} wrapper symbol consumers exist: "
            + ", ".join(unknown)
        )
    unknown = sorted(includes - set(known) - set(include_only))
    if unknown:
        raise FreezeCheckError(
            f"unclassified {label} wrapper header includes exist: "
            + ", ".join(unknown)
        )
    missing_includes = sorted(set(symbols) - includes)
    if missing_includes:
        raise FreezeCheckError(
            f"{label} wrapper callers omit its header: " + ", ".join(missing_includes)
        )

    hits: dict[str, int] = {}
    exact_registration_count = 0
    for path in _files(repository_root, cmake=True):
        relative = path.relative_to(repository_root).as_posix()
        text = _read_text(path)
        count = text.count(Path(source).name)
        if count:
            hits[relative] = count
        if relative == registration["path"]:
            exact_registration_count = text.count(registration["needle"])
    unknown = sorted(set(hits) - {registration["path"]})
    if unknown:
        raise FreezeCheckError(
            f"unclassified {label} wrapper CMake registrations exist: "
            + ", ".join(unknown)
        )
    if hits.get(registration["path"], 0) != exact_registration_count:
        raise FreezeCheckError(f"{label} wrapper has a noncanonical CMake registration")
    registration_count = exact_registration_count
    if registration_count > 1:
        raise FreezeCheckError(f"{label} wrapper CMake registration is duplicated")

    owner_presence = {
        path: (repository_root / path).is_file() for path in sorted(owner_paths)
    }
    owner_exists = any(owner_presence.values())
    if owner_exists and not all(owner_presence.values()):
        raise FreezeCheckError(f"{label} wrapper owner retirement is partial")

    blockers = sorted(symbols)
    if blockers:
        if not owner_exists or registration_count != 1:
            raise FreezeCheckError(
                f"{label} wrapper or registration was retired while consumers remain"
            )
        state = "blocked"
    else:
        if includes:
            raise FreezeCheckError(
                f"{label} wrapper has no callers; retire its stale includes and owner together"
            )
        if owner_exists or registration_count:
            raise FreezeCheckError(
                f"{label} wrapper has no callers and must be retired with its registration"
            )
        state = "retired"

    return {
        "state": state,
        "todo_owner": wrapper["todo_owner"],
        "source": source,
        "header": header,
        "exclusive_resources": [
            {"path": path, "present": owner_presence[path]}
            for path in exclusive_resources
        ],
        "cmake_registration": {
            "path": registration["path"],
            "present": registration_count == 1,
        },
        "blocking_consumers": [
            {
                "path": path,
                "symbols": sorted(symbols[path]),
                "todo_owner": known[path]["todo_owner"],
                "why_reachable": known[path]["why_reachable"],
            }
            for path in blockers
        ],
        "include_only": [
            {
                "path": path,
                "todo_owner": (known.get(path) or include_only[path])["todo_owner"],
                "why_reachable": (known.get(path) or include_only[path])[
                    "why_reachable"
                ],
            }
            for path in sorted(includes - set(blockers))
        ],
        "separate_owners": [
            {**entry, "exists": (repository_root / entry["path"]).is_file()}
            for entry in manifest["separate_owners"]
        ],
    }


def verify_jpeg_wrapper_consumers(repository_root: Path) -> dict[str, Any]:
    return verify_wrapper_consumers(
        repository_root,
        load_jpeg_wrapper_manifest(repository_root),
        JPEG_WRAPPER_INCLUDE,
        "JPEG",
    )


def verify_png_wrapper_consumers(repository_root: Path) -> dict[str, Any]:
    return verify_wrapper_consumers(
        repository_root,
        load_png_wrapper_manifest(repository_root),
        PNG_WRAPPER_INCLUDE,
        "PNG",
    )


def verify_tiff_wrapper_consumers(repository_root: Path) -> dict[str, Any]:
    return verify_wrapper_consumers(
        repository_root,
        load_tiff_wrapper_manifest(repository_root),
        TIFF_WRAPPER_INCLUDE,
        "TIFF",
    )


def verify_qoi_wrapper_consumers(repository_root: Path) -> dict[str, Any]:
    return verify_wrapper_consumers(
        repository_root,
        load_qoi_wrapper_manifest(repository_root),
        QOI_WRAPPER_INCLUDE,
        "QOI",
    )


def verify_rgbe_wrapper_consumers(repository_root: Path) -> dict[str, Any]:
    return verify_wrapper_consumers(
        repository_root,
        load_rgbe_wrapper_manifest(repository_root),
        RGBE_WRAPPER_INCLUDE,
        "RGBE",
    )


def verify_libraw_wrapper_consumers(repository_root: Path) -> dict[str, Any]:
    return verify_wrapper_consumers(
        repository_root,
        load_libraw_wrapper_manifest(repository_root),
        LIBRAW_WRAPPER_INCLUDE,
        "LibRaw",
    )


def verify_dng_opcode_wrapper_consumers(repository_root: Path) -> dict[str, Any]:
    return verify_wrapper_consumers(
        repository_root,
        load_dng_opcode_wrapper_manifest(repository_root),
        DNG_OPCODE_WRAPPER_INCLUDE,
        "DNG opcode",
    )


def verify_dng_writer_consumers(repository_root: Path) -> dict[str, Any]:
    return verify_wrapper_consumers(
        repository_root,
        load_dng_writer_manifest(repository_root),
        DNG_WRITER_INCLUDE,
        "DNG writer",
    )


def run_git(repository_root: Path, *arguments: str) -> str:
    completed = subprocess.run(
        ("git", *arguments),
        cwd=repository_root,
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise FreezeCheckError(f"git {' '.join(arguments)} failed: {detail}")
    return completed.stdout.rstrip("\n")


def load_retired_src_paths(repository_root: Path) -> set[str]:
    path = repository_root / RETIRED_SRC_LIST
    if not path.is_file():
        raise FreezeCheckError(f"missing retired-path list: {RETIRED_SRC_LIST}")
    retired: set[str] = set()
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        retired.add(line)
    return retired


def list_tree(repository_root: Path, spec: str) -> set[str]:
    output = run_git(repository_root, "ls-tree", "-r", "--name-only", spec)
    return {line for line in output.splitlines() if line}


def blob_id(repository_root: Path, spec: str) -> str:
    return run_git(repository_root, "rev-parse", spec)


def verify_exact_tree(
    repository_root: Path, freeze: str, current_path: str, freeze_path: str
) -> str:
    frozen_tree = blob_id(repository_root, f"{freeze}:{freeze_path}")
    current_tree = blob_id(repository_root, f"HEAD:{current_path}")
    if frozen_tree != current_tree:
        raise FreezeCheckError(
            f"committed {current_path} object differs from freeze path {freeze_path}: "
            f"{frozen_tree} != {current_tree}"
        )
    return frozen_tree


def verify_src_with_retirements(
    repository_root: Path, freeze: str, retired: set[str]
) -> str:
    frozen_files = list_tree(repository_root, f"{freeze}:src")
    current_files = list_tree(repository_root, "HEAD:legacy/src")
    unknown_retired = sorted(retired - frozen_files)
    if unknown_retired:
        raise FreezeCheckError(
            "retired src paths are not in the freeze tree: " + ", ".join(unknown_retired)
        )
    unexpected_deleted = sorted(frozen_files - current_files - retired)
    if unexpected_deleted:
        raise FreezeCheckError(
            "committed legacy/src is missing files that are not retired: "
            + ", ".join(unexpected_deleted)
        )
    unexpected_added = sorted(current_files - frozen_files)
    if unexpected_added:
        raise FreezeCheckError(
            "committed legacy/src has files that are not in the freeze tree: "
            + ", ".join(unexpected_added)
        )
    for relative in sorted(current_files):
        if relative in MUTABLE_LEFTOVER_SRC_PATHS:
            continue
        frozen_blob = blob_id(repository_root, f"{freeze}:src/{relative}")
        current_blob = blob_id(repository_root, f"HEAD:legacy/src/{relative}")
        if frozen_blob != current_blob:
            raise FreezeCheckError(
                f"committed leftover legacy/src/{relative} differs from freeze src/{relative}"
            )
    return blob_id(repository_root, f"{freeze}:src")


def porcelain_path(line: str) -> str:
    if len(line) < 4:
        return ""
    path = line[3:]
    if " -> " in path:
        path = path.split(" -> ", 1)[1]
    return path.strip().strip('"')


def verify_worktree(
    repository_root: Path, current_paths: list[str], retired: set[str]
) -> None:
    worktree_changes = run_git(
        repository_root,
        "status",
        "--porcelain",
        "--untracked-files=all",
        "--",
        *current_paths,
    )
    unexpected: list[str] = []
    for line in worktree_changes.splitlines():
        path = porcelain_path(line)
        prefix = line[:2] if len(line) >= 2 else ""
        relative = path[len("legacy/src/") :] if path.startswith("legacy/src/") else ""
        deleted = "D" in prefix
        if path.startswith("legacy/src/") and deleted and relative in retired:
            continue
        if path.startswith("legacy/src/") and relative in MUTABLE_LEFTOVER_SRC_PATHS:
            continue
        unexpected.append(path or line)
    if unexpected:
        raise FreezeCheckError(
            "working-tree changes exist under frozen paths: " + ", ".join(unexpected)
        )


def verify(
    repository_root: Path, freeze_commit: str
) -> tuple[
    dict[str, str],
    dict[str, Any],
    dict[str, Any],
    dict[str, Any],
    dict[str, Any],
    dict[str, Any],
    dict[str, Any],
    dict[str, Any],
    dict[str, Any],
]:
    freeze = run_git(repository_root, "rev-parse", f"{freeze_commit}^{{commit}}")
    run_git(repository_root, "merge-base", "--is-ancestor", freeze, "HEAD^{commit}")
    retired = load_retired_src_paths(repository_root)

    trees: dict[str, str] = {}
    current_paths: list[str] = []
    for current_path, freeze_path in PROTECTED_PATHS:
        if current_path == "legacy/src":
            trees[current_path] = verify_src_with_retirements(
                repository_root, freeze, retired
            )
        else:
            trees[current_path] = verify_exact_tree(
                repository_root, freeze, current_path, freeze_path
            )
        current_paths.append(current_path)
    verify_worktree(repository_root, current_paths, retired)
    jpeg_wrapper = verify_jpeg_wrapper_consumers(repository_root)
    png_wrapper = verify_png_wrapper_consumers(repository_root)
    tiff_wrapper = verify_tiff_wrapper_consumers(repository_root)
    qoi_wrapper = verify_qoi_wrapper_consumers(repository_root)
    rgbe_wrapper = verify_rgbe_wrapper_consumers(repository_root)
    libraw_wrapper = verify_libraw_wrapper_consumers(repository_root)
    dng_opcode_wrapper = verify_dng_opcode_wrapper_consumers(repository_root)
    dng_writer = verify_dng_writer_consumers(repository_root)
    return (
        trees,
        jpeg_wrapper,
        png_wrapper,
        tiff_wrapper,
        qoi_wrapper,
        rgbe_wrapper,
        libraw_wrapper,
        dng_opcode_wrapper,
        dng_writer,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repository-root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="Ravo repository root (default: inferred from this script)",
    )
    parser.add_argument(
        "--freeze-commit",
        default=DEFAULT_FREEZE_COMMIT,
        help="immutable 0.9 freeze commit (default: %(default)s)",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="emit the verified freeze and current JPEG/PNG/TIFF/QOI/RGBE/LibRaw/DNG wrapper censuses as JSON",
    )
    arguments = parser.parse_args()
    try:
        (
            trees,
            jpeg_wrapper,
            png_wrapper,
            tiff_wrapper,
            qoi_wrapper,
            rgbe_wrapper,
            libraw_wrapper,
            dng_opcode_wrapper,
            dng_writer,
        ) = verify(arguments.repository_root.resolve(), arguments.freeze_commit)
    except FreezeCheckError as error:
        print(f"freeze reference check failed: {error}")
        return 1
    if arguments.json:
        print(
            json.dumps(
                {
                    "freeze_commit": arguments.freeze_commit,
                    "protected_trees": trees,
                    "legacy_jpeg_wrapper": jpeg_wrapper,
                    "legacy_png_wrapper": png_wrapper,
                    "legacy_tiff_wrapper": tiff_wrapper,
                    "legacy_qoi_wrapper": qoi_wrapper,
                    "legacy_rgbe_wrapper": rgbe_wrapper,
                    "legacy_libraw_wrapper": libraw_wrapper,
                    "legacy_dng_opcode_wrapper": dng_opcode_wrapper,
                    "legacy_dng_writer": dng_writer,
                },
                indent=2,
                sort_keys=True,
            )
        )
        return 0
    print(
        f"freeze reference verified: commit={arguments.freeze_commit} "
        f"legacy/src={trees['legacy/src']} legacy/tests={trees['legacy/tests']} "
        f"protected_paths={len(PROTECTED_PATHS)} "
        f"jpeg_wrapper={jpeg_wrapper['state']} "
        f"jpeg_blockers={len(jpeg_wrapper['blocking_consumers'])} "
        f"png_wrapper={png_wrapper['state']} "
        f"png_blockers={len(png_wrapper['blocking_consumers'])} "
        f"tiff_wrapper={tiff_wrapper['state']} "
        f"tiff_blockers={len(tiff_wrapper['blocking_consumers'])} "
        f"qoi_wrapper={qoi_wrapper['state']} "
        f"qoi_blockers={len(qoi_wrapper['blocking_consumers'])} "
        f"rgbe_wrapper={rgbe_wrapper['state']} "
        f"rgbe_blockers={len(rgbe_wrapper['blocking_consumers'])} "
        f"libraw_wrapper={libraw_wrapper['state']} "
        f"libraw_blockers={len(libraw_wrapper['blocking_consumers'])} "
        f"dng_opcode_wrapper={dng_opcode_wrapper['state']} "
        f"dng_opcode_blockers={len(dng_opcode_wrapper['blocking_consumers'])} "
        f"dng_writer={dng_writer['state']} "
        f"dng_writer_blockers={len(dng_writer['blocking_consumers'])}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
