#!/usr/bin/env python3
"""
从 JSON 配置文件中提取字符串并添加到所有翻译文件 (.ts)
"""
# example:
# cd <repo-root>
# python .codex/skills/i18n-translation-workflow/3_add_json_strings_to_ts.py Ravo/desktop/config
#
# 说明：扫描 JSON 文件提取字符串，然后添加到脚本目录下的所有 .ts 文件

import json
import html
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

from _runtime import find_repo_root


def extract_strings_from_json(json_file_path):
    """从 JSON 文件中提取 display_name 和 tooltip"""
    strings = set()

    with open(json_file_path, "r", encoding="utf-8") as f:
        data = json.load(f)

    def extract_recursive(obj, path=""):
        """递归提取字符串"""
        if isinstance(obj, dict):
            # 提取 display_name 和 tooltip
            if "display_name" in obj:
                value = obj["display_name"]
                if isinstance(value, str) and value:
                    strings.add(value)
            if "tooltip" in obj:
                value = obj["tooltip"]
                if isinstance(value, str) and value:
                    strings.add(value)

            # 递归处理所有值
            for key, value in obj.items():
                extract_recursive(value, f"{path}.{key}" if path else key)
        elif isinstance(obj, list):
            for i, item in enumerate(obj):
                extract_recursive(item, f"{path}[{i}]" if path else f"[{i}]")

    extract_recursive(data)
    return strings


def get_existing_config_strings(ts_file_path):
    """从 .ts 文件的 ConfigStrings context 中提取已存在的 source 文本（避免重复添加）"""
    existing_config_sources = set()

    if not ts_file_path.exists():
        return existing_config_sources

    try:
        tree = ET.parse(ts_file_path)
        root = tree.getroot()

        # 查找 ConfigStrings context
        for context in root.findall(".//context"):
            name_elem = context.find("name")
            if name_elem is not None and name_elem.text == "ConfigStrings":
                # 在这个 context 中查找所有 message
                for message in context.findall("message"):
                    source_elem = message.find("source")
                    if source_elem is not None and source_elem.text:
                        existing_config_sources.add(source_elem.text.strip())
                break

    except Exception as e:
        print(f"Warning: Error parsing {ts_file_path} for ConfigStrings: {e}")

    return existing_config_sources


def escape_xml_text(text):
    """转义 XML 特殊字符"""
    text = text.replace("&", "&amp;")
    text = text.replace("<", "&lt;")
    text = text.replace(">", "&gt;")
    text = text.replace("'", "&apos;")
    text = text.replace('"', "&quot;")
    return text


def reactivate_config_strings(ts_file_path, active_strings):
    """恢复仍由 JSON 配置使用、但被 lupdate 标记为 vanished 的条目。"""
    with open(ts_file_path, "r", encoding="utf-8") as f:
        content = f.read()

    context_pattern = re.compile(
        r"<context>\s*<name>ConfigStrings</name>.*?</context>", re.DOTALL
    )
    context_match = context_pattern.search(content)
    if context_match is None:
        return 0

    reactivated_count = 0
    message_pattern = re.compile(r"<message>.*?</message>", re.DOTALL)
    source_pattern = re.compile(r"<source>(.*?)</source>", re.DOTALL)
    translation_pattern = re.compile(
        r'<translation(?P<attrs>[^>]*)>(?P<text>.*?)</translation>', re.DOTALL
    )

    def reactivate_message(match):
        nonlocal reactivated_count

        message = match.group(0)
        source_match = source_pattern.search(message)
        translation_match = translation_pattern.search(message)
        if source_match is None or translation_match is None:
            return message

        source_text = html.unescape(source_match.group(1))
        attrs = translation_match.group("attrs")
        if source_text not in active_strings or 'type="vanished"' not in attrs:
            return message

        active_attrs = attrs.replace(' type="vanished"', "")
        replacement = f"<translation{active_attrs}>{translation_match.group('text')}</translation>"
        reactivated_count += 1
        return (
            message[: translation_match.start()]
            + replacement
            + message[translation_match.end() :]
        )

    updated_context = message_pattern.sub(reactivate_message, context_match.group(0))
    if reactivated_count == 0:
        return 0

    content = content[: context_match.start()] + updated_context + content[context_match.end() :]
    with open(ts_file_path, "w", encoding="utf-8") as f:
        f.write(content)

    return reactivated_count


def add_strings_to_ts(ts_file_path, new_strings):
    """将新字符串添加到 .ts 文件的 ConfigStrings context（所有翻译都标记为 unfinished）"""
    if not ts_file_path.exists():
        print(f"Error: Translation file not found: {ts_file_path}")
        return False

    # 读取文件内容
    with open(ts_file_path, "r", encoding="utf-8") as f:
        content = f.read()

    # 检查 ConfigStrings context 中是否已经有这些字符串（避免重复）
    existing_config = get_existing_config_strings(ts_file_path)
    strings_to_add = [s for s in new_strings if s not in existing_config]

    if not strings_to_add:
        return False

    # 创建一个新的 context 用于 JSON 配置字符串
    new_context_xml = []
    new_context_xml.append("<context>")
    new_context_xml.append("    <name>ConfigStrings</name>")

    for source_text in sorted(strings_to_add):
        escaped_source = escape_xml_text(source_text)

        new_context_xml.append("    <message>")
        new_context_xml.append(f"        <source>{escaped_source}</source>")
        new_context_xml.append(f'        <translation type="unfinished"></translation>')
        new_context_xml.append("    </message>")

    new_context_xml.append("</context>")

    # 检查是否已经存在 ConfigStrings context
    if "<name>ConfigStrings</name>" in content:
        # 如果存在，在 ConfigStrings context 的最后一个 </message> 之后插入新的 messages
        # 提取 message 部分（不包括 context 开始和结束标签）
        messages_xml = "\n".join(new_context_xml[2:-1])  # 跳过 context 开始和结束标签
        # 查找 ConfigStrings context 的结束位置
        # 匹配模式：ConfigStrings context 中的最后一个 </message>，后面跟着 </context>
        pattern = (
            r"(<context>\s*<name>ConfigStrings</name>.*?</message>\s*)(</context>)"
        )
        match = re.search(pattern, content, re.DOTALL)
        if match:
            # 在最后一个 </message> 之后插入新的 messages
            replacement = match.group(1) + messages_xml + "\n" + match.group(2)
            content = content[: match.start()] + replacement + content[match.end() :]
        else:
            # 如果找不到匹配，在 </TS> 之前插入新的 context
            pattern = r"(</TS>)"
            replacement = "\n".join(new_context_xml) + r"\n\1"
            content = re.sub(pattern, replacement, content)
    else:
        # 如果不存在，在 </TS> 之前插入新的 context
        pattern = r"(</TS>)"
        replacement = "\n".join(new_context_xml) + r"\n\1"
        content = re.sub(pattern, replacement, content)

    # 写回文件
    with open(ts_file_path, "w", encoding="utf-8") as f:
        f.write(content)

    return True


def main():
    if len(sys.argv) < 2:
        print("Error: Directory path argument is required.")
        print("\nUsage:")
        print(f"  python {Path(__file__).name} <directory_path>")
        print("\nExamples:")
        print(f"  python {Path(__file__).name} Ravo/desktop/config")
        print("\n说明：必须传入要扫描的文件夹路径")
        return 1

    dir_path = Path(sys.argv[1]).resolve()
    print(f"Target directory: {dir_path}")

    if not dir_path.exists():
        print(f"Error: Directory not found: {dir_path}")
        return 1

    if not dir_path.is_dir():
        print(f"Error: Path is not a directory: {dir_path}")
        return 1

    # 旧版本假设脚本与 ts 文件同目录。迁移到 .codex 后按仓库定位真正的 i18n 目录
    repo_root = find_repo_root()
    i18n_dir = repo_root / "Ravo" / "desktop" / "i18n"

    # 查找所有 JSON 文件
    json_files = list(dir_path.rglob("*.json"))
    if not json_files:
        print(f"Warning: No JSON files found in: {dir_path}")
        return 1

    print(f"Found {len(json_files)} JSON files")

    # 提取所有字符串
    all_strings = set()
    for json_file in json_files:
        try:
            strings = extract_strings_from_json(json_file)
            if strings:
                all_strings.update(strings)
                print(f"  [OK] {json_file.name}: {len(strings)} strings")
        except Exception as e:
            print(f"  [ERROR] {json_file.name}: {e}")

    print(f"\nTotal unique strings extracted: {len(all_strings)}")

    # 查找脚本目录下的所有 .ts 文件
    ts_files = list(i18n_dir.glob("*.ts"))
    if not ts_files:
        print(f"Warning: No .ts files found in: {i18n_dir}")
        return 1

    print(f"\nFound {len(ts_files)} .ts file(s)")

    # 处理所有 .ts 文件
    for ts_file in ts_files:
        print(f"\nProcessing {ts_file.name}...")

        reactivated_count = reactivate_config_strings(ts_file, all_strings)

        # 检查 ConfigStrings context 中已存在的字符串（避免重复添加）
        existing_config = get_existing_config_strings(ts_file)
        strings_to_add = [s for s in all_strings if s not in existing_config]

        if strings_to_add:
            add_strings_to_ts(ts_file, strings_to_add)
            print(f"  Added {len(strings_to_add)} strings to ConfigStrings context")
        elif reactivated_count == 0:
            print(f"  All strings already exist in ConfigStrings context")

        if reactivated_count:
            print(f"  Reactivated {reactivated_count} strings in ConfigStrings context")

    print("\n[OK] Done!")
    return 0


if __name__ == "__main__":
    sys.exit(main())
