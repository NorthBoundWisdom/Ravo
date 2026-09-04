#!/usr/bin/env python3
"""Check that Ravo production sources retain the M1 dependency boundary."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


PRODUCTION_DIRECTORIES = (
    "foundation",
    "recipe",
    "engine",
    "domain",
    "adapters",
    "services",
    "control",
    "cli",
    "desktop",
)
SOURCE_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".qml"})
FORBIDDEN_INCLUDE_PATTERNS = (
    (re.compile(r"^\s*#\s*include\s*[<\"](?:\.\./)*src/", re.MULTILINE), "frozen src header"),
    (re.compile(r"^\s*#\s*include\s*[<\"](?:\.\./)*legacy-0\.9/", re.MULTILINE), "frozen 0.9 header"),
    (re.compile(r"^\s*#\s*include\s*[<\"](?:\.\./)*legacy/", re.MULTILINE), "leftover tree header"),
    (re.compile(r"^\s*#\s*include\s*[<\"](?:gtk|dtgtk)/", re.MULTILINE), "GTK header"),
    (re.compile(r"^\s*#\s*include\s*[<\"](?:darktable|libdarktable)", re.MULTILINE), "legacy core header"),
    (re.compile(r"^\s*#\s*include\s*[<\"](?:sqlite|sqlite3)", re.MULTILINE), "direct SQLite header"),
)
FORBIDDEN_WIDGETS_PATTERN = re.compile(
    r"\b(?:Qt6::Widgets|QWidget\w*|QtWidgets|QDialog\b|QMainWindow\b|QApplication\b)\b"
)
FORBIDDEN_SQL_PATTERN = re.compile(r"\b(?:sqlite3(?:_[A-Za-z0-9_]+)?|QSql\w*)\b")
FORBIDDEN_QML_PATTERN = re.compile(r"\b(?:Qt6::(?:Qml|Quick)\w*|QQml\w*|QQuick\w*)\b")
QT_INCLUDE_PATTERN = re.compile(r"^\s*#\s*include\s*[<\"](?:Qt|Q[A-Z])", re.MULTILINE)
QT_TOKEN_PATTERN = re.compile(r"\bQt6::([A-Za-z0-9_]+)\b")
QML_IMPORT_PATTERN = re.compile(r"^\s*import\s+([A-Za-z0-9.]+)", re.MULTILINE)
TARGET_LINK_PATTERN = re.compile(
    r"target_link_libraries\(\s*([A-Za-z0-9_]+)\s+(.*?)\)", re.DOTALL
)
FIRST_PARTY_TARGET_PATTERN = re.compile(
    r"\b(ravo(?:_foundation|_recipe|_engine|_domain|_adapters|_services|_control|_desktop|_cli|_studio)?)\b"
)
PUBLIC_RAVO_INCLUDE_PATTERN = re.compile(r'^\s*#\s*include\s*"ravo/([a-z_]+)/', re.MULTILINE)

ALLOWED_FIRST_PARTY_LINKS = {
    "ravo_foundation": frozenset(),
    "ravo_recipe": frozenset({"ravo_foundation"}),
    "ravo_engine": frozenset({"ravo_foundation", "ravo_recipe"}),
    "ravo_domain": frozenset({"ravo_foundation"}),
    "ravo_adapters": frozenset({"ravo_foundation", "ravo_recipe", "ravo_domain"}),
    "ravo_services": frozenset({"ravo_domain", "ravo_engine", "ravo_adapters"}),
    "ravo_control": frozenset({"ravo_foundation"}),
    "ravo_cli": frozenset(
        {"ravo_adapters", "ravo_engine", "ravo_services", "ravo_control"}
    ),
    "ravo": frozenset({"ravo_cli"}),
    "ravo_desktop": frozenset({"ravo_services", "ravo_adapters", "ravo_control"}),
    "ravo_studio": frozenset({"ravo_desktop"}),
}
REQUIRED_FIRST_PARTY_LINKS = {
    "ravo_foundation": frozenset(),
    "ravo_recipe": frozenset({"ravo_foundation"}),
    "ravo_engine": frozenset({"ravo_recipe"}),
    "ravo_domain": frozenset({"ravo_foundation"}),
    "ravo_adapters": frozenset({"ravo_foundation", "ravo_domain"}),
    "ravo_services": frozenset({"ravo_domain", "ravo_engine"}),
    "ravo_control": frozenset({"ravo_foundation"}),
    "ravo_cli": frozenset(
        {"ravo_adapters", "ravo_engine", "ravo_services", "ravo_control"}
    ),
    "ravo": frozenset({"ravo_cli"}),
    "ravo_desktop": frozenset({"ravo_services", "ravo_adapters", "ravo_control"}),
    "ravo_studio": frozenset({"ravo_desktop"}),
}
ALLOWED_PUBLIC_HEADER_LAYERS = {
    "foundation": frozenset({"foundation"}),
    "recipe": frozenset({"foundation", "recipe"}),
    "engine": frozenset({"foundation", "recipe", "engine"}),
    "domain": frozenset({"foundation", "domain"}),
    "adapters": frozenset({"foundation", "recipe", "domain", "adapters"}),
    "services": frozenset({"foundation", "recipe", "engine", "domain", "services"}),
    "control": frozenset({"foundation", "control"}),
    "cli": frozenset(
        {"foundation", "recipe", "engine", "domain", "adapters", "services", "control", "cli"}
    ),
    "desktop": frozenset(
        {"foundation", "recipe", "engine", "domain", "adapters", "services", "control", "desktop"}
    ),
}
ALLOWED_QT_COMPONENTS = {
    "foundation": frozenset(),
    "recipe": frozenset(),
    "engine": frozenset({"Core", "Gui", "GuiPrivate"}),
    "domain": frozenset(),
    "adapters": frozenset({"Core", "Gui", "Sql"}),
    "services": frozenset(),
    "control": frozenset({"Core", "Network"}),
    "cli": frozenset({"Core", "Network"}),
    "desktop": frozenset(
        {
            "Core",
            "Gui",
            "GuiPrivate",
            "Network",
            "Qml",
            "Quick",
            "QuickControls2",
            "QuickDialogs2",
            "QuickLayouts",
        }
    ),
}
QT_FREE_OWNERS = frozenset({"foundation", "recipe", "domain", "services"})
SQL_OWNERS = frozenset({"adapters"})
QML_OWNERS = frozenset({"desktop"})
GUI_OWNERS = frozenset({"adapters", "desktop", "engine"})
ALLOWED_QML_IMPORTS = frozenset(
    {
        "QtQuick",
        "QtQuick.Controls",
        "QtQuick.Dialogs",
        "QtQuick.Layouts",
        "QtQuick.Window",
        "GeoControls",
        "GeoControls.AppShell",
        "Ravo.Studio",
    }
)


class BoundaryError(Exception):
    """Raised when a production source crosses a forbidden M1 boundary."""


def owner_for(path: Path, repository_root: Path) -> str:
    relative = path.relative_to(repository_root)
    if len(relative.parts) < 2 or relative.parts[0] != "Ravo":
        raise BoundaryError(f"production path is outside Ravo/: {relative.as_posix()}")
    owner = relative.parts[1]
    if owner not in PRODUCTION_DIRECTORIES:
        raise BoundaryError(f"unexpected Ravo production owner: {relative.as_posix()}")
    return owner


def source_files(repository_root: Path) -> list[Path]:
    files: list[Path] = []
    for directory in PRODUCTION_DIRECTORIES:
        root = repository_root / "Ravo" / directory
        if not root.is_dir():
            raise BoundaryError(f"missing Ravo production directory: {root}")
        for path in root.rglob("*"):
            if path.is_file() and (path.suffix in SOURCE_SUFFIXES or path.name == "CMakeLists.txt"):
                files.append(path)
    return sorted(files)


def line_number(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def verify_qml(path: Path, repository_root: Path, owner: str) -> None:
    relative = path.relative_to(repository_root).as_posix()
    if owner != "desktop":
        raise BoundaryError(f"{relative} places production QML outside desktop")
    text = path.read_text(encoding="utf-8")
    for match in QML_IMPORT_PATTERN.finditer(text):
        imported = match.group(1)
        if imported not in ALLOWED_QML_IMPORTS:
            raise BoundaryError(
                f"{relative}:{line_number(text, match.start())} imports {imported}, "
                "which is outside the desktop QML allowlist"
            )


def verify_source(path: Path, repository_root: Path) -> None:
    owner = owner_for(path, repository_root)
    if path.suffix == ".qml":
        verify_qml(path, repository_root, owner)
        return
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as error:
        raise BoundaryError(f"cannot read production source {path}: {error}") from error
    relative = path.relative_to(repository_root).as_posix()
    for pattern, description in FORBIDDEN_INCLUDE_PATTERNS:
        match = pattern.search(text)
        if match:
            raise BoundaryError(f"{relative}:{line_number(text, match.start())} includes a {description}")

    if owner in QT_FREE_OWNERS:
        match = QT_INCLUDE_PATTERN.search(text)
        if match:
            raise BoundaryError(
                f"{relative}:{line_number(text, match.start())} includes Qt from a Qt-free layer"
            )

    match = FORBIDDEN_WIDGETS_PATTERN.search(text)
    if match:
        raise BoundaryError(f"{relative}:{line_number(text, match.start())} uses a Qt Widgets API")

    if owner not in SQL_OWNERS:
        match = FORBIDDEN_SQL_PATTERN.search(text)
        if match:
            raise BoundaryError(
                f"{relative}:{line_number(text, match.start())} uses a catalog database API"
            )
    elif "/include/" in path.as_posix().replace("\\", "/"):
        match = FORBIDDEN_SQL_PATTERN.search(text)
        if match:
            raise BoundaryError(
                f"{relative}:{line_number(text, match.start())} exposes a catalog database API "
                "from a public header"
            )

    if owner not in QML_OWNERS:
        match = FORBIDDEN_QML_PATTERN.search(text)
        if match:
            raise BoundaryError(f"{relative}:{line_number(text, match.start())} uses a QML/Quick API")

    if owner not in GUI_OWNERS:
        match = re.search(r"\b(?:QImage\w*|QGuiApplication|QPainter\w*)\b", text)
        if match:
            raise BoundaryError(f"{relative}:{line_number(text, match.start())} uses a Qt Gui API")


def verify_public_header_direction(path: Path, repository_root: Path) -> None:
    relative = path.relative_to(repository_root)
    if len(relative.parts) < 3 or relative.parts[0] != "Ravo" or relative.parts[2] != "include":
        return
    owner = relative.parts[1]
    allowed_layers = ALLOWED_PUBLIC_HEADER_LAYERS.get(owner)
    if allowed_layers is None:
        raise BoundaryError(f"unexpected Ravo production owner for public header: {relative.as_posix()}")
    text = path.read_text(encoding="utf-8")
    for match in PUBLIC_RAVO_INCLUDE_PATTERN.finditer(text):
        included_layer = match.group(1)
        if included_layer not in allowed_layers:
            raise BoundaryError(
                f"{relative.as_posix()}:{line_number(text, match.start())} includes ravo/{included_layer} "
                f"against the {owner} public-header direction"
            )


def verify_qt_cmake_boundary(repository_root: Path) -> None:
    for path in source_files(repository_root):
        if path.name != "CMakeLists.txt":
            continue
        owner = owner_for(path, repository_root)
        text = path.read_text(encoding="utf-8")
        qt_targets = QT_TOKEN_PATTERN.findall(text)
        if not qt_targets:
            continue
        allowed = ALLOWED_QT_COMPONENTS.get(owner, frozenset())
        unexpected = [target for target in qt_targets if target not in allowed]
        if unexpected:
            relative = path.relative_to(repository_root).as_posix()
            raise BoundaryError(
                f"{relative} links Qt components outside the {owner} allowlist: "
                + ", ".join(sorted(set(unexpected)))
            )
        if "Widgets" in qt_targets:
            relative = path.relative_to(repository_root).as_posix()
            raise BoundaryError(f"{relative} links Qt Widgets, which is forbidden")


def verify_first_party_target_direction(repository_root: Path) -> None:
    links = {target: set() for target in ALLOWED_FIRST_PARTY_LINKS}
    for path in source_files(repository_root):
        if path.name != "CMakeLists.txt":
            continue
        text = path.read_text(encoding="utf-8")
        for match in TARGET_LINK_PATTERN.finditer(text):
            target = match.group(1)
            if target not in links:
                continue
            links[target].update(FIRST_PARTY_TARGET_PATTERN.findall(match.group(2)))
    for target, actual_links in links.items():
        unexpected = actual_links - ALLOWED_FIRST_PARTY_LINKS[target]
        missing = REQUIRED_FIRST_PARTY_LINKS[target] - actual_links
        if unexpected or missing:
            details = []
            if unexpected:
                details.append("unexpected: " + ", ".join(sorted(unexpected)))
            if missing:
                details.append("missing: " + ", ".join(sorted(missing)))
            raise BoundaryError(
                f"first-party dependency direction for {target} is invalid; " + "; ".join(details)
            )


def verify_qml_locations(repository_root: Path) -> None:
    ravo_root = repository_root / "Ravo"
    for path in ravo_root.rglob("*.qml"):
        if not path.is_file():
            continue
        relative = path.relative_to(repository_root)
        if len(relative.parts) < 3 or relative.parts[1] != "desktop":
            raise BoundaryError(f"{relative.as_posix()} places production QML outside desktop")


def verify(repository_root: Path) -> None:
    for path in source_files(repository_root):
        if path.suffix in SOURCE_SUFFIXES:
            verify_source(path, repository_root)
        if path.suffix in {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp"}:
            verify_public_header_direction(path, repository_root)
    verify_qt_cmake_boundary(repository_root)
    verify_first_party_target_direction(repository_root)
    verify_qml_locations(repository_root)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repository-root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="Ravo repository root (default: inferred from this script)",
    )
    arguments = parser.parse_args()
    try:
        verify(arguments.repository_root.resolve())
    except BoundaryError as error:
        print(f"Ravo dependency boundary check failed: {error}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
