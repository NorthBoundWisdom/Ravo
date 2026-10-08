#!/usr/bin/env python3
"""Native package launch checks, intended for disposable CI runners without Qt SDKs."""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import sys


def run_logged(command: list[str], *, env: dict[str, str], work: Path, name: str) -> None:
    """Keep loader diagnostics even when startup crashes or times out."""
    with (work / f"{name}.log").open("w", encoding="utf-8") as log:
        log.write(f"command={command!r}\n")
        log.flush()
        result = subprocess.run(command, cwd=work, env=env, stdout=log,
                                stderr=subprocess.STDOUT, timeout=300, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"{name} exited {result.returncode}; see {work / (name + '.log')}")


def run_native_smoke(artifact: Path, studio: Path, cli: Path, work: Path,
                     env: dict[str, str]) -> None:
    """Launch native Qt, never substitute offscreen or an extracted AppImage.

    DEB installation/purge is explicitly authorized by --require-native-smoke;
    callers must use a disposable Linux host. Existing installations are refused.
    """
    env = dict(env)
    env.pop("QT_QPA_PLATFORM", None)
    for key in ("APPIMAGE_EXTRACT_AND_RUN", "APPIMAGE", "APPDIR", "OWD"):
        env.pop(key, None)
    env["QT_DEBUG_PLUGINS"] = "1"
    # Software rendering bounds this check to startup and native platform plugins;
    # it does not qualify GPU drivers. The startup entry point rejects offscreen
    # and must present a native frame before exiting successfully.
    env["QT_QUICK_BACKEND"] = "software"
    env["QSG_RHI_BACKEND"] = "software"
    platform = {"darwin": "cocoa", "win32": "windows", "linux": "xcb"}[sys.platform]
    if sys.platform == "linux" and not env.get("DISPLAY"):
        raise RuntimeError("native Linux smoke requires DISPLAY (run under xvfb-run)")
    launch = studio
    installed = False
    try:
        if artifact.suffix.lower() == ".deb":
            if Path("/opt/RavoStudio").exists() or Path("/usr/bin/ravo_studio").exists():
                raise RuntimeError("refuse to replace an existing Ravo installation")
            package = subprocess.check_output(
                ["dpkg-deb", "--field", str(artifact), "Package"], text=True).strip()
            if package != "ravostudio":
                raise RuntimeError(f"unexpected Debian package identity: {package}")
            installed = True  # Even a partial dpkg failure needs cleanup.
            run_logged(["sudo", "-n", "dpkg", "--install", str(artifact)],
                       env=env, work=work, name="native-deb-install")
            launch = Path("/usr/bin/ravo_studio")
            run_logged(["/usr/bin/ravo", "--help"], env=env, work=work, name="native-cli")
        else:
            run_logged([str(cli), "--help"], env=env, work=work, name="native-cli")
        if artifact.name.endswith(".AppImage"):
            launch = work / "native-launch.AppImage"
            shutil.copy2(artifact, launch)
            launch.chmod(launch.stat().st_mode | 0o100)
        run_logged([str(launch), "--startup-smoke", "-platform", platform],
                   env=env, work=work, name="native-studio")
        marker = '{"type":"ravo.native_startup","version":1,"first_frame":true}'
        if marker not in (work / "native-studio.log").read_text(encoding="utf-8", errors="replace"):
            raise RuntimeError("native startup exited without first-frame evidence")
    finally:
        if installed:
            run_logged(["sudo", "-n", "dpkg", "--purge", "ravostudio"],
                       env=env, work=work, name="native-deb-purge")
