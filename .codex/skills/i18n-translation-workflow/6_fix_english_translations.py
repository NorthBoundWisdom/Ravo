#!/usr/bin/env python3
"""
修复英文翻译文件：将所有未完成的翻译设置为与源文本相同
"""
# example:
# cd <repo-root>
# python .codex/skills/i18n-translation-workflow/6_fix_english_translations.py Ravo/desktop/i18n/RavoStudio_en_US.ts --force
#
# 说明：此脚本会将翻译文件中所有未完成的翻译（type="unfinished"）
# 设置为与源文本相同，因为英文翻译通常就是源文本本身。

import re
import sys
from pathlib import Path


def fix_english_translations(ts_file_path, force=False):
    """修复英文翻译文件"""
    with open(ts_file_path, "r", encoding="utf-8") as f:
        content = f.read()

    original_content = content

    message_pattern = re.compile(r"<message>(.*?)</message>", re.DOTALL)
    source_pattern = re.compile(r"<source>(.*?)</source>", re.DOTALL)
    translation_pattern = re.compile(
        r"<translation(?P<attrs>[^>]*)>(?P<text>.*?)</translation>",
        re.DOTALL,
    )

    def replace_func(match):
        body = match.group(1)
        source_match = source_pattern.search(body)
        translation_match = translation_pattern.search(body)
        if source_match is None or translation_match is None:
            return match.group(0)

        attrs = translation_match.group("attrs")
        if not force and 'type="unfinished"' not in attrs:
            return match.group(0)
        if 'type="vanished"' in attrs:
            return match.group(0)

        source_text = source_match.group(1)

        replacement = f"<translation>{source_text}</translation>"
        new_body = (
            body[: translation_match.start()]
            + replacement
            + body[translation_match.end() :]
        )
        return f"<message>{new_body}</message>"

    content = message_pattern.sub(replace_func, content)

    if content != original_content:
        with open(ts_file_path, "w", encoding="utf-8") as f:
            f.write(content)
        print(f"Updated: {ts_file_path}")
        return True

    return False


def main():
    # 检查是否传入了文件路径参数
    if len(sys.argv) < 2:
        print("Error: Translation file path argument is required.")
        print("\nUsage:")
        print(f"  python {Path(__file__).name} <ts_file_path>")
        print("\nExamples:")
        print(f"  python {Path(__file__).name} RavoStudio_en_US.ts")
        print(f"  python {Path(__file__).name} Ravo/desktop/i18n/RavoStudio_en_US.ts --force")
        print("\n说明：必须传入翻译文件路径（.ts 文件）")
        return 1

    # 获取传入的文件路径
    ts_file_path = Path(sys.argv[1]).resolve()

    # 显示传入的文件路径
    print(f"Target file: {ts_file_path}")

    # 验证文件是否存在
    if not ts_file_path.exists():
        print(f"Error: Translation file not found: {ts_file_path}")
        return 1

    if not ts_file_path.is_file():
        print(f"Error: Path is not a file: {ts_file_path}")
        return 1

    if ts_file_path.suffix.lower() != ".ts":
        print(f"Error: File is not a .ts translation file: {ts_file_path}")
        return 1

    force = "--force" in sys.argv[2:]
    if fix_english_translations(ts_file_path, force=force):
        print("\n[OK] Fixed English translations!")
        return 0
    else:
        print("No updates needed.")
        return 0


if __name__ == "__main__":
    exit(main())
