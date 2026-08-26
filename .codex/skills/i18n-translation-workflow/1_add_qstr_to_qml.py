#!/usr/bin/env python3
"""
批量将 QML 文件中的硬编码字符串替换为 qsTr() 调用
"""
# example:
# cd <repo-root>
# python add_qstr_to_qml.py
# python add_qstr_to_qml.py ../CustomQml
# python .codex/skills/i18n-translation-workflow/1_add_qstr_to_qml.py Ravo/desktop/qml
# python .codex/skills/i18n-translation-workflow/1_add_qstr_to_qml.py Ravo/desktop/qml/Main.qml

import os
import re
import sys
from pathlib import Path

from _runtime import find_repo_root

# 需要翻译的属性列表
TRANSLATABLE_PROPERTIES = [
    "text",
    "title",
    "label",
    "tooltip",
    "placeholderText",
    "displayText",
]

# 排除的模式（不需要翻译的字符串）
EXCLUDE_PATTERNS = [
    r"^[a-z]",  # 以小写字母开头（通常是变量名或属性名）
    r"^qrc:",  # 资源路径
    r"^:/",  # 资源路径
    r"^http",  # URL
    r"^[A-Z][a-z]+[A-Z]",  # 驼峰命名（可能是组件名）
    r"^\d+",  # 纯数字
    r"^[A-Z_]+$",  # 全大写（可能是常量）
    r"^[a-z_]+$",  # 全小写（可能是变量名）
    r"\.svg$",  # SVG 文件名
    r"\.png$",  # PNG 文件名
    r"\.jpg$",  # JPG 文件名
    r"\.qml$",  # QML 文件名
    r"objectName",  # objectName 属性
    r"actionName",  # actionName 属性
    r"iconSource",  # iconSource 属性
    r"id:",  # id 定义
    r"^Ravo Studio(?:\s|$)",  # Product name
]


def should_translate(text):
    """判断字符串是否需要翻译"""
    text = text.strip("\"'")

    # 太短的字符串不翻译
    if len(text) < 2:
        return False
    if re.fullmatch(r"(?:\\u[0-9A-Fa-f]{4}|\\U[0-9A-Fa-f]{8})+", text):
        return False

    # 检查排除模式
    for pattern in EXCLUDE_PATTERNS:
        if re.search(pattern, text):
            return False

    # 包含至少一个字母的字符串才需要翻译
    if not re.search(r"[A-Za-z]", text):
        return False

    # 已经包含 qsTr 的不处理
    return True


def process_qml_file(file_path):
    """处理单个 QML 文件"""
    try:
        with open(file_path, "r", encoding="utf-8") as f:
            content = f.read()
    except Exception as e:
        print(f"Error reading {file_path}: {e}")
        return False

    original_content = content
    modified = False

    # 处理各种属性
    for prop in TRANSLATABLE_PROPERTIES:
        # 匹配 pattern: property: "string"
        pattern = rf'({prop})\s*:\s*"([^"]+)"'

        def replace_func(match):
            prop_name = match.group(1)
            text = match.group(2)

            # 如果已经是 qsTr()，跳过
            if "qsTr(" in content[max(0, match.start() - 20) : match.start()]:
                return match.group(0)

            if should_translate(text):
                return f'{prop_name}: qsTr("{text}")'
            return match.group(0)

        new_content = re.sub(pattern, replace_func, content)
        if new_content != content:
            content = new_content
            modified = True

    # 处理 CustomToolTip text 特殊格式
    pattern = r'CustomToolTip\s*\{[^{}]*\btext:\s*"([^"]+)"'

    def replace_tooltip(match):
        text = match.group(1)
        if should_translate(text):
            return match.group(0).replace(f'text: "{text}"', f'text: qsTr("{text}")')
        return match.group(0)

    new_content = re.sub(pattern, replace_tooltip, content)
    if new_content != content:
        content = new_content
        modified = True

    if modified and content != original_content:
        try:
            with open(file_path, "w", encoding="utf-8") as f:
                f.write(content)
            print(f"Updated: {file_path}")
            return True
        except Exception as e:
            print(f"Error writing {file_path}: {e}")
            return False

    return False


def main():
    if len(sys.argv) > 1:
        target_path = Path(sys.argv[1]).resolve()
    else:
        target_path = (find_repo_root() / "Ravo" / "desktop" / "qml").resolve()

    # 显示传入的路径
    print(f"Target path: {target_path}")

    if not target_path.exists():
        print(f"Error: Path not found: {target_path}")
        return 1

    # 判断是文件还是目录
    if target_path.is_file():
        if target_path.suffix.lower() != ".qml":
            print(f"Error: Not a QML file: {target_path}")
            return 1
        qml_files = [target_path]
        print(f"Processing single QML file: {target_path.name}")
    elif target_path.is_dir():
        qml_files = list(target_path.rglob("*.qml"))
        print(f"Found {len(qml_files)} QML files in directory")
    else:
        print(f"Error: Invalid path: {target_path}")
        return 1

    if not qml_files:
        print("No QML files found.")
        return 0

    updated_count = 0
    for qml_file in qml_files:
        if process_qml_file(qml_file):
            updated_count += 1

    print(f"\nUpdated {updated_count} files")
    print(
        "\nNote: Please review the changes and run lupdate to update translation files:"
    )
    print(
        "  python3 .codex/skills/i18n-translation-workflow/2_update_translations.py "
        "Ravo/desktop /path/to/lupdate"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
