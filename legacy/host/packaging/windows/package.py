# Usage:
#   PYTHONPATH=FreeCM python3 packaging/windows/package.py --config <generated-package.json>

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET
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
from repomgrcpp.package.win_deploy import (
    is_api_set,
    is_system_dll,
    parse_dumpbin_deps,
)
from repomgrcpp.package.wix import generate_wix_fragment


WIX_NAMESPACE = "http://schemas.microsoft.com/wix/2006/wi"
PAYLOAD_GROUP_ID = "DarkTableNextPayload"


def nested_string(data: dict[str, Any], dotted_key: str) -> str:
    value: Any = data
    for part in dotted_key.split("."):
        if not isinstance(value, dict) or part not in value:
            raise PackageError(f"Missing package config value: {dotted_key}")
        value = value[part]
    if not isinstance(value, str) or not value:
        raise PackageError(f"Invalid package config value: {dotted_key}")
    return value


def config_path(data: dict[str, Any], dotted_key: str) -> Path:
    return Path(nested_string(data, dotted_key)).resolve()


def load_config(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise PackageError(f"Unable to read Windows package config {path}: {exc}") from exc
    if not isinstance(data, dict):
        raise PackageError(f"Invalid Windows package config {path}: expected object")
    return data


def require_child(parent: Path, child: Path, *, label: str) -> None:
    parent = parent.resolve()
    child = child.resolve()
    if child == parent or not child.is_relative_to(parent):
        raise PackageError(f"Invalid {label}: expected a child of {parent}, got {child}")


def copy_runtime_resources(runtime_root: Path, stage_dir: Path, schema_tool: Path) -> None:
    resources = (
        (runtime_root / "etc" / "fonts", stage_dir / "etc" / "fonts"),
        (
            runtime_root / "share" / "glib-2.0" / "schemas",
            stage_dir / "share" / "glib-2.0" / "schemas",
        ),
        (runtime_root / "share" / "themes", stage_dir / "share" / "themes"),
        (runtime_root / "share" / "icons", stage_dir / "share" / "icons"),
        (runtime_root / "share" / "locale", stage_dir / "share" / "locale"),
    )
    for source, destination in resources:
        if source.is_dir():
            copy_tree(source, destination, required=True, prefix="package_win")

    loader_dir = runtime_root / "lib" / "gdk-pixbuf-2.0" / "2.10.0" / "loaders"
    if loader_dir.is_dir() and any(loader_dir.glob("*.dll")):
        raise PackageError(
            "The selected gdk-pixbuf build uses external loaders; a relocatable loader cache "
            "must be configured before it can be packaged"
        )

    schemas_dir = stage_dir / "share" / "glib-2.0" / "schemas"
    if schemas_dir.is_dir():
        original_path = os.environ.get("PATH", "")
        os.environ["PATH"] = os.pathsep.join([str(runtime_root / "bin"), original_path])
        try:
            run_command([str(schema_tool), str(schemas_dir)], prefix="package_win")
        finally:
            os.environ["PATH"] = original_path


def copy_source_licenses(data: dict[str, Any], stage_dir: Path) -> None:
    windows = data.get("windows")
    if not isinstance(windows, dict):
        raise PackageError("Invalid package config section: windows")
    entries = windows.get("sourceLicenses", [])
    if not isinstance(entries, list):
        raise PackageError("Invalid windows.sourceLicenses; expected array")
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            raise PackageError(f"Invalid windows.sourceLicenses[{index}]; expected object")
        source_value = entry.get("source")
        destination_value = entry.get("destination")
        if not isinstance(source_value, str) or not isinstance(destination_value, str):
            raise PackageError(f"Invalid windows.sourceLicenses[{index}] paths")
        destination = (stage_dir / destination_value).resolve()
        require_child(stage_dir, destination, label=f"windows.sourceLicenses[{index}].destination")
        destination.parent.mkdir(parents=True, exist_ok=True)
        source = Path(source_value).resolve()
        if not source.is_file():
            raise PackageError(f"Required source license not found: {source}")
        shutil.copy2(source, destination)


def build_search_index(search_roots: list[Path]) -> dict[str, Path]:
    result: dict[str, Path] = {}
    for root in search_roots:
        if not root.is_dir():
            continue
        for candidate in sorted(root.rglob("*.dll")):
            result.setdefault(candidate.name.lower(), candidate)
    return result


def collect_runtime_closure(
    stage_dir: Path,
    build_bin_dir: Path,
    runtime_root: Path,
    binary_dir: Path,
    dumpbin: Path,
) -> set[str]:
    stage_bin = stage_dir / "bin"
    stage_bin.mkdir(parents=True, exist_ok=True)
    for library in sorted(build_bin_dir.glob("*.dll")):
        copy_file(library, stage_bin, prefix="package_win")

    search_index = build_search_index(
        [build_bin_dir, runtime_root / "bin", binary_dir / "dependency_installs"]
    )
    staged = {path.name.lower(): path for path in stage_bin.glob("*.dll")}
    queue = sorted(
        [
            path
            for path in stage_dir.rglob("*")
            if path.is_file() and path.suffix.lower() in {".exe", ".dll"}
        ]
    )
    inspected: set[Path] = set()
    system32 = Path(os.environ.get("SystemRoot", "C:/Windows")) / "System32"

    while queue:
        binary = queue.pop(0).resolve()
        if binary in inspected:
            continue
        inspected.add(binary)
        completed = subprocess.run(  # nosec B603
            [str(dumpbin), "/dependents", str(binary)],
            capture_output=True,
            text=True,
            check=False,
        )
        if completed.returncode != 0:
            raise PackageError(
                f"dumpbin failed ({completed.returncode}) while inspecting {binary}"
            )
        dependencies = parse_dumpbin_deps((completed.stdout or "") + (completed.stderr or ""))
        for dependency in dependencies:
            lower_name = dependency.lower()
            if is_api_set(dependency) or is_system_dll(dependency):
                continue
            if lower_name in staged:
                continue
            source = search_index.get(lower_name)
            if source is None and (system32 / dependency).is_file():
                continue
            if source is None:
                raise PackageError(
                    f"Runtime DLL not found for {binary.name}: {dependency}"
                )
            copy_file(source, stage_bin, prefix="package_win")
            deployed = stage_bin / source.name
            staged[lower_name] = deployed
            queue.append(deployed)

    log(
        f"validated {len(inspected)} PE files and staged {len(staged)} runtime DLLs",
        prefix="package_win",
    )
    return set(staged)


def copy_vcpkg_licenses(runtime_root: Path, runtime_dlls: set[str], stage_dir: Path) -> None:
    installed_root = runtime_root.parent
    info_dir = installed_root / "vcpkg" / "info"
    triplet = runtime_root.name
    destination_root = stage_dir / "share" / "licenses" / "vcpkg"
    copied_ports: set[str] = set()

    if not info_dir.is_dir():
        raise PackageError(f"vcpkg package metadata not found: {info_dir}")

    for manifest in sorted(info_dir.glob(f"*_{triplet}.list")):
        lines = [
            line.strip().replace("\\", "/")
            for line in manifest.read_text(encoding="utf-8").splitlines()
        ]
        package_dlls = {
            Path(line).name.lower()
            for line in lines
            if f"{triplet}/bin/" in line and line.lower().endswith(".dll")
        }
        if not package_dlls.intersection(runtime_dlls):
            continue
        copyright_entries = [
            line
            for line in lines
            if line.startswith(f"{triplet}/share/") and line.endswith("/copyright")
        ]
        for relative in copyright_entries:
            parts = Path(relative).parts
            if len(parts) < 4:
                continue
            port = parts[2]
            if port in copied_ports:
                continue
            source = installed_root / Path(relative)
            if source.is_file():
                destination = destination_root / port
                destination.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, destination / "copyright")
                copied_ports.add(port)

    log(f"collected licenses for {len(copied_ports)} vcpkg ports", prefix="package_win")


def remove_development_artifacts(stage_dir: Path) -> None:
    for pattern in ("*.lib", "*.exp", "*.pdb"):
        for artifact in stage_dir.rglob(pattern):
            artifact.unlink()
    helper = stage_dir / "bin" / "darktable-rs-identify.exe"
    if helper.exists():
        helper.unlink()
    installed_core = stage_dir / "lib" / "darktable" / "darktable.dll"
    if installed_core.exists():
        installed_core.unlink()


def write_png_icon(source_png: Path, output_ico: Path) -> None:
    payload = source_png.read_bytes()
    if not payload.startswith(b"\x89PNG\r\n\x1a\n") or len(payload) < 24:
        raise PackageError(f"Invalid PNG icon source: {source_png}")
    width, height = struct.unpack(">II", payload[16:24])
    if width > 256 or height > 256:
        raise PackageError("MSI icon source must not exceed 256x256")
    output_ico.parent.mkdir(parents=True, exist_ok=True)
    header = struct.pack("<HHH", 0, 1, 1)
    directory = struct.pack(
        "<BBBBHHII",
        0 if width == 256 else width,
        0 if height == 256 else height,
        0,
        0,
        1,
        32,
        len(payload),
        22,
    )
    output_ico.write_bytes(header + directory + payload)


def write_license_rtf(source: Path, output: Path) -> None:
    text = source.read_text(encoding="utf-8")
    escaped = text.replace("\\", r"\\").replace("{", r"\{").replace("}", r"\}")
    paragraphs = "\\par\n".join(escaped.splitlines())
    output.write_text(
        r"{\rtf1\ansi\deff0{\fonttbl{\f0 Consolas;}}\fs18 " + paragraphs + "}",
        encoding="ascii",
    )


def wix_element(parent: ET.Element, name: str, attributes: dict[str, str]) -> ET.Element:
    return ET.SubElement(parent, f"{{{WIX_NAMESPACE}}}{name}", attributes)


def write_payload_fragment(stage_dir: Path, output: Path) -> None:
    fragment_text = generate_wix_fragment(
        stage_dir,
        root_id="INSTALLFOLDER",
        prefix="DarkTableNext",
        component_group_id=PAYLOAD_GROUP_ID,
    )
    root = ET.fromstring(fragment_text)
    for component in root.iter(f"{{{WIX_NAMESPACE}}}Component"):
        component.attrib.pop("Permanent", None)
    ET.register_namespace("", WIX_NAMESPACE)
    ET.indent(root, space="  ")
    ET.ElementTree(root).write(output, encoding="utf-8", xml_declaration=True)


def write_product_wxs(
    data: dict[str, Any], output: Path, icon: Path, license_rtf: Path
) -> None:
    display_name = nested_string(data, "app.displayName")
    manufacturer = nested_string(data, "app.manufacturer")
    executable = nested_string(data, "app.executable")
    root = ET.Element(f"{{{WIX_NAMESPACE}}}Wix")
    product = wix_element(
        root,
        "Product",
        {
            "Id": "*",
            "Name": display_name,
            "Language": "1033",
            "Version": nested_string(data, "app.version"),
            "Manufacturer": manufacturer,
            "UpgradeCode": nested_string(data, "app.upgradeCode"),
        },
    )
    wix_element(
        product,
        "Package",
        {
            "InstallerVersion": "500",
            "Compressed": "yes",
            "InstallScope": "perMachine",
            "InstallPrivileges": "elevated",
            "Platform": "x64",
        },
    )
    wix_element(
        product,
        "MajorUpgrade",
        {
            "DowngradeErrorMessage": "A newer version of DarkTableNext is already installed.",
        },
    )
    wix_element(product, "MediaTemplate", {"EmbedCab": "yes", "CompressionLevel": "high"})
    condition = wix_element(
        product, "Condition", {"Message": "DarkTableNext requires 64-bit Windows."}
    )
    condition.text = "VersionNT64"
    wix_element(product, "Icon", {"Id": "ProductIcon", "SourceFile": str(icon)})
    wix_element(product, "Property", {"Id": "ARPPRODUCTICON", "Value": "ProductIcon"})
    wix_element(
        product,
        "Property",
        {"Id": "ARPURLINFOABOUT", "Value": "https://github.com/NorthBoundWisdom/DarkTableNext"},
    )
    wix_element(
        product,
        "Property",
        {"Id": "ARPHELPLINK", "Value": "https://github.com/NorthBoundWisdom/DarkTableNext/issues"},
    )
    wix_element(product, "Property", {"Id": "WIXUI_INSTALLDIR", "Value": "INSTALLFOLDER"})

    target_dir = wix_element(product, "Directory", {"Id": "TARGETDIR", "Name": "SourceDir"})
    program_files = wix_element(target_dir, "Directory", {"Id": "ProgramFiles64Folder"})
    wix_element(program_files, "Directory", {"Id": "INSTALLFOLDER", "Name": display_name})
    program_menu = wix_element(target_dir, "Directory", {"Id": "CommonProgramMenuFolder"})
    shortcut_dir = wix_element(
        program_menu, "Directory", {"Id": "ApplicationProgramsFolder", "Name": display_name}
    )
    shortcut_component = wix_element(
        shortcut_dir,
        "Component",
        {"Id": "ApplicationShortcut", "Guid": "*", "Win64": "yes"},
    )
    wix_element(
        shortcut_component,
        "Shortcut",
        {
            "Id": "ApplicationStartMenuShortcut",
            "Name": display_name,
            "Description": "Photo workflow and RAW editor",
            "Target": f"[INSTALLFOLDER]bin\\{executable}",
            "WorkingDirectory": "INSTALLFOLDER",
            "Icon": "ProductIcon",
        },
    )
    wix_element(
        shortcut_component,
        "RemoveFolder",
        {"Id": "RemoveApplicationProgramsFolder", "On": "uninstall"},
    )
    wix_element(
        shortcut_component,
        "RegistryValue",
        {
            "Root": "HKLM",
            "Key": f"Software\\{manufacturer}\\{display_name}",
            "Name": "Installed",
            "Type": "integer",
            "Value": "1",
            "KeyPath": "yes",
        },
    )

    feature = wix_element(
        product,
        "Feature",
        {
            "Id": "Complete",
            "Title": display_name,
            "Level": "1",
            "ConfigurableDirectory": "INSTALLFOLDER",
        },
    )
    wix_element(feature, "ComponentGroupRef", {"Id": PAYLOAD_GROUP_ID})
    wix_element(feature, "ComponentRef", {"Id": "ApplicationShortcut"})
    wix_element(product, "UIRef", {"Id": "WixUI_InstallDir"})
    wix_element(product, "UIRef", {"Id": "WixUI_ErrorProgressText"})
    wix_element(product, "WixVariable", {"Id": "WixUILicenseRtf", "Value": str(license_rtf)})

    ET.register_namespace("", WIX_NAMESPACE)
    ET.indent(root, space="  ")
    ET.ElementTree(root).write(output, encoding="utf-8", xml_declaration=True)


def build_msi(data: dict[str, Any], stage_dir: Path, work_dir: Path, output_msi: Path) -> None:
    candle = config_path(data, "windows.candle")
    light = config_path(data, "windows.light")
    icon_png = config_path(data, "windows.iconPng")
    source_dir = config_path(data, "paths.sourceDir")
    for tool in (candle, light):
        if not tool.is_file():
            raise PackageError(f"Required WiX tool not found: {tool}")

    icon = work_dir / "DarkTableNext.ico"
    license_rtf = work_dir / "LICENSE.rtf"
    payload_wxs = work_dir / "payload.wxs"
    product_wxs = work_dir / "product.wxs"
    payload_object = work_dir / "payload.wixobj"
    product_object = work_dir / "product.wixobj"
    write_png_icon(icon_png, icon)
    write_license_rtf(source_dir / "LICENSE", license_rtf)
    write_payload_fragment(stage_dir, payload_wxs)
    write_product_wxs(data, product_wxs, icon, license_rtf)

    run_command(
        [str(candle), "-nologo", "-arch", "x64", "-out", str(product_object), str(product_wxs)],
        prefix="package_win",
    )
    run_command(
        [str(candle), "-nologo", "-arch", "x64", "-out", str(payload_object), str(payload_wxs)],
        prefix="package_win",
    )
    output_msi.parent.mkdir(parents=True, exist_ok=True)
    if output_msi.exists():
        output_msi.unlink()
    run_command(
        [
            str(light),
            "-nologo",
            "-ext",
            "WixUIExtension",
            "-cultures:en-us",
            "-spdb",
            "-out",
            str(output_msi),
            str(product_object),
            str(payload_object),
        ],
        prefix="package_win",
    )
    digest = hashlib.sha256(output_msi.read_bytes()).hexdigest()
    log(f"created {output_msi} ({output_msi.stat().st_size} bytes)", prefix="package_win")
    log(f"sha256 {digest}", prefix="package_win")


def package_windows(config_file: Path) -> Path:
    data = load_config(config_file)
    binary_dir = config_path(data, "paths.binaryDir")
    build_bin_dir = config_path(data, "paths.buildBinDir")
    stage_dir = config_path(data, "paths.stageDir")
    work_dir = config_path(data, "paths.workDir")
    output_msi = config_path(data, "paths.outputMsi")
    runtime_root = config_path(data, "windows.runtimeRoot")
    schema_tool = config_path(data, "windows.glibCompileSchemas")
    dumpbin = config_path(data, "windows.dumpbin")
    require_child(binary_dir, stage_dir, label="paths.stageDir")
    require_child(binary_dir, work_dir, label="paths.workDir")
    require_child(binary_dir, output_msi, label="paths.outputMsi")
    if output_msi.suffix.lower() != ".msi":
        raise PackageError(f"Invalid MSI output path: {output_msi}")
    if not stage_dir.is_dir():
        raise PackageError(f"Windows package staging tree not found: {stage_dir}")

    clean_dir(work_dir)
    remove_development_artifacts(stage_dir)
    copy_runtime_resources(runtime_root, stage_dir, schema_tool)
    copy_source_licenses(data, stage_dir)
    runtime_dlls = collect_runtime_closure(
        stage_dir, build_bin_dir, runtime_root, binary_dir, dumpbin
    )
    copy_vcpkg_licenses(runtime_root, runtime_dlls, stage_dir)
    build_msi(data, stage_dir, work_dir, output_msi)
    return output_msi


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Create the DarkTableNext Windows MSI package")
    parser.add_argument("--config", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        package_windows(args.config.resolve())
    except PackageError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
