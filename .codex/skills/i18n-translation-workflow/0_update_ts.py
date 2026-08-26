#!/usr/bin/env python3
"""
翻译文件更新工作流脚本

使用示例:
  python 0_update_ts.py -p1  # 运行第一部分：提取和同步
  python 0_update_ts.py -p2  # 运行第二部分：应用翻译

工作流说明:
  -p1: 1_add_qstr_to_qml.py -> 2_update_translations.py -> 3_add_json_strings_to_ts.py -> 4_update_zh_trans_ini.py
  -p2: 5_apply_chinese_translations.py -> 6_fix_english_translations.py
"""

import argparse
import html
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path

# 设置路径
from _runtime import find_repo_root

_repo_root = find_repo_root()
_script_dir = _repo_root / "Ravo" / "desktop" / "i18n"
_gui_dir = _repo_root / "Ravo" / "desktop"
_qml_dir = _gui_dir / "qml"
_config_dir = _gui_dir / "config"
_ini_file = _script_dir / "zh_translate.ini"
_zh_ts = _script_dir / "RavoStudio_zh_CN.ts"
_en_ts = _script_dir / "RavoStudio_en_US.ts"


# lupdate 安装根路径（可按需直接修改）
LUPDATE_SEARCH_ROOTS_MAC = [
    str(Path.home() / "Qt"),
    "/opt/Qt",
    "/Applications/Qt",
]

LUPDATE_SEARCH_ROOTS_WIN = [
    "C:/Qt",
    "D:/Qt",
    "E:/Qt",
]

LUPDATE_SEARCH_ROOTS_LINUX = [
    "/opt/Qt",
    "/usr/lib/qt6",
    "/usr/lib64/qt6",
    "/usr/local/Qt",
]


def candidate_lupdate_paths():
    """基于安装根路径扫描 lupdate（不依赖具体 Qt 版本号）"""
    candidates = []
    system = platform.system()

    if system == "Darwin":
        roots = [Path(p) for p in LUPDATE_SEARCH_ROOTS_MAC]
        for root in roots:
            if not root.exists():
                continue
            # 典型安装结构：<QtRoot>/<version>/macos/bin/lupdate
            for p in root.glob("*/macos/bin/lupdate"):
                candidates.append(str(p))
            # 兼容直接安装在根目录下的 bin
            direct = root / "bin" / "lupdate"
            if direct.exists():
                candidates.append(str(direct))

    elif system == "Windows":
        roots = [Path(p) for p in LUPDATE_SEARCH_ROOTS_WIN]
        for root in roots:
            if not root.exists():
                continue
            # 典型安装结构：<QtRoot>/<version>/<kit>/bin/lupdate.exe
            for p in root.glob("*/*/bin/lupdate.exe"):
                candidates.append(str(p))
            # 兼容直接安装在根目录下的 bin
            direct = root / "bin" / "lupdate.exe"
            if direct.exists():
                candidates.append(str(direct))

    elif system == "Linux":
        for root in (Path(p) for p in LUPDATE_SEARCH_ROOTS_LINUX):
            if not root.exists():
                continue
            direct = root / "bin" / "lupdate"
            if direct.exists():
                candidates.append(str(direct))
            for candidate in root.glob("*/bin/lupdate"):
                candidates.append(str(candidate))

    # 去重并排序，保证结果稳定
    return sorted(set(candidates))


def find_lupdate():
    """查找 lupdate：环境变量 -> PATH -> 内置候选路径"""
    env_lupdate = os.environ.get("LUPDATE_EXE")
    if env_lupdate:
        p = Path(env_lupdate)
        if p.exists():
            return str(p)

    which_path = shutil.which("lupdate")
    if which_path:
        return which_path

    for candidate in candidate_lupdate_paths():
        if Path(candidate).exists():
            return candidate

    return None


# ANSI 颜色代码
class Colors:
    """ANSI 颜色代码"""

    RESET = "\033[0m"
    BOLD = "\033[1m"
    DIM = "\033[2m"

    # 前景色
    BLACK = "\033[30m"
    RED = "\033[31m"
    GREEN = "\033[32m"
    YELLOW = "\033[33m"
    BLUE = "\033[34m"
    MAGENTA = "\033[35m"
    CYAN = "\033[36m"
    WHITE = "\033[37m"

    # 亮色
    BRIGHT_BLACK = "\033[90m"
    BRIGHT_RED = "\033[91m"
    BRIGHT_GREEN = "\033[92m"
    BRIGHT_YELLOW = "\033[93m"
    BRIGHT_BLUE = "\033[94m"
    BRIGHT_MAGENTA = "\033[95m"
    BRIGHT_CYAN = "\033[96m"
    BRIGHT_WHITE = "\033[97m"


def enable_color_support():
    """在 Windows 上启用 ANSI 颜色支持"""
    if platform.system() == "Windows":
        # Windows 10 及以上版本支持 ANSI 转义码
        for stream_name in ("stdout", "stderr"):
            stream = getattr(sys, stream_name, None)
            if stream and hasattr(stream, "reconfigure"):
                try:
                    stream.reconfigure(encoding="utf-8")  # type: ignore[attr-defined]
                except Exception:
                    pass


def sanitize_console_text(text):
    """将部分在 Windows GBK 控制台中不稳定的符号替换为 ASCII。"""
    return (
        text.replace("✓", "[OK]")
        .replace("✗", "[FAIL]")
        .replace("⚠", "[WARN]")
    )


def colorize(text, color_code):
    """为文本添加颜色"""
    text = sanitize_console_text(text)
    if not sys.stdout.isatty():
        # 如果不是终端，不添加颜色
        return text
    return f"{color_code}{text}{Colors.RESET}"


def print_success(message):
    """打印成功消息（绿色）"""
    print(colorize(message, Colors.BRIGHT_GREEN))


def print_error(message):
    """打印错误消息（红色）"""
    print(colorize(message, Colors.BRIGHT_RED), file=sys.stderr)


def print_warning(message):
    """打印警告消息（黄色）"""
    print(colorize(message, Colors.BRIGHT_YELLOW))


def print_info(message):
    """打印信息消息（蓝色）"""
    print(colorize(message, Colors.BRIGHT_BLUE))


def print_title(message):
    """打印标题（粗体蓝色）"""
    print(colorize(message, Colors.BOLD + Colors.BRIGHT_BLUE))


def print_separator():
    """打印分隔线（蓝色）"""
    print(colorize("=" * 42, Colors.BRIGHT_BLUE))


def get_script_dir():
    """获取脚本所在目录的绝对路径"""
    return Path(__file__).parent.resolve()


def is_windows():
    """检测是否为 Windows 系统"""
    return platform.system() == "Windows"


def run_python_script(script_name, *args):
    """
    运行 Python 脚本

    Args:
        script_name: 脚本文件名
        *args: Arguments forwarded to the script

    Returns:
        True 如果成功，False 如果失败
    """
    script_dir = get_script_dir()
    script_path = script_dir / script_name

    if not script_path.exists():
        print_error(f"  ✗ 错误：找不到脚本 {script_path}")
        return False

    cmd = [sys.executable, str(script_path)] + [str(arg) for arg in args]
    cmd_str = " ".join(f'"{arg}"' if " " in str(arg) else str(arg) for arg in cmd)
    print_info(f"  运行: {cmd_str}")

    try:
        env = os.environ.copy()
        env["PYTHONUTF8"] = "1"
        result = subprocess.run(cmd, check=True, cwd=str(script_dir), env=env)
        return result.returncode == 0
    except subprocess.CalledProcessError as e:
        print_error(f"  ✗ 脚本执行失败，返回码: {e.returncode}")
        return False
    except Exception as e:
        print_error(f"  ✗ 运行脚本时出错: {e}")
        return False


def extract_known_ini_keys(ts_file_path):
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


def report_double_equal():
    """报告 zh_translate.ini 文件中无法按新规则正确解析的行

    Returns:
        bool: True 如果发现问题，False 如果没有问题
    """
    if not _ini_file.exists():
        print_warning(f"  ⚠ 警告：未找到文件 {_ini_file}")
        return False

    known_keys = extract_known_ini_keys(_zh_ts) if _zh_ts.exists() else set()
    parse_failures = []
    unknown_keys = []

    try:
        with open(_ini_file, "r", encoding="utf-8") as f:
            for line_num, line in enumerate(f, 1):
                stripped = line.strip()
                if not stripped or stripped.startswith("#"):
                    continue
                raw = line.rstrip("\n\r")
                split_result = split_ini_line(raw, known_keys)
                if split_result is None:
                    parse_failures.append((line_num, raw))
                    continue
                key, _ = split_result
                if known_keys and key not in known_keys:
                    unknown_keys.append((line_num, raw))
    except Exception as e:
        print_error(f"  ✗ 读取文件时出错: {e}")
        return False

    if parse_failures:
        print_warning(f"  ⚠ 发现 {len(parse_failures)} 行无法解析（缺少 '=' 分隔符）：")
        for line_num, line_content in parse_failures:
            print(f"    第 {line_num} 行: {line_content}")
    else:
        print_success("  ✓ 未发现无法解析的行")

    if unknown_keys:
        print_warning(
            f"  ⚠ 发现 {len(unknown_keys)} 行 key 不在当前 TS 中（可能是历史残留或手工新增）："
        )
        for line_num, line_content in unknown_keys[:20]:
            print(f"    第 {line_num} 行: {line_content}")
        if len(unknown_keys) > 20:
            print(f"    ... 还有 {len(unknown_keys) - 20} 行未显示")

    if parse_failures:
        return True

    return False


def check_double_equal():
    """检查 zh_translate.ini 文件中是否存在无法解析的行，如果有则退出"""
    print_info("检查 zh_translate.ini 文件中的行是否可正确解析...")

    if not _ini_file.exists():
        print_error(f"  ✗ 错误：未找到文件 {_ini_file}")
        print_error("  请先运行 -p1 生成翻译文件")
        sys.exit(1)

    known_keys = extract_known_ini_keys(_zh_ts) if _zh_ts.exists() else set()
    parse_failures = []
    unknown_keys = []

    try:
        with open(_ini_file, "r", encoding="utf-8") as f:
            for line_num, line in enumerate(f, 1):
                stripped = line.strip()
                if not stripped or stripped.startswith("#"):
                    continue
                raw = line.rstrip("\n\r")
                split_result = split_ini_line(raw, known_keys)
                if split_result is None:
                    parse_failures.append((line_num, raw))
                    continue
                key, _ = split_result
                if known_keys and key not in known_keys:
                    unknown_keys.append((line_num, raw))
    except Exception as e:
        print_error(f"  ✗ 读取文件时出错: {e}")
        sys.exit(1)

    if parse_failures:
        print_error("")
        print_error("  ✗ 发现无法解析的行（缺少 '=' 分隔符），请先修复：")
        for line_num, line_content in parse_failures:
            print_error(f"    第 {line_num} 行: {line_content}")
        print_error("")
        print_error("  请修复上述问题后重新运行 -p2")
        sys.exit(1)

    print_success("  ✓ 未发现无法解析的行")
    if unknown_keys:
        print_warning(
            f"  ⚠ 发现 {len(unknown_keys)} 行 key 不在当前 TS 中（将被忽略，不影响 -p2）"
        )
    print("")


def check_unfinished_translations():
    """检查两个 .ts 文件中是否存在 unfinished 字段并报告结果"""
    print_separator()
    print_title("检查翻译文件中的未完成项")
    print_separator()
    print("")

    def collect_unfinished_entries(content):
        entries = []
        message_pattern = re.compile(r"<message>(.*?)</message>", re.DOTALL)
        source_pattern = re.compile(r"<source>(.*?)</source>", re.DOTALL)
        translation_pattern = re.compile(
            r"<translation(?P<attrs>[^>]*)>.*?</translation>",
            re.DOTALL,
        )
        for message_match in message_pattern.finditer(content):
            body = message_match.group(1)
            source_match = source_pattern.search(body)
            translation_match = translation_pattern.search(body)
            if source_match is None or translation_match is None:
                continue
            attrs = translation_match.group("attrs")
            if 'type="unfinished"' not in attrs:
                continue
            decoded_source = html.unescape(source_match.group(1).strip())
            if len(decoded_source) > 60:
                decoded_source = decoded_source[:60] + "..."
            entries.append(decoded_source)
        return entries

    def check_one_ts(ts_path):
        if not ts_path.exists():
            print_warning(f"  ⚠ 文件不存在: {ts_path.name}")
            return {"count": -1, "entries": []}

        print_info(f"检查文件: {ts_path.name}")
        try:
            with open(ts_path, "r", encoding="utf-8") as f:
                content = f.read()

            unfinished_entries = collect_unfinished_entries(content)
            unfinished_count = len(unfinished_entries)

            if unfinished_count > 0:
                print_warning(f"  ⚠ 发现 {unfinished_count} 个未完成的翻译")
                if unfinished_entries:
                    print("  示例条目（前10个）：")
                    for i, entry in enumerate(unfinished_entries[:10], 1):
                        print(f"    {i}. {entry}")
                    if unfinished_count > 10:
                        print(f"    ... 还有 {unfinished_count - 10} 个未显示")
            else:
                print_success("  ✓ 未发现未完成的翻译")
            return {
                "count": unfinished_count,
                "entries": unfinished_entries[:10],
            }
        except Exception as e:
            print_error(f"  ✗ 读取文件时出错: {e}")
            return {"count": -1, "entries": []}

    results = {}

    # 检查中文翻译文件
    results[_zh_ts.name] = check_one_ts(_zh_ts)

    print("")

    # 检查英文翻译文件
    results[_en_ts.name] = check_one_ts(_en_ts)

    print("")

    # 汇总报告
    print_separator()
    print_title("检查结果汇总")
    print_separator()
    print("")

    total_unfinished = 0
    for filename, data in results.items():
        if data["count"] >= 0:
            total_unfinished += data["count"]
            if data["count"] > 0:
                print_warning(f"  {filename}: {data['count']} 个未完成的翻译")
            else:
                print_success(f"  {filename}: 无未完成的翻译")
        else:
            print_warning(f"  {filename}: 无法检查")

    print("")
    if total_unfinished > 0:
        print_warning(f"总计: {total_unfinished} 个未完成的翻译")
    else:
        print_success("总计: 所有翻译均已完成")
    print("")


def show_help():
    """显示帮助信息"""
    script_name = Path(__file__).name
    print("翻译文件更新工作流脚本")
    print("")
    print("用法:")
    print(f"  python {script_name} -p1    # 运行第一部分：提取和同步")
    print(f"  python {script_name} -p2    # 运行第二部分：应用翻译")
    print("")
    print("工作流说明:")
    print("  -p1 (第一部分):")
    print("    1. 将 QML 文件中的硬编码字符串替换为 qsTr() 调用")
    print("    2. 运行 lupdate 提取所有 qsTr() 字符串到 .ts 文件")
    print("    3. 从 JSON 配置文件中提取字符串并添加到 .ts 文件")
    print("    4. 同步 zh_translate.ini 与 RavoStudio_zh_CN.ts 文件")
    print("    5. 检查 zh_translate.ini 文件中是否存在无法解析的行")
    print("")
    print("  -p2 (第二部分):")
    print("    5. 将 zh_translate.ini 中的翻译更新到 RavoStudio_zh_CN.ts 文件")
    print("    6. 修复英文翻译文件（将未完成的翻译设置为与源文本相同）")
    print("")
    print("注意：")
    print("  - 运行 -p1 后，请手动编辑 zh_translate.ini 完成翻译")
    print("  - 然后运行 -p2 应用翻译")


def run_part1():
    """运行第一部分：提取和同步"""
    print_separator()
    print_title("运行第一部分：提取和同步")
    print_separator()
    print("")

    # 步骤 1: 将 QML 文件中的硬编码字符串替换为 qsTr() 调用
    print_info("[1/5] 将 QML 文件中的硬编码字符串替换为 qsTr() 调用...")
    if run_python_script("1_add_qstr_to_qml.py", _qml_dir):
        print_success("  ✓ 步骤 1 完成")
    else:
        print_error("  ✗ 步骤 1 失败")
        sys.exit(1)
    print("")

    # 步骤 2: 运行 lupdate 提取所有 qsTr() 字符串
    print_info("[2/5] 运行 lupdate 提取所有 qsTr() 字符串到 .ts 文件...")
    lupdate_exe = find_lupdate()
    if not lupdate_exe:
        print_error("  ✗ 未找到 lupdate。请设置环境变量 LUPDATE_EXE 或将 lupdate 加入 PATH。")
        print_warning("  已尝试内置候选路径（当前平台）：")
        for p in candidate_lupdate_paths()[:12]:
            print_warning(f"    - {p}")
        sys.exit(1)
    if run_python_script("2_update_translations.py", _gui_dir, lupdate_exe):
        print_success("  ✓ 步骤 2 完成")
    else:
        print_error("  ✗ 步骤 2 失败")
        sys.exit(1)
    print("")

    # 步骤 3: 从可选 JSON 配置文件中提取字符串
    if _config_dir.exists() and _config_dir.is_dir():
        print_info("[3/5] 从 JSON 配置文件中提取字符串并添加到 .ts 文件...")
        if run_python_script("3_add_json_strings_to_ts.py", _config_dir):
            print_success("  ✓ 步骤 3 完成")
        else:
            print_error("  ✗ 步骤 3 失败")
            sys.exit(1)
    else:
        print_warning(f"[3/5] 跳过：未找到 Ravo desktop 配置目录 ({_config_dir})")
    print("")

    # 步骤 4: 同步 zh_translate.ini 与 RavoStudio_zh_CN.ts 文件
    print_info("[4/5] 同步 zh_translate.ini 与 RavoStudio_zh_CN.ts 文件...")
    if not _ini_file.exists():
        _ini_file.touch()
        print_info(f"  已创建初始翻译记忆: {_ini_file}")
    if run_python_script("4_update_zh_trans_ini.py", _ini_file, _zh_ts):
        print_success("  ✓ 步骤 4 完成")
    else:
        print_error("  ✗ 步骤 4 失败")
        sys.exit(1)
    print("")

    # 步骤 5: 报告无法解析的行
    print_info("[5/5] 检查 zh_translate.ini 文件中是否存在无法解析的行...")
    report_double_equal()
    print("")

    print_separator()
    print_title("第一部分完成！")
    print_separator()
    print("")
    print_info("下一步：")
    print(f"  1. 编辑 {_ini_file} 完成翻译")
    script_name = Path(__file__).name
    print_info(f"  2. 运行: python .codex/skills/i18n-translation-workflow/{script_name} -p2")
    print("")


def run_part2():
    """运行第二部分：应用翻译"""
    print_separator()
    print_title("运行第二部分：应用翻译")
    print_separator()
    print("")

    # 在开始前检查是否有无法解析的行
    check_double_equal()

    # 步骤 5: 将 zh_translate.ini 中的翻译更新到 RavoStudio_zh_CN.ts 文件
    print_info("[1/2] 将 zh_translate.ini 中的翻译更新到 RavoStudio_zh_CN.ts 文件...")
    if run_python_script("5_apply_chinese_translations.py", _zh_ts, "--force"):
        print_success("  ✓ 步骤 1 完成")
    else:
        print_error("  ✗ 步骤 1 失败")
        sys.exit(1)
    print("")

    # 步骤 6: 修复英文翻译文件
    print_info("[2/2] 修复英文翻译文件（将未完成的翻译设置为与源文本相同）...")
    if run_python_script("6_fix_english_translations.py", _en_ts, "--force"):
        print_success("  ✓ 步骤 2 完成")
    else:
        print_error("  ✗ 步骤 2 失败")
        sys.exit(1)
    print("")

    print_separator()
    print_title("第二部分完成！")
    print_separator()
    print("")
    print_success("翻译文件已更新完成。")
    print("")

    # 检查未完成的翻译
    check_unfinished_translations()


def main():
    """主函数"""
    parser = argparse.ArgumentParser(
        description="翻译文件更新工作流脚本",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
工作流说明:
  -p1 (第一部分):
    1. 将 QML 文件中的硬编码字符串替换为 qsTr() 调用
    2. 运行 lupdate 提取所有 qsTr() 字符串到 .ts 文件
    3. 从 JSON 配置文件中提取字符串并添加到 .ts 文件
    4. 同步 zh_translate.ini 与 RavoStudio_zh_CN.ts 文件
    5. 检查 zh_translate.ini 文件中是否存在无法解析的行

  -p2 (第二部分):
    5. 将 zh_translate.ini 中的翻译更新到 RavoStudio_zh_CN.ts 文件
    6. 修复英文翻译文件（将未完成的翻译设置为与源文本相同）

注意：
  - 运行 -p1 后，请手动编辑 zh_translate.ini 完成翻译
  - 然后运行 -p2 应用翻译
        """,
    )

    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument(
        "-p1",
        "--part1",
        action="store_true",
        help="运行第一部分：提取和同步",
    )
    group.add_argument(
        "-p2",
        "--part2",
        action="store_true",
        help="运行第二部分：应用翻译",
    )

    args = parser.parse_args()

    # 启用颜色支持
    enable_color_support()

    # 切换到脚本所在目录
    script_dir = get_script_dir()
    os.chdir(script_dir)

    if args.part1:
        run_part1()
    elif args.part2:
        run_part2()


if __name__ == "__main__":
    main()
