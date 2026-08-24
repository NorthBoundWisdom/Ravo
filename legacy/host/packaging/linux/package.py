#!/usr/bin/env python3
"""Build a relocatable GTK AppDir and package it as an AppImage."""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess  # nosec B404
import sys
from pathlib import Path
from typing import Any

from repomgrcpp.package.common import (
    PackageError,
    clean_dir,
    copy_file,
    copy_tree,
    log,
    run_command,
)


PREFIX = "package_linux"
SYSTEM_LIBRARY_PREFIXES = (
    "ld-linux",
    "libanl.so",
    "libc.so",
    "libdl.so",
    "libm.so",
    "libnss_",
    "libpthread.so",
    "libresolv.so",
    "librt.so",
    "libthread_db.so",
    "libutil.so",
    "linux-vdso.so",
)
LICENSE_PATTERNS = ("COPYING*", "COPYRIGHT*", "LICENSE*", "LICENCE*", "NOTICE*")


def nested_string(data: dict[str, Any], dotted_key: str, *, required: bool = True) -> str:
    value: Any = data
    for part in dotted_key.split("."):
        if not isinstance(value, dict) or part not in value:
            if required:
                raise PackageError(f"Missing package config value: {dotted_key}")
            return ""
        value = value[part]
    if not isinstance(value, str) or (required and not value):
        raise PackageError(f"Invalid package config value: {dotted_key}")
    return value


def config_path(data: dict[str, Any], dotted_key: str, *, required: bool = True) -> Path:
    value = nested_string(data, dotted_key, required=required)
    if not value:
        raise PackageError(f"Missing package config path: {dotted_key}")
    return Path(value).resolve()


def optional_config_path(data: dict[str, Any], dotted_key: str) -> Path | None:
    value = nested_string(data, dotted_key, required=False)
    return Path(value).resolve() if value else None


def config_path_list(data: dict[str, Any], dotted_key: str) -> list[Path]:
    value: Any = data
    for part in dotted_key.split("."):
        if not isinstance(value, dict) or part not in value:
            return []
        value = value[part]
    if not isinstance(value, list) or any(
        not isinstance(item, str) or not item for item in value
    ):
        raise PackageError(f"Invalid package config path list: {dotted_key}")
    return [Path(item).resolve() for item in value]


def load_config(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise PackageError(f"Unable to read Linux package config {path}: {exc}") from exc
    if not isinstance(data, dict):
        raise PackageError(f"Invalid Linux package config {path}: expected object")
    return data


def require_child(parent: Path, child: Path, *, label: str) -> None:
    parent = parent.resolve()
    child = child.resolve()
    if child == parent or not child.is_relative_to(parent):
        raise PackageError(f"Invalid {label}: expected a child of {parent}, got {child}")


def is_elf(path: Path) -> bool:
    if not path.is_file() or path.is_symlink():
        return False
    try:
        with path.open("rb") as handle:
            return handle.read(4) == b"\x7fELF"
    except OSError:
        return False


def elf_files(root: Path) -> list[Path]:
    return sorted(path for path in root.rglob("*") if is_elf(path))


def should_skip_system_library(name: str) -> bool:
    return name.startswith(SYSTEM_LIBRARY_PREFIXES)


def parse_ldd(binary: Path, library_dirs: list[Path]) -> list[tuple[str, Path | None]]:
    environment = os.environ.copy()
    inherited = environment.get("LD_LIBRARY_PATH", "")
    environment["LD_LIBRARY_PATH"] = os.pathsep.join(
        [*(str(path) for path in library_dirs), inherited]
    ).rstrip(os.pathsep)
    completed = subprocess.run(  # nosec B603
        ["ldd", str(binary)],
        capture_output=True,
        text=True,
        check=False,
        env=environment,
    )
    output = (completed.stdout or "") + (completed.stderr or "")
    if "statically linked" in output or "not a dynamic executable" in output:
        return []
    if completed.returncode != 0:
        raise PackageError(f"ldd failed ({completed.returncode}) while inspecting {binary}: {output.strip()}")

    result: list[tuple[str, Path | None]] = []
    for raw_line in output.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        if "=>" in line:
            name, resolved = (part.strip() for part in line.split("=>", 1))
            if resolved.startswith("not found"):
                result.append((name, None))
                continue
            path_value = re.sub(r"\s+\(0x[0-9a-fA-F]+\)$", "", resolved)
            if path_value.startswith("/"):
                result.append((name, Path(path_value).resolve()))
            continue
        path_value = re.sub(r"\s+\(0x[0-9a-fA-F]+\)$", "", line)
        if path_value.startswith("/"):
            path = Path(path_value).resolve()
            result.append((path.name, path))
    return result


def copy_library(source: Path, dependency_name: str, lib_dir: Path) -> Path:
    real_source = source.resolve()
    real_target = lib_dir / real_source.name
    dependency_target = lib_dir / dependency_name
    lib_dir.mkdir(parents=True, exist_ok=True)

    if not real_target.exists():
        shutil.copy2(real_source, real_target)
    if dependency_target != real_target and not dependency_target.exists():
        dependency_target.symlink_to(real_target.name)
    return real_target


def collect_runtime_closure(
    data: dict[str, Any], app_dir: Path
) -> tuple[set[Path], set[Path]]:
    usr_dir = app_dir / "usr"
    lib_dir = usr_dir / "lib"
    library_dirs = [
        lib_dir,
        lib_dir / "darktable",
        *(path for path in config_path_list(data, "linux.runtimeSearchDirs") if path.is_dir()),
    ]
    queue = elf_files(usr_dir)
    staged_by_name = {path.name: path for path in queue}
    inspected: set[Path] = set()
    runtime_sources: set[Path] = set()

    while queue:
        binary = queue.pop(0).resolve()
        if binary in inspected:
            continue
        inspected.add(binary)
        for name, source in parse_ldd(binary, library_dirs):
            if source is None:
                raise PackageError(f"Unresolved ELF dependency for {binary}: {name}")
            staged = staged_by_name.get(name)
            if staged is not None:
                queue.append(staged)
                continue
            if should_skip_system_library(name):
                continue
            if source == usr_dir or source.is_relative_to(usr_dir):
                staged_by_name[name] = source
                queue.append(source)
                continue
            deployed = copy_library(source, name, lib_dir)
            staged_by_name[name] = deployed
            staged_by_name[deployed.name] = deployed
            runtime_sources.add(source.resolve())
            queue.append(deployed)

    for binary in elf_files(usr_dir):
        for name, source in parse_ldd(binary, library_dirs):
            if source is None:
                raise PackageError(f"Unresolved packaged ELF dependency for {binary}: {name}")
            if name in staged_by_name or should_skip_system_library(name):
                continue
            if source == usr_dir or source.is_relative_to(usr_dir):
                continue
            raise PackageError(f"ELF dependency escaped the AppDir for {binary}: {name} -> {source}")

    log(
        f"validated {len(inspected)} ELF files and staged {len(runtime_sources)} runtime libraries",
        prefix=PREFIX,
    )
    return inspected, runtime_sources


def copy_optional_tree(source: Path | None, destination: Path) -> None:
    if source is not None and source.is_dir():
        copy_tree(source, destination, required=False, prefix=PREFIX)


def copy_gtk_runtime(data: dict[str, Any], app_dir: Path) -> None:
    usr_dir = app_dir / "usr"
    lib_dir = usr_dir / "lib"
    libexec_dir = usr_dir / "libexec" / "darktablenext"
    gtk_version = nested_string(data, "linux.gtkBinaryVersion")

    loader_dir = config_path(data, "linux.gdkPixbufLoaderDir")
    copy_tree(
        loader_dir,
        lib_dir / "gdk-pixbuf-2.0" / "2.10.0" / "loaders",
        required=True,
        prefix=PREFIX,
    )
    copy_file(config_path(data, "linux.gdkPixbufQueryLoaders"), libexec_dir, prefix=PREFIX)

    gtk_lib_dir = config_path(data, "linux.gtkLibDir")
    gtk_module_root = gtk_lib_dir / "gtk-3.0" / gtk_version
    copy_tree(
        gtk_module_root / "immodules",
        lib_dir / "gtk-3.0" / gtk_version / "immodules",
        required=True,
        prefix=PREFIX,
    )
    copy_optional_tree(
        gtk_module_root / "printbackends",
        lib_dir / "gtk-3.0" / gtk_version / "printbackends",
    )
    copy_file(config_path(data, "linux.gtkQueryImmodules"), libexec_dir, prefix=PREFIX)

    copy_optional_tree(
        optional_config_path(data, "linux.gioModuleDir"),
        lib_dir / "gio" / "modules",
    )
    copy_optional_tree(
        optional_config_path(data, "linux.glibSchemaDir"),
        usr_dir / "share" / "glib-2.0" / "schemas",
    )
    copy_optional_tree(
        optional_config_path(data, "linux.iconThemeDir"),
        usr_dir / "share" / "icons",
    )
    copy_optional_tree(
        optional_config_path(data, "linux.fontconfigDir"),
        usr_dir / "etc" / "fonts",
    )

    schemas_dir = usr_dir / "share" / "glib-2.0" / "schemas"
    if schemas_dir.is_dir() and any(schemas_dir.glob("*.gschema.xml")):
        run_command(
            [str(config_path(data, "linux.glibCompileSchemas")), str(schemas_dir)],
            prefix=PREFIX,
        )


def copy_source_licenses(data: dict[str, Any], app_dir: Path) -> None:
    linux = data.get("linux")
    entries = linux.get("sourceLicenses", []) if isinstance(linux, dict) else []
    if not isinstance(entries, list):
        raise PackageError("Invalid linux.sourceLicenses; expected array")
    license_root = app_dir / "usr" / "share" / "licenses"
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            raise PackageError(f"Invalid linux.sourceLicenses[{index}]; expected object")
        source = entry.get("source")
        destination = entry.get("destination")
        if not isinstance(source, str) or not isinstance(destination, str):
            raise PackageError(f"Invalid linux.sourceLicenses[{index}] paths")
        target = (license_root / destination).resolve()
        require_child(license_root, target, label=f"linux.sourceLicenses[{index}].destination")
        source_path = Path(source).resolve()
        if not source_path.is_file():
            raise PackageError(f"Required source license not found: {source_path}")
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source_path, target)


def copy_homebrew_licenses(data: dict[str, Any], runtime_sources: set[Path], app_dir: Path) -> None:
    cellar = optional_config_path(data, "linux.homebrewCellar")
    if cellar is None or not cellar.is_dir():
        return
    formula_roots: set[Path] = set()
    for source in runtime_sources:
        try:
            relative = source.resolve().relative_to(cellar)
        except ValueError:
            continue
        if len(relative.parts) >= 2:
            formula_roots.add(cellar / relative.parts[0] / relative.parts[1])

    destination_root = app_dir / "usr" / "share" / "licenses" / "homebrew"
    copied = 0
    for formula_root in sorted(formula_roots):
        destination = destination_root / formula_root.parent.name
        seen: set[Path] = set()
        for pattern in LICENSE_PATTERNS:
            for source in sorted(formula_root.glob(pattern)):
                if source.is_file() and source not in seen:
                    destination.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source, destination / source.name)
                    seen.add(source)
                    copied += 1
    log(f"collected {copied} Homebrew runtime license files", prefix=PREFIX)


def write_launcher(data: dict[str, Any], app_dir: Path) -> None:
    app_name = nested_string(data, "app.name")
    display_name = nested_string(data, "app.displayName")
    gtk_version = nested_string(data, "linux.gtkBinaryVersion")
    app_run = app_dir / "AppRun"
    app_run.write_text(
        f'''#!/bin/sh
set -eu
HERE="$(dirname "$(readlink -f "$0")")"
PREFIX="$HERE/usr"
export PREFIX
export LD_LIBRARY_PATH="$PREFIX/lib:$PREFIX/lib/darktable${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}"
export PATH="$PREFIX/bin${{PATH:+:$PATH}}"
XDG_DATA_DIRS="${{XDG_DATA_DIRS:-/usr/local/share:/usr/share}}"
export XDG_DATA_DIRS="$PREFIX/share:$XDG_DATA_DIRS"
export GSETTINGS_SCHEMA_DIR="$PREFIX/share/glib-2.0/schemas"
export GTK_DATA_PREFIX="$PREFIX"
export GTK_EXE_PREFIX="$PREFIX"
export GTK_PATH="$PREFIX/lib/gtk-3.0"
export GIO_EXTRA_MODULES="$PREFIX/lib/gio/modules"
export FONTCONFIG_PATH="$PREFIX/etc/fonts"

CACHE_ROOT="${{XDG_RUNTIME_DIR:-${{TMPDIR:-/tmp}}}}"
CACHE_DIR="$(mktemp -d "$CACHE_ROOT/darktablenext-appimage.XXXXXX")"
trap 'rm -rf "$CACHE_DIR"' EXIT HUP INT TERM

"$PREFIX/libexec/darktablenext/gdk-pixbuf-query-loaders" \
  "$PREFIX/lib/gdk-pixbuf-2.0/2.10.0/loaders/"*.so > "$CACHE_DIR/loaders.cache"
"$PREFIX/libexec/darktablenext/gtk-query-immodules-3.0" \
  "$PREFIX/lib/gtk-3.0/{gtk_version}/immodules/"*.so > "$CACHE_DIR/immodules.cache"
export GDK_PIXBUF_MODULE_FILE="$CACHE_DIR/loaders.cache"
export GTK_IM_MODULE_FILE="$CACHE_DIR/immodules.cache"

"$PREFIX/bin/{app_name}" "$@"
''',
        encoding="utf-8",
    )
    app_run.chmod(0o755)

    desktop_contents = (
        "[Desktop Entry]\n"
        "Version=1.0\n"
        "Type=Application\n"
        f"Name={display_name}\n"
        f"Exec={app_name} %U\n"
        f"Icon={app_name}\n"
        "Terminal=false\n"
        "Categories=Graphics;Photography;\n"
        "StartupNotify=true\n"
        "StartupWMClass=darktable\n"
    )
    desktop = app_dir / "org.darktable.darktable.desktop"
    desktop.write_text(desktop_contents, encoding="utf-8")
    applications_dir = app_dir / "usr" / "share" / "applications"
    applications_dir.mkdir(parents=True, exist_ok=True)
    (applications_dir / desktop.name).write_text(desktop_contents, encoding="utf-8")

    metainfo_dir = app_dir / "usr" / "share" / "metainfo"
    metainfo_dir.mkdir(parents=True, exist_ok=True)
    (metainfo_dir / "org.darktable.darktable.appdata.xml").write_text(
        f'''<?xml version="1.0" encoding="UTF-8"?>
<component type="desktop-application">
  <id>org.darktable.darktable</id>
  <metadata_license>FSFAP</metadata_license>
  <project_license>GPL-3.0-or-later</project_license>
  <name>{display_name}</name>
  <summary>Photo workflow and RAW image editor</summary>
  <description>
    <p>Manage a local photo collection, develop RAW images non-destructively, and export finished files.</p>
  </description>
  <developer id="io.github.northboundwisdom">
    <name>NorthBoundWisdom</name>
  </developer>
  <launchable type="desktop-id">{desktop.name}</launchable>
  <provides>
    <binary>{app_name}</binary>
  </provides>
  <url type="homepage">https://github.com/NorthBoundWisdom/DarkTableNext</url>
  <content_rating type="oars-1.1"/>
</component>
''',
        encoding="utf-8",
    )

    icon_source = config_path(data, "linux.iconPng")
    copy_file(icon_source, app_dir, prefix=PREFIX)
    copied_icon = app_dir / icon_source.name
    icon = app_dir / f"{app_name}.png"
    if copied_icon != icon:
        copied_icon.replace(icon)
    dir_icon = app_dir / ".DirIcon"
    if dir_icon.exists() or dir_icon.is_symlink():
        dir_icon.unlink()
    dir_icon.symlink_to(icon.name)


def create_appimage(data: dict[str, Any], app_dir: Path) -> Path:
    binary_dir = config_path(data, "paths.binaryDir")
    work_dir = config_path(data, "paths.workDir")
    output = config_path(data, "paths.outputAppImage")
    tool_source = config_path(data, "linux.appImageTool")
    runtime = config_path(data, "linux.appImageRuntime")
    require_child(binary_dir, app_dir, label="paths.appDir")
    require_child(binary_dir, work_dir, label="paths.workDir")
    require_child(binary_dir, output, label="paths.outputAppImage")
    if not runtime.is_file():
        raise PackageError(f"Pinned AppImage runtime not found: {runtime}")

    clean_dir(work_dir)
    copy_file(tool_source, work_dir, prefix=PREFIX)
    tool = work_dir / tool_source.name
    tool.chmod(0o755)
    if output.exists() or output.is_symlink():
        output.unlink()

    previous = os.environ.get("APPIMAGE_EXTRACT_AND_RUN")
    os.environ["APPIMAGE_EXTRACT_AND_RUN"] = "1"
    try:
        run_command(
            [str(tool), "--runtime-file", str(runtime), str(app_dir), str(output)],
            prefix=PREFIX,
        )
    finally:
        if previous is None:
            os.environ.pop("APPIMAGE_EXTRACT_AND_RUN", None)
        else:
            os.environ["APPIMAGE_EXTRACT_AND_RUN"] = previous
    if not output.is_file():
        raise PackageError(f"appimagetool did not create expected output: {output}")
    output.chmod(0o755)
    return output


def package(config_file: Path) -> Path:
    data = load_config(config_file)
    app_dir = config_path(data, "paths.appDir")
    if not (app_dir / "usr" / "bin" / nested_string(data, "app.name")).is_file():
        raise PackageError(f"Staged application executable not found in {app_dir}")

    copy_gtk_runtime(data, app_dir)
    copy_source_licenses(data, app_dir)
    _, runtime_sources = collect_runtime_closure(data, app_dir)
    copy_homebrew_licenses(data, runtime_sources, app_dir)
    write_launcher(data, app_dir)
    output = create_appimage(data, app_dir)
    log(f"AppImage created: {output}", prefix=PREFIX)
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path)
    args = parser.parse_args()
    try:
        package(args.config.resolve())
    except PackageError as exc:
        print(f"[{PREFIX}][error] {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
