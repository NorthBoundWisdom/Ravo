#!/usr/bin/env python3
"""Manage FreeCM source roots consumed by the Ravo CMake graph."""

from __future__ import annotations

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
FREECM_ROOT = REPO_ROOT / "FreeCM"
for path in (REPO_ROOT, FREECM_ROOT):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

from freecm.dependency_roots import (  # noqa: E402
    DependencyRootConfig,
    DependencyRootSpec,
    bind_dependency_root_workflow,
)


DEV_MODE_KEY = "DevMode"


def dev_mode_from_lock_data(lock_data: object, *, path_label: object) -> bool:
    """Return AppConfigs.DevMode from a lock file.

    Older ignored active locks predate AppConfigs. They retain the production
    default until a developer explicitly adds AppConfigs.DevMode.
    """
    if not isinstance(lock_data, dict):
        raise ValueError(f"Invalid source-roots lock file (expected object): {path_label}")
    if DEV_MODE_KEY in lock_data:
        raise ValueError(
            f"{DEV_MODE_KEY} must be stored in AppConfigs.{DEV_MODE_KEY} in {path_label}"
        )

    app_configs = lock_data.get("AppConfigs", {})
    if app_configs is None:
        app_configs = {}
    if not isinstance(app_configs, dict):
        raise ValueError(f"Invalid AppConfigs map in {path_label}")

    value = app_configs.get(DEV_MODE_KEY, False)
    if not isinstance(value, bool):
        raise ValueError(
            f"Invalid AppConfigs.{DEV_MODE_KEY} in {path_label}; expected boolean"
        )
    return value


DEPENDENCY_ROOT_SPECS: tuple[DependencyRootSpec, ...] = (
    DependencyRootSpec(
        dependency_name="LibRaw",
        repo_name="LibRaw",
        env_key="LIBRAW_SOURCE_ROOT",
        required_relative_paths=("libraw/libraw_version.h",),
    ),
    DependencyRootSpec(
        dependency_name="GeoControls",
        repo_name="GeoControls",
        env_key="GEOCONTROLS_SOURCE_ROOT",
        required_relative_paths=("CMakeLists.txt", "Controls/CMakeLists.txt"),
    ),
)


workflow = bind_dependency_root_workflow(
    globals(),
    DependencyRootConfig(
        repo_root=REPO_ROOT,
        dependency_root_specs=DEPENDENCY_ROOT_SPECS,
        repo_display_name="Ravo",
    ),
)


if __name__ == "__main__":
    raise SystemExit(main())
