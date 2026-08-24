#!/usr/bin/env python3
"""Manage FreeCM source roots and generated CMake presets."""

from __future__ import annotations

import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent
FREECM_ROOT = REPO_ROOT / "FreeCM"

for path in (REPO_ROOT, FREECM_ROOT):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

from configs.source_roots import *  # noqa: F401,F403,E402
from repomgrcpp.cmake_workflow import (  # noqa: E402
    CMakeDependencyBuildSpec,
    bind_cmake_workflow_script,
)
from repomgrcpp.preset_templates import resolve_preset_models as resolve_freecm_preset_models  # noqa: E402


def resolve_preset_models(*args: object, **kwargs: object):
    """Keep generated compiler presets tied to their platform toolchains."""
    lock_data = args[1] if len(args) > 1 else kwargs.get("lock_data")
    if lock_data is None:
        raise ValueError("resolve_preset_models requires the dependency lock data")
    dev_mode_from_lock_data(lock_data, path_label=REPO_ROOT / "source_roots.lock.jsonc")

    resolved = resolve_freecm_preset_models(*args, **kwargs)
    for model in (resolved.resolved_model, resolved.generated_model):
        for preset in model["configurePresets"]:
            cache = preset["cacheVariables"]
            name = preset["name"]
            environment = preset.setdefault("environment", {})
            if resolved.os_group == "mac":
                if name.startswith("mac_clang_"):
                    cache["CMAKE_C_COMPILER"] = "clang"
                    cache["CMAKE_CXX_COMPILER"] = "clang++"
                elif name.startswith("mac_gcc_"):
                    cache["CMAKE_C_COMPILER"] = "gcc-16"
                    cache["CMAKE_CXX_COMPILER"] = "g++-16"
                    environment["PATH"] = (
                        "/opt/homebrew/opt/gcc/bin:/opt/homebrew/opt/llvm/bin:$penv{PATH}"
                    )
            elif resolved.os_group == "win":
                path_prefix = (
                    "C:/Program Files/Git/usr/bin;"
                    "C:/OpenSource/vcpkg/installed/x64-windows/tools/libxml2;"
                    "C:/OpenSource/vcpkg/installed/x64-windows/tools/libxslt;"
                )
                if name.startswith("win_llvm_"):
                    path_prefix = "C:/Program Files/LLVM/bin;" + path_prefix
                # Keep lock cmakeEnvironment.PATH (Qt bin, etc.) ahead of $penv{PATH}.
                existing_path = environment.get("PATH")
                if isinstance(existing_path, str) and existing_path.strip():
                    environment["PATH"] = path_prefix + existing_path
                else:
                    environment["PATH"] = path_prefix + "$penv{PATH}"
            if name.endswith("_debug"):
                cache["BUILD_TESTING"] = "ON"
    return resolved


# Ravo consumes these source roots with add_subdirectory(); no standalone
# dependency SDKs need to be built before configure.
DEPENDENCY_BUILD_ORDER: tuple[CMakeDependencyBuildSpec, ...] = ()

WORKFLOW_SCRIPT = bind_cmake_workflow_script(
    globals(),
    repo_root=REPO_ROOT,
    repo_display_name="Ravo",
    dependency_build_order=DEPENDENCY_BUILD_ORDER,
)
if __name__ == "__main__":
    raise SystemExit(WORKFLOW_SCRIPT.main())
