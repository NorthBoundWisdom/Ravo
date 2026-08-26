#!/usr/bin/env python3
"""
将 zh_translate.ini 中的翻译更新到 RavoStudio_zh_CN.ts 文件
"""
import html
import argparse
import re
import sys
from pathlib import Path


def split_ini_line(line, known_keys=None):
    """切分 ini 行为 key/value（优先按已知 key 精确匹配）"""
    eq_positions = [i for i, ch in enumerate(line) if ch == "="]
    if not eq_positions:
        return None

    if known_keys:
        matched = []
        for pos in eq_positions:
            key = line[:pos]
            if key in known_keys:
                matched.append((pos, key, line[pos + 1 :]))
        if matched:
            # 若多个位置都可匹配，优先更长的 key（靠右的分割点）
            matched.sort(key=lambda item: item[0], reverse=True)
            _, key, value = matched[0]
            return key, value

    # 回退：使用最右侧等号，保证 key 中可包含 "==" 等表达式
    pos = eq_positions[-1]
    return line[:pos], line[pos + 1 :]


def extract_known_ini_keys_from_ts(ts_file_path):
    """从 .ts 文件提取所有可用的 ini key（source 与 context::source）"""
    keys = set()
    if not ts_file_path.exists():
        return keys

    with open(ts_file_path, "r", encoding="utf-8") as f:
        content = f.read()

    context_pattern = r"<context>(.*?)</context>"
    source_pattern = r"<source>(.*?)</source>"
    for context_block in re.findall(context_pattern, content, re.DOTALL):
        name_match = re.search(r"<name>(.*?)</name>", context_block, re.DOTALL)
        context_name = html.unescape(name_match.group(1)) if name_match else ""
        for src in re.findall(source_pattern, context_block, re.DOTALL):
            source_text = html.unescape(src)
            if not source_text:
                continue
            source_key = source_text.replace("\n", "\\n")
            keys.add(source_key)
            if context_name:
                keys.add(f"{context_name}::{source_key}")
    return keys


def load_translations(ini_file_path, known_keys=None):
    """从 INI 文件加载翻译映射"""
    translations = {}

    if not ini_file_path.exists():
        print(f"Error: Translation file not found: {ini_file_path}")
        return translations

    with open(ini_file_path, "r", encoding="utf-8") as f:
        for line_num, raw_line in enumerate(f, 1):
            # Keep leading/trailing spaces in key/value; only drop newline chars.
            line = raw_line.rstrip("\r\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue

            split_result = split_ini_line(line, known_keys)
            if split_result is None:
                print(f"Warning: line {line_num} has no '=' delimiter, skipped: {line}")
                continue

            source_text, chinese_translation = split_result

            # 处理转义的换行符
            source_text = source_text.replace("\\n", "\n")
            chinese_translation = chinese_translation.replace("\\n", "\n")

            # 跳过未完成的翻译标记
            if (
                source_text
                and chinese_translation.strip()
                and chinese_translation.strip() != "<unfinished>"
            ):
                translations[source_text] = chinese_translation

    return translations


def escape_xml_text(text):
    """转义 XML 特殊字符"""
    # 从 ini 文件读取的文本是原始文本，直接转义即可
    # 注意：必须先转义 &，否则会破坏后续的转义
    text = text.replace("&", "&amp;")
    text = text.replace("<", "&lt;")
    text = text.replace(">", "&gt;")
    return text


def unescape_xml_text(text):
    """反转义 XML 文本"""
    return html.unescape(text)


def build_context_ranges(content):
    """提取 context 范围，用于 source 精确匹配 context::source"""
    ranges = []
    for m in re.finditer(r"<context>(.*?)</context>", content, flags=re.DOTALL):
        block = m.group(1)
        name_match = re.search(r"<name>(.*?)</name>", block, flags=re.DOTALL)
        context_name = html.unescape(name_match.group(1)) if name_match else ""
        ranges.append((m.start(), m.end(), context_name))
    return ranges


def find_context_name(position, context_ranges):
    """根据匹配位置找到所在 context 名称"""
    for start, end, name in context_ranges:
        if start <= position <= end:
            return name
    return ""


def find_translation(translations, source_text, context_name):
    """优先使用 context::source，其次回退到 source"""
    if context_name:
        context_key = f"{context_name}::{source_text}"
        if context_key in translations:
            return translations[context_key]
    return translations.get(source_text)


def update_ts_file(ts_file_path, translations, force=False):
    """更新 .ts 文件中的翻译"""
    with open(ts_file_path, "r", encoding="utf-8") as f:
        content = f.read()

    original_content = content
    update_count = 0
    context_ranges = build_context_ranges(content)

    message_pattern = re.compile(r"<message>(.*?)</message>", re.DOTALL)
    source_pattern = re.compile(r"<source>(.*?)</source>", re.DOTALL)
    translation_pattern = re.compile(
        r"<translation(?P<attrs>[^>]*)>(?P<text>.*?)</translation>",
        re.DOTALL,
    )

    def replace_func(match):
        nonlocal update_count
        body = match.group(1)
        source_match = source_pattern.search(body)
        translation_match = translation_pattern.search(body)
        if source_match is None or translation_match is None:
            return match.group(0)

        attrs = translation_match.group("attrs")
        is_unfinished = 'type="unfinished"' in attrs
        if 'type="vanished"' in attrs:
            return match.group(0)

        if not force and not is_unfinished:
            return match.group(0)

        # 解码 source XML 实体
        decoded_source = unescape_xml_text(source_match.group(1))
        context_name = find_context_name(match.start(), context_ranges)

        # 查找对应的翻译
        chinese_translation = find_translation(
            translations, decoded_source, context_name
        )
        if chinese_translation is not None:
            # 转义翻译文本中的 XML 特殊字符
            escaped_translation = escape_xml_text(chinese_translation)
            replacement = f"<translation>{escaped_translation}</translation>"
            new_body = (
                body[: translation_match.start()]
                + replacement
                + body[translation_match.end() :]
            )
            new_message = f"<message>{new_body}</message>"
            if new_message != match.group(0):
                update_count += 1
            return new_message

        return match.group(0)

    content = message_pattern.sub(replace_func, content)

    if content != original_content:
        with open(ts_file_path, "w", encoding="utf-8") as f:
            f.write(content)
        print(f"Updated {update_count} translations in: {ts_file_path}")
        return True

    if update_count == 0:
        print("No translations updated.")
    return False


def main():
    parser = argparse.ArgumentParser(
        description="将 zh_translate.ini 中的翻译更新到 .ts 文件"
    )
    parser.add_argument("ts_file_path", help=".ts 文件路径")
    parser.add_argument(
        "--force",
        action="store_true",
        help="覆盖所有 translation（不仅 unfinished）",
    )
    args = parser.parse_args()

    ts_file_path = Path(args.ts_file_path).resolve()

    if not ts_file_path.exists():
        print(f"Error: Translation file not found: {ts_file_path}")
        return 1

    if ts_file_path.suffix.lower() != ".ts":
        print(f"Error: File is not a .ts translation file: {ts_file_path}")
        return 1

    # 从同文件夹读取翻译配置文件
    ini_file_path = ts_file_path.parent / "zh_translate.ini"
    print(f"Loading translations from: {ini_file_path}")
    known_keys = extract_known_ini_keys_from_ts(ts_file_path)

    translations = load_translations(ini_file_path, known_keys)

    if not translations:
        print("Error: No translations loaded from config file.")
        return 1

    print(f"Loaded {len(translations)} translation mappings.")
    print(f"Updating: {ts_file_path}")

    if update_ts_file(ts_file_path, translations, force=args.force):
        print("\n[OK] Translations updated successfully!")
        return 0
    else:
        return 0


if __name__ == "__main__":
    exit(main())
