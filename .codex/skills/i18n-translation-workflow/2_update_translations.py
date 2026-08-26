#!/usr/bin/env python3
"""
更新翻译文件：运行 lupdate 提取所有 qsTr() 字符串
"""
# example:
# cd <repo-root>
# python .codex/skills/i18n-translation-workflow/2_update_translations.py Ravo/desktop
# python .codex/skills/i18n-translation-workflow/2_update_translations.py Ravo/desktop /path/to/qt/bin/lupdate
#
# 说明：此脚本会自动查找 Qt 的 lupdate 工具并运行，提取所有 QML 文件中的 qsTr() 字符串
# 到翻译文件中。必须传入要扫描的目录路径（通常是 Ravo/desktop）。

import os
import subprocess
import shutil
import sys
from pathlib import Path


def configure_stdio_for_windows():
    if os.name != "nt":
        return
    for stream_name in ("stdout", "stderr"):
        stream = getattr(sys, stream_name, None)
        if stream and hasattr(stream, "reconfigure"):
            try:
                stream.reconfigure(encoding="utf-8")  # type: ignore[attr-defined]
            except Exception:
                pass


def find_lupdate():
    """查找 lupdate 可执行文件

    优先级：
      1. 环境变量 LUPDATE_EXE
      2. PATH 中的 lupdate
    """
    env_lupdate = os.environ.get("LUPDATE_EXE")
    if env_lupdate:
        return env_lupdate

    which_lupdate = shutil.which("lupdate")
    if which_lupdate:
        return which_lupdate

    return None


def main():
    configure_stdio_for_windows()

    # 检查是否传入了路径参数
    if len(sys.argv) < 2:
        print("Error: Path argument is required.")
        print("\nUsage:")
        print(f"  python {Path(__file__).name} <path_to_scan> [lupdate_path]")
        print("\nExamples:")
        print(f"  python {Path(__file__).name} Ravo/desktop")
        print(f"  python {Path(__file__).name} Ravo/desktop /path/to/qt/bin/lupdate")
        print("\n说明：必须传入要扫描的目录路径（通常是 Ravo/desktop）")
        return 1

    # 获取传入的路径
    scan_path = Path(sys.argv[1]).resolve()

    # 显示传入的路径
    print(f"Target path: {scan_path}")

    # 验证路径是否存在
    if not scan_path.exists():
        print(f"Error: Path not found: {scan_path}")
        return 1

    if not scan_path.is_dir():
        print(f"Error: Path is not a directory: {scan_path}")
        return 1

    # 计算翻译文件的位置（假设在扫描目录的 i18n 子目录中）
    ts_dir = scan_path / "i18n"
    zh_ts = ts_dir / "RavoStudio_zh_CN.ts"
    en_ts = ts_dir / "RavoStudio_en_US.ts"

    # 验证翻译文件目录是否存在
    if not ts_dir.exists():
        print(f"Error: Translation directory not found: {ts_dir}")
        print("Please ensure the i18n directory exists in the scan path.")
        return 1

    # 第二个参数（可选）：lupdate 可执行文件路径或命令名
    if len(sys.argv) >= 3:
        # 直接使用传入的可执行文件（可以是绝对路径，也可以是命令名）
        lupdate_exe = sys.argv[2]
    else:
        lupdate_exe = find_lupdate()

    if not lupdate_exe:
        print("Error: Could not find lupdate executable.")
        print("Please install Qt LinguistTools or specify the path manually.")
        print("\nYou can run lupdate manually:")
        print(f"  lupdate {scan_path} -ts {zh_ts} {en_ts}")
        return 1

    print(f"Using lupdate: {lupdate_exe}")
    print(f"Updating translation files...")
    print(f"  Source: {scan_path}")
    print(f"  Output: {zh_ts}, {en_ts}")

    # 运行 lupdate，使用扫描路径作为工作目录（lupdate 会相对于当前目录解析路径）
    # 为了确保相对路径正确，使用扫描路径的父目录作为工作目录
    cmd = [
        lupdate_exe,
        "-silent",
        "-locations",
        "none",
        "-no-obsolete",
        str(scan_path),
        "-ts",
        str(zh_ts),
        str(en_ts),
    ]

    try:
        # 使用扫描路径作为工作目录，这样相对路径的翻译文件可以正确找到。
        # lupdate 在成功时也可能输出大量历史告警；这里仅在失败时输出 stderr。
        result = subprocess.run(
            cmd,
            cwd=str(scan_path),
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        if result.returncode != 0:
            print(f"\nError running lupdate (exit code {result.returncode}).")
            if result.stderr:
                print(result.stderr.strip())
            if result.stdout:
                print(result.stdout.strip())
            return 1
        print("\n[OK] Translation files updated successfully!")
        print(f"\nNext steps:")
        print(f"  1. Open {zh_ts} in Qt Linguist to translate strings")
        print(f"  2. Rebuild the project to generate .qm files")
        return 0
    except Exception as e:
        print(f"\nError: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
