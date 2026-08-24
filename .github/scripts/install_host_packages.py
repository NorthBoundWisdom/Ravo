#!/usr/bin/env python3
"""Install host CMake packages used by Ravo CI.

This is GitHub Actions setup, not product CMake. It does not use FetchContent
inside the Ravo graph. Packages are cloned at pinned tags and installed into
an explicit prefix that later becomes part of CMAKE_PREFIX_PATH.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
from pathlib import Path

GTEST_REMOTE = "https://github.com/google/googletest.git"
GTEST_REF = "v1.17.0"
QUILL_REMOTE = "https://github.com/odygrd/quill.git"
QUILL_REF = "v12.1.0"
ZLIB_REMOTE = "https://github.com/madler/zlib.git"
ZLIB_REF = "v1.3.1"
LIBPNG_REMOTE = "https://github.com/pnggroup/libpng.git"
LIBPNG_REF = "v1.6.50"


def run(command: list[str], *, cwd: Path | None = None) -> None:
    print("+", " ".join(command), flush=True)
    subprocess.run(command, cwd=cwd, check=True)


def clone_ref(remote: str, ref: str, destination: Path) -> None:
    if destination.exists():
        shutil.rmtree(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    run(
        [
            "git",
            "clone",
            "--depth",
            "1",
            "--branch",
            ref,
            remote,
            str(destination),
        ]
    )


def cmake_install(
    source: Path,
    build: Path,
    *,
    prefix: Path,
    build_type: str,
    extra_cache: list[str] | None = None,
) -> None:
    if build.exists():
        shutil.rmtree(build)
    build.mkdir(parents=True, exist_ok=True)
    configure = [
        "cmake",
        "-S",
        str(source),
        "-B",
        str(build),
        "-G",
        "Ninja",
        f"-DCMAKE_BUILD_TYPE={build_type}",
        f"-DCMAKE_INSTALL_PREFIX={prefix}",
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
        "-DBUILD_SHARED_LIBS=OFF",
    ]
    if extra_cache:
        configure.extend(extra_cache)
    run(configure)
    run(["cmake", "--build", str(build), "--target", "install"])


def install_packages(*, work_dir: Path, prefix: Path, build_type: str, with_png_zlib: bool) -> None:
    prefix.mkdir(parents=True, exist_ok=True)
    work_dir.mkdir(parents=True, exist_ok=True)
    cmake_prefix = str(prefix).replace("\\", "/")

    if with_png_zlib:
        zlib_src = work_dir / "zlib"
        clone_ref(ZLIB_REMOTE, ZLIB_REF, zlib_src)
        cmake_install(
            zlib_src,
            work_dir / "zlib-build",
            prefix=prefix,
            build_type=build_type,
        )
        png_src = work_dir / "libpng"
        clone_ref(LIBPNG_REMOTE, LIBPNG_REF, png_src)
        cmake_install(
            png_src,
            work_dir / "libpng-build",
            prefix=prefix,
            build_type=build_type,
            extra_cache=[
                f"-DCMAKE_PREFIX_PATH={cmake_prefix}",
                f"-DZLIB_ROOT={cmake_prefix}",
                "-DPNG_SHARED=OFF",
                "-DPNG_TESTS=OFF",
            ],
        )

    gtest_src = work_dir / "googletest"
    clone_ref(GTEST_REMOTE, GTEST_REF, gtest_src)
    cmake_install(
        gtest_src,
        work_dir / "googletest-build",
        prefix=prefix,
        build_type=build_type,
        extra_cache=[
            "-DBUILD_GMOCK=ON",
            "-DINSTALL_GTEST=ON",
            "-Dgtest_force_shared_crt=ON",
        ],
    )

    quill_src = work_dir / "quill"
    clone_ref(QUILL_REMOTE, QUILL_REF, quill_src)
    cmake_install(
        quill_src,
        work_dir / "quill-build",
        prefix=prefix,
        build_type=build_type,
        extra_cache=[
            "-DQUILL_BUILD_TESTS=OFF",
            "-DQUILL_BUILD_EXAMPLES=OFF",
            "-DQUILL_ENABLE_INSTALL=ON",
        ],
    )


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--build-type", default="Debug", choices=("Debug", "Release"))
    parser.add_argument(
        "--with-png-zlib",
        action="store_true",
        help="install zlib and libpng into the same prefix (Windows CI)",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    install_packages(
        work_dir=args.work_dir.resolve(),
        prefix=args.prefix.resolve(),
        build_type=args.build_type,
        with_png_zlib=args.with_png_zlib,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
