#!/usr/bin/env python3
"""
同步 zh_translate.ini 与 RavoStudio_zh_CN.ts 文件

使用示例:
    python3 4_update_zh_trans_ini.py zh_translate.ini RavoStudio_zh_CN.ts

功能:
    1. 将当前 TS 条目放在文件前段，并按 TS 出现顺序更新
    2. 添加 TS 中不存在的翻译项（作为未翻译项）
    3. 将已经退出当前 TS 的条目保留为历史翻译记忆
"""
import html
import argparse
import re
import sys
from pathlib import Path
from collections import Counter


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


def extract_entries_from_ts(ts_file_path):
    """从 .ts 文件中提取所有 (context, source)（按出现顺序）"""
    entries = []

    if not ts_file_path.exists():
        print(f"Error: Translation file not found: {ts_file_path}")
        return entries

    with open(ts_file_path, "r", encoding="utf-8") as f:
        content = f.read()

    context_pattern = r"<context>(.*?)</context>"
    source_pattern = r"<source>(.*?)</source>"
    for context_block in re.findall(context_pattern, content, re.DOTALL):
        name_match = re.search(r"<name>(.*?)</name>", context_block, re.DOTALL)
        context_name = html.unescape(name_match.group(1)) if name_match else ""
        for src in re.findall(source_pattern, context_block, re.DOTALL):
            source_text = html.unescape(src)
            if source_text:
                normalized_source = normalize_text_for_matching(source_text)
                context_source = (
                    f"{context_name}::{source_text}" if context_name else source_text
                )
                normalized_context_source = normalize_text_for_matching(context_source)
                entries.append(
                    {
                        "context": context_name,
                        "source": source_text,
                        "normalized_source": normalized_source,
                        "context_source": context_source,
                        "normalized_context_source": normalized_context_source,
                    }
                )
    return entries


def normalize_text_for_matching(text):
    """规范化文本格式，用于匹配比较
    将实际的换行符转换为 \n 字符串，以便与 ini 文件中的格式匹配
    注意：保留头尾空格，不做 strip 处理
    """
    if not text:
        return ""
    # 将实际的换行符转换为 \n 字符串（用于匹配）
    # 这样 "text\nmore" 和 "text\\nmore" 可以匹配
    # 保留头尾空格，不做 strip 处理
    text = text.replace("\n", "\\n")
    return text


def load_ini_translations(ini_file_path, known_keys=None):
    """从 INI 文件加载翻译映射"""
    translations = {}
    key_counter = Counter()

    if not ini_file_path.exists():
        return translations, key_counter

    with open(ini_file_path, "r", encoding="utf-8") as f:
        for line_num, raw_line in enumerate(f, 1):
            # 去掉行尾的换行符，但保留行首空格
            line = raw_line.rstrip("\n\r")
            if not line or line.lstrip().startswith("#"):
                continue

            split_result = split_ini_line(line, known_keys)
            if split_result is None:
                print(f"Warning: line {line_num} has no '=' delimiter, skipped: {line}")
                continue

            source_text, chinese_translation = split_result

            # 保留原始文本，包括头尾空格（不做 strip 处理）
            # 规范化源文本用于匹配（保持 \n 字符串格式，不转换为实际换行符）
            # 这样可以直接与 .ts 文件中提取的规范化文本匹配
            normalized_source = normalize_text_for_matching(source_text)
            if normalized_source or source_text:  # 允许空文本（如果源文本本身是空的）
                translations[normalized_source] = chinese_translation
                key_counter[normalized_source] += 1

    return translations, key_counter


def escape_ini_text(text):
    """转义 INI 文件中的换行符"""
    # 将换行符转义为 \n
    return text.replace("\n", "\\n")


def selected_key(entry, source_count):
    """Return the persistent key for one active TS entry."""
    if source_count[entry["normalized_source"]] > 1:
        return entry["normalized_context_source"]
    return entry["normalized_source"]


def update_ini_file(ini_file_path, ts_entries, existing_translations):
    """更新 INI 文件
    ts_sources: [(原始文本, 规范化文本), ...] 元组列表
    注意：保留所有出现的条目，包括重复的（按照 .ts 文件中的顺序）
    """
    # 创建新的条目列表（按 .ts 文件中的顺序）
    new_entries = []
    # 用于跟踪已使用的翻译，避免重复查找
    translation_cache = {}

    # 按照 .ts 文件中的顺序添加条目（不去重，保留所有出现）
    source_count = Counter([e["normalized_source"] for e in ts_entries])
    active_keys = [selected_key(entry, source_count) for entry in ts_entries]
    active_key_set = set(active_keys)

    for entry in ts_entries:
        original_source = entry["source"]
        normalized_source = entry["normalized_source"]
        normalized_context_source = entry["normalized_context_source"]
        is_ambiguous = source_count[normalized_source] > 1
        # 歧义 source 使用 context::source 作为首选 key，非歧义仍使用 source key。
        persistent_key = (
            normalized_context_source
            if is_ambiguous
            else normalized_source
        )
        fallback_key = normalized_source

        if persistent_key not in translation_cache:
            translation = existing_translations.get(persistent_key, "")
            if not translation and fallback_key != persistent_key:
                translation = existing_translations.get(fallback_key, "")
            translation_cache[persistent_key] = translation
        else:
            translation = translation_cache[persistent_key]

        # 转义换行符（写入文件时，使用原始文本）
        write_source = entry["context_source"] if is_ambiguous else original_source
        escaped_source = escape_ini_text(write_source)

        # 如果翻译为空，使用 <unfinished> 标记
        if not translation:
            escaped_translation = "<unfinished>"
        else:
            escaped_translation = escape_ini_text(translation)

        new_entries.append(f"{escaped_source}={escaped_translation}")

    historical_entries = [
        (source, translation)
        for source, translation in existing_translations.items()
        if source not in active_key_set
    ]

    # 写入当前条目，随后保留退出当前 TS 的翻译记忆。
    with open(ini_file_path, "w", encoding="utf-8") as f:
        for entry in new_entries:
            f.write(entry + "\n")
        if historical_entries:
            f.write("\n# Historical translations retained for future reuse.\n")
            for source, translation in historical_entries:
                f.write(f"{escape_ini_text(source)}={escape_ini_text(translation)}\n")

    return len(new_entries), len(historical_entries)


def main():
    parser = argparse.ArgumentParser(
        description="同步 zh_translate.ini 与 RavoStudio_zh_CN.ts 文件"
    )
    parser.add_argument("ini_file_path", help="ini 文件路径")
    parser.add_argument("ts_file_path", help="ts 文件路径")
    parser.add_argument(
        "--debug",
        action="store_true",
        help="输出调试信息（默认关闭）",
    )
    args = parser.parse_args()

    ini_file_path = Path(args.ini_file_path).resolve()
    ts_file_path = Path(args.ts_file_path).resolve()

    if not ini_file_path.exists():
        print(f"Error: INI file not found: {ini_file_path}")
        return 1

    if not ts_file_path.exists():
        print(f"Error: Translation file not found: {ts_file_path}")
        return 1

    if ts_file_path.suffix.lower() != ".ts":
        print(f"Error: File is not a .ts translation file: {ts_file_path}")
        return 1

    print(f"Reading sources from: {ts_file_path}")
    ts_entries = extract_entries_from_ts(ts_file_path)
    print(f"Found {len(ts_entries)} source texts in .ts file")
    normalized_known_keys = set()
    for entry in ts_entries:
        normalized_known_keys.add(entry["normalized_source"])
        normalized_known_keys.add(entry["normalized_context_source"])

    print(f"Loading existing translations from: {ini_file_path}")
    existing_translations, existing_key_counter = load_ini_translations(
        ini_file_path, normalized_known_keys
    )
    print(f"Found {len(existing_translations)} existing translations")
    duplicate_keys = [(k, c) for k, c in existing_key_counter.items() if c > 1]
    if duplicate_keys:
        print(
            f"Warning: Found {len(duplicate_keys)} duplicated ini keys (showing top 10):"
        )
        for k, c in sorted(duplicate_keys, key=lambda x: (-x[1], x[0]))[:10]:
            print(f"  {c}x {repr(k)}")

    # 提取当前条目的稳定 key，用于准确统计上下文歧义项。
    source_count = Counter([entry["normalized_source"] for entry in ts_entries])
    active_keys = [selected_key(entry, source_count) for entry in ts_entries]
    active_key_set = set(active_keys)
    if args.debug:
        debug_keys = ["Series A", "System", "Touch Force"]
        print(f"\nDebug: Checking specific keys:")
        for key in debug_keys:
            normalized_key = normalize_text_for_matching(key)
            in_ts = normalized_key in active_key_set
            in_ini = normalized_key in existing_translations
            print(
                f"  '{key}': in_ts={in_ts}, in_ini={in_ini}, normalized='{normalized_key}'"
            )
            if in_ini:
                print(f"    Translation: '{existing_translations[normalized_key]}'")

    # 统计变化
    added_count = 0
    kept_count = 0
    reused_count = 0

    # 检查哪些条目会被添加
    for entry in ts_entries:
        source = selected_key(entry, source_count)
        fallback_key = entry["normalized_source"]
        if source in existing_translations:
            kept_count += 1
        elif fallback_key in existing_translations:
            reused_count += 1
        else:
            added_count += 1
    historical_count = sum(1 for source in existing_translations if source not in active_key_set)

    print(f"\nChanges:")
    print(f"  - Will add: {added_count} entries")
    print(f"  - Will keep: {kept_count} entries")
    print(f"  - Will reuse fallback translations: {reused_count} entries")
    print(f"  - Will retain historical translations: {historical_count} entries")

    # 更新文件
    total_entries, historical_entries = update_ini_file(
        ini_file_path, ts_entries, existing_translations
    )

    print(f"\n[OK] Updated {ini_file_path}")
    print(f"Active entries: {total_entries}")
    print(f"Historical entries: {historical_entries}")

    return 0


if __name__ == "__main__":
    exit(main())
