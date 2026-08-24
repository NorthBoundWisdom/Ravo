#!/usr/bin/env python3
# Usage:
#   python3 hooks/install.py
#   python3 hooks/install.py --existing backup
#   python3 hooks/install.py --existing replace

"""Install the shared FreeCM Git hooks into this Ravo checkout."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
FREECM_ROOT = REPO_ROOT / "FreeCM"
FREECM_HOOKS = FREECM_ROOT / "hooks"
if str(FREECM_ROOT) not in sys.path:
    sys.path.insert(0, str(FREECM_ROOT))

if not (FREECM_HOOKS / "install.py").is_file():
    raise SystemExit(
        "FreeCM/hooks/install.py is missing; run: git submodule update --init FreeCM"
    )

import hooks.install as freecm_install  # noqa: E402
from freecm.git_repositories import git_toplevel  # noqa: E402


def main(argv: list[str] | None = None) -> int:
    args = freecm_install.build_parser().parse_args(sys.argv[1:] if argv is None else argv)

    freecm_install.print_header("Installing Git hooks...")
    print()

    try:
        repo_root = git_toplevel(REPO_ROOT)
    except (OSError, subprocess.CalledProcessError) as error:
        freecm_install.print_error(f"Cannot resolve the Ravo repository root: {error}")
        return 1
    if repo_root != REPO_ROOT:
        freecm_install.print_error(
            f"hooks/install.py belongs to {REPO_ROOT}, not {repo_root}"
        )
        return 1

    host_hooks = REPO_ROOT / "hooks"
    try:
        hooks_dir = freecm_install.get_hooks_dir(repo_root)
    except (freecm_install.HookInstallError, OSError, subprocess.CalledProcessError) as error:
        freecm_install.print_error(f"Cannot resolve Git hooks directory: {error}")
        return 1

    cfg = freecm_install.load_path_config(host_hooks)
    if cfg is None:
        return 1
    freecm_install.print_info(
        f"{freecm_install.colorize('Using config:', freecm_install.GRAY)} "
        f"{host_hooks / freecm_install.PATH_INI_FILENAME}"
    )
    if not freecm_install.apply_tool_paths_from_ini(repo_root, cfg):
        return 1
    print()

    freecm_install.print_info(
        f"{freecm_install.colorize('Copying FreeCM hook files to', freecm_install.GRAY)} "
        f"{hooks_dir}"
    )
    try:
        freecm_install.install_hooks(
            FREECM_HOOKS, hooks_dir, existing_policy=args.existing
        )
    except (freecm_install.HookInstallError, OSError, ValueError) as error:
        freecm_install.print_error(str(error))
        return 1

    print()
    if freecm_install.should_configure_git_from_ini(cfg):
        freecm_install.configure_git_local()
    else:
        freecm_install.print_warn(
            "Skipping git local config "
            f"(set USE_GIT_CONFIG=true in {freecm_install.PATH_INI_FILENAME} to enable)."
        )

    print()
    freecm_install.print_ok("Git hooks installation completed!")
    print()
    print("Now when you use 'git commit':")
    print("1. C/C++ files under Ravo/ will be formatted with clang-format")
    print(
        "2. QML/JS files under Ravo/ will be formatted when qmlformat is configured"
    )
    print("3. Text files will be normalized to LF without trailing whitespace")
    print("4. Files larger than 15MB will be blocked from committing")
    print("5. Commit message template will be displayed automatically")
    print("6. Commit message format will be validated against [type]: description")
    print()
    print(
        "Supported types: feat, fix, refactor, style, docs, test, chore, perf, "
        "ci, build, enhancement"
    )
    print()
    print(
        f"Requirements: python3/python and a valid {freecm_install.PATH_INI_FILENAME} "
        "in hooks/."
    )
    print(
        f"Edit {freecm_install.PATH_INI_FILENAME} to change tool paths and rerun "
        "hooks/install.py."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
