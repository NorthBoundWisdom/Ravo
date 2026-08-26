#!/usr/bin/env python3
"""Ravo Studio i18n 翻译工作流入口。

支持：
1. 执行单阶段（--part 1 / --part 2）
2. 一次性全流程（--part all）

一次性流程会：
1) 删除旧的 RavoStudio_zh_CN.ts / RavoStudio_en_US.ts（可关闭）
2) 执行 -p1（更新 .ts 与 zh_translate.ini）
3) 自动（或人工）补齐 zh_translate.ini 中的 <unfinished>
4) 执行 -p2（回写中文 ts + 修复 en ts）
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
from collections import defaultdict
from pathlib import Path
from typing import Callable
from urllib import error as urllib_error
from urllib import parse as urllib_parse
from urllib import request as urllib_request

from _runtime import find_repo_root, run_legacy_script


def _split_ini_line(line: str, known_keys: set[str] | None = None):
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
            matched.sort(key=lambda item: item[0], reverse=True)
            _, key, value = matched[0]
            return key, value

    pos = eq_positions[-1]
    return line[:pos], line[pos + 1 :]


def _parse_ini_order(raw_text: str, known_keys: set[str] | None = None):
    """返回有效条目 key 顺序（用于与 p1 对齐校验）。"""
    keys = []
    for line in raw_text.splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue

        split_result = _split_ini_line(line, known_keys)
        if split_result is None:
            keys.append(("__INVALID__" + line, ""))
            continue

        key, value = split_result
        keys.append((key, value))
    return keys


def _read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _collect_unfinished_in_ini(raw_text: str):
    """返回 ini 中 value 为 <unfinished> 的条目。"""
    entries = []
    for line_no, line in enumerate(raw_text.splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue

        split_result = _split_ini_line(line)
        if split_result is None:
            continue

        key, value = split_result
        if value.strip() == "<unfinished>":
            entries.append((line_no, key, value))
    return entries


def _snapshot_ts_paths(repo_root: Path):
    i18n_dir = repo_root / "Ravo" / "desktop" / "i18n"
    return {
        "zh": i18n_dir / "RavoStudio_zh_CN.ts",
        "en": i18n_dir / "RavoStudio_en_US.ts",
        "ini": i18n_dir / "zh_translate.ini",
    }


def _clean_ts_files(paths: dict[str, Path]) -> None:
    for key in ("zh", "en"):
        path = paths[key]
        if path.exists():
            path.unlink()
            print(f"  ✓ 已删除旧文件: {path.name}")


def _extract_source_from_ini_key(key: str) -> str:
    if "::" in key:
        return key.rsplit("::", 1)[-1].replace("\\n", "\n")
    return key.replace("\\n", "\n")


def _escape_ini_value(text: str) -> str:
    return text.replace("\n", "\\n")


def _call_json_url(url: str, payload: dict | None = None, timeout: int = 20):
    if payload is None:
        req = urllib_request.Request(url, method="GET")
    else:
        req = urllib_request.Request(
            url,
            data=json.dumps(payload).encode("utf-8"),
            method="POST",
            headers={"Content-Type": "application/json", "Accept": "application/json"},
        )
    req.add_header("User-Agent", "RavoStudio-i18n-workflow/0.2")
    with urllib_request.urlopen(req, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def _is_retryable_error(error: Exception) -> bool:
    if isinstance(error, urllib_error.HTTPError):
        return error.code in (408, 429, 500, 502, 503, 504)
    if isinstance(error, urllib_error.URLError):
        return True
    return isinstance(error, TimeoutError)


def _run_with_retry(
    task: Callable[[], list[str]],
    label: str,
    max_retries: int,
    base_delay: float,
    max_delay: float,
) -> list[str]:
    attempts = max_retries + 1
    for attempt in range(attempts):
        try:
            return task()
        except Exception as error:
            if not _is_retryable_error(error) or attempt >= max_retries:
                raise

            sleep_seconds = min(max_delay, base_delay * (2**attempt))
            print(
                f"[WARN] {label} 第 {attempt + 1}/{max_retries} 次重试（当前已有 {attempt + 1} 次失败），"
                f"{sleep_seconds:.1f}s 后重试: {error}"
            )
            time.sleep(sleep_seconds)


def _translate_with_mymemory(texts: list[str], timeout: int):
    results = []
    for text in texts:
        if text == "":
            results.append("")
            continue

        encoded = urllib_parse.quote(text)
        url = (
            "https://api.mymemory.translated.net/get?q="
            f"{encoded}&langpair=en%7Czh-CN"
        )
        payload = _call_json_url(url, timeout=timeout)
        translation = payload.get("responseData", {}).get("translatedText", "")
        if not isinstance(translation, str) or not translation.strip():
            raise RuntimeError(f"未能从 MyMemory 获取翻译：{text[:80]}")

        results.append(translation.strip())
        time.sleep(0.15)

    return results


def _extract_json_from_markdown(text: str) -> str:
    if "```" not in text:
        return text.strip()

    start = text.find("```")
    end = text.rfind("```")
    if start == -1 or end <= start:
        return text.strip()

    fenced = text[start + 3 : end].strip()
    lines = fenced.splitlines()
    if lines and lines[0].strip().lower() == "json":
        lines = lines[1:]
    return "\n".join(lines).strip()


def _translate_with_openai(
    texts: list[str],
    model: str,
    api_key: str,
    base_url: str,
    timeout: int,
):
    if not api_key:
        raise RuntimeError("未设置 OpenAI API key。请设置 OPENAI_API_KEY 或使用 --openai-api-key。")

    tasks = [{"id": index, "text": text} for index, text in enumerate(texts)]
    system_prompt = (
        "你是翻译助手，只进行英文到中文（简体）的翻译。"
        "请严格按 JSON 返回，不要输出解释。"
    )
    user_prompt = (
        "请将以下英文文本逐条翻译为中文（简体），保持占位符与格式。"
        "\n输出格式为 JSON：\n"
        '{"translations":[{"id":0,"text":"..."}, ...]}\n'
        "请保持 id 不变，按相同顺序返回。\n"
        f"{json.dumps(tasks, ensure_ascii=False)}"
    )

    endpoint = base_url.rstrip("/") + "/chat/completions"
    data = {
        "model": model,
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_prompt},
        ],
        "temperature": 0.2,
    }

    req = urllib_request.Request(
        endpoint,
        data=json.dumps(data).encode("utf-8"),
        method="POST",
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
            "Accept": "application/json",
        },
    )
    req.add_header("User-Agent", "RavoStudio-i18n-workflow/0.2")
    with urllib_request.urlopen(req, timeout=timeout) as response:
        data_resp = json.loads(response.read().decode("utf-8"))

    candidates = data_resp.get("choices", [])
    if not candidates:
        raise RuntimeError("OpenAI 响应中未找到翻译结果")

    raw = candidates[0].get("message", {}).get("content", "")
    raw_json = _extract_json_from_markdown(raw)

    try:
        parsed = json.loads(raw_json)
    except Exception as error:
        raise RuntimeError(f"OpenAI 返回格式无法解析: {error}\n{raw}") from error

    items = parsed.get("translations")
    if not isinstance(items, list):
        raise RuntimeError("OpenAI 返回缺少 translations 数组")

    result_map = {}
    for item in items:
        item_id = item.get("id")
        translated = item.get("text")
        if not isinstance(item_id, int) or not isinstance(translated, str):
            continue
        result_map[item_id] = translated.strip()

    results = []
    for index in range(len(texts)):
        if index not in result_map:
            raise RuntimeError(f"OpenAI 未返回 id={index} 的翻译结果")
        if not result_map[index].strip():
            raise RuntimeError(f"OpenAI 返回了空翻译: id={index}")
        results.append(result_map[index])
    return results


def _translate_entries(
    entries: list[tuple[int, str, str]],
    args,
    chunk_index: int,
) -> list[str]:
    if not entries:
        return []

    source_texts = [_extract_source_from_ini_key(entry[1]) for entry in entries]

    if args.translation_provider == "openai":
        key = args.openai_api_key or os.environ.get("OPENAI_API_KEY", "")
        return _run_with_retry(
            lambda: _translate_with_openai(
                source_texts,
                model=args.openai_model,
                api_key=key,
                base_url=args.openai_base_url,
                timeout=args.translation_timeout,
            ),
            f"OpenAI 批次 {chunk_index}",
            args.translation_retries,
            args.translation_retry_base_delay,
            args.translation_retry_max_delay,
        )

    if args.translation_provider == "mymemory":
        return _run_with_retry(
            lambda: _translate_with_mymemory(
                source_texts,
                timeout=args.translation_timeout,
            ),
            f"MyMemory 批次 {chunk_index}",
            args.translation_retries,
            args.translation_retry_base_delay,
            args.translation_retry_max_delay,
        )

    raise RuntimeError(f"不支持的翻译服务: {args.translation_provider}")


def _apply_ini_translations(
    ini_text: str,
    unfinished: list[tuple[int, str, str]],
    translations: list[str],
) -> tuple[str, int]:
    if len(unfinished) != len(translations):
        raise RuntimeError("翻译数量与待翻译条目数量不一致")

    pending = defaultdict(list)
    for (_, key, _), text in zip(unfinished, translations):
        pending[key].append(text)

    updated_lines = []
    filled_count = 0

    for line in ini_text.splitlines(keepends=True):
        raw = line.rstrip("\r\n")
        newline = line[len(raw) :]

        split_result = _split_ini_line(raw)
        if split_result is None:
            updated_lines.append(line)
            continue

        key, value = split_result
        if value.strip() == "<unfinished>" and pending.get(key):
            translated = pending[key].pop(0)
            updated_lines.append(f"{key}={_escape_ini_value(translated)}" + newline)
            filled_count += 1
        else:
            updated_lines.append(line)

    remaining = sum(len(values) for values in pending.values())
    if remaining:
        raise RuntimeError(f"有 {remaining} 条翻译未写回到 ini（键值匹配失败）")

    return "".join(updated_lines), filled_count


def _check_and_collect_unfinished(ts_path: Path):
    if not ts_path.exists():
        return []
    content = _read_text(ts_path)
    message_pattern = re.compile(r"<message>(.*?)</message>", re.DOTALL)
    source_pattern = re.compile(r"<source>(.*?)</source>", re.DOTALL)
    translation_pattern = re.compile(
        r"<translation(?P<attrs>[^>]*)>.*?</translation>",
        re.DOTALL,
    )
    entries = []
    for message_match in message_pattern.finditer(content):
        body = message_match.group(1)
        source_match = source_pattern.search(body)
        translation_match = translation_pattern.search(body)
        if source_match is None or translation_match is None:
            continue
        if 'type="unfinished"' not in translation_match.group("attrs"):
            continue
        text = source_match.group(1).strip()
        if len(text) > 90:
            text = text[:87] + "..."
        entries.append(text)
    return entries


def _chunked(items: list, size: int):
    for start in range(0, len(items), size):
        yield items[start : start + size]


def _auto_fill_ini(ts_path: Path, args) -> tuple[str, int]:
    ini_text = _read_text(ts_path)
    unfinished = _collect_unfinished_in_ini(ini_text)
    if not unfinished:
        return ini_text, 0

    if args.translation_batch_size <= 0:
        raise RuntimeError("--translation-batch-size 必须为正整数")
    if args.translation_retries < 0:
        raise RuntimeError("--translation-retries 必须 >= 0")
    if args.translation_retry_base_delay < 0:
        raise RuntimeError("--translation-retry-base-delay 必须为非负数")
    if args.translation_retry_max_delay <= 0:
        raise RuntimeError("--translation-retry-max-delay 必须为正数")

    all_translations: list[str] = []
    for chunk_index, chunk in enumerate(
        _chunked(unfinished, args.translation_batch_size), start=1
    ):
        all_translations.extend(_translate_entries(chunk, args, chunk_index))

    updated_ini, filled_count = _apply_ini_translations(
        ini_text, unfinished, all_translations
    )
    return updated_ini, filled_count


def parse_args():
    parser = argparse.ArgumentParser(description="Ravo Studio i18n 翻译工作流（中英第一阶段）")
    parser.add_argument(
        "--part",
        choices=["1", "2", "all"],
        required=True,
        help="执行第1阶段(-p1)、第2阶段(-p2)，或一次性执行全部流程(all)",
    )
    parser.add_argument(
        "--repo-root",
        default=None,
        help="可选：仓库根路径，默认自动向上查找",
    )
    parser.add_argument(
        "--clean-ts",
        action="store_true",
        default=False,
        help="一次性流程和p1前删除旧的 RavoStudio_zh_CN.ts/RavoStudio_en_US.ts（默认关闭，可开启）",
    )
    parser.add_argument(
        "--skip-order-check",
        action="store_true",
        help="一次性流程中不校验 zh_translate.ini 行顺序是否保持不变",
    )
    parser.add_argument(
        "--non-interactive",
        action="store_true",
        help="一次性流程不等待人工确认；当存在未翻译条目会直接失败",
    )
    parser.add_argument(
        "--auto-translate",
        action="store_true",
        help="一次性流程自动翻译 ini 中的 <unfinished>。",
    )
    parser.add_argument(
        "--translation-provider",
        default="mymemory",
        choices=["mymemory", "openai"],
        help="自动翻译服务（--part all --auto-translate 时生效）",
    )
    parser.add_argument(
        "--openai-api-key",
        default="",
        help="OpenAI API key（如用 openai provider）",
    )
    parser.add_argument(
        "--openai-model",
        default="gpt-4o-mini",
        help="OpenAI 模型（默认: gpt-4o-mini）",
    )
    parser.add_argument(
        "--openai-base-url",
        default="https://api.openai.com/v1",
        help="OpenAI Chat Completions 入口（默认: https://api.openai.com/v1）",
    )
    parser.add_argument(
        "--translation-timeout",
        type=int,
        default=20,
        help="翻译 API 超时（秒）",
    )
    parser.add_argument(
        "--translation-batch-size",
        type=int,
        default=30,
        help="批量翻译时每批条目数（默认: 30）",
    )
    parser.add_argument(
        "--translation-retries",
        type=int,
        default=3,
        help="翻译失败重试次数（默认: 3）",
    )
    parser.add_argument(
        "--translation-retry-base-delay",
        type=float,
        default=1.0,
        help="重试基础等待秒数（默认: 1.0）",
    )
    parser.add_argument(
        "--translation-retry-max-delay",
        type=float,
        default=8.0,
        help="重试最大等待秒数（默认: 8.0）",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repo_root = Path(args.repo_root).resolve() if args.repo_root else None
    args.part = str(args.part)
    if repo_root is None:
        repo_root = find_repo_root()

    ts_paths = _snapshot_ts_paths(repo_root)

    if args.part in ("1", "all"):
        if args.clean_ts or args.part == "all":
            _clean_ts_files(ts_paths)

        result = run_legacy_script("0_update_ts.py", ["-p1"], repo_root=repo_root)
        if result != 0:
            return result

        unfinished = _check_and_collect_unfinished(ts_paths["zh"])
        print(f"未完成中文翻译条目: {len(unfinished)}")
        if unfinished:
            print("示例前10条：")
            for i, item in enumerate(unfinished[:10], 1):
                print(f"  {i}. {item}")

        if args.part == "1":
            return 0

        if not ts_paths["ini"].exists():
            print(f"[ERROR] zh_translate.ini 不存在: {ts_paths['ini']}")
            return 1

        ini_before = _read_text(ts_paths["ini"])
        baseline = _parse_ini_order(ini_before)
        baseline_keys = {item[0] for item in baseline}
        latest_ini_text = ini_before

        if args.auto_translate:
            print("启动自动翻译：基于 ini 中的 <unfinished> 条目自动填充（保持行顺序）。")
            latest_ini_text, filled_count = _auto_fill_ini(ts_paths["ini"], args)
            if filled_count == 0:
                print("[WARN] 当前无 <unfinished> 条目。")
            else:
                ts_paths["ini"].write_text(latest_ini_text, encoding="utf-8")
                print(f"  ✓ 已自动完成翻译: {filled_count} 条")

        elif args.non_interactive:
            ini_state = _collect_unfinished_in_ini(ini_before)
            if ini_state:
                print("[ERROR] 检测到未翻译条目，非交互模式下请先更新 zh_translate.ini。")
                print(f"  发现 {len(ini_state)} 个 <unfinished>。")
                return 1

        else:
            print("请在编辑器中完成 zh_translate.ini 的翻译（按行替换，不要改行顺序），保存后回车继续。")
            input("完成翻译后按 Enter 继续：")

            while True:
                latest_ini_text = _read_text(ts_paths["ini"])
                after_keys = _parse_ini_order(latest_ini_text, baseline_keys)
                unfinished_ini = _collect_unfinished_in_ini(latest_ini_text)

                if not args.skip_order_check:
                    if len(baseline) != len(after_keys):
                        print("[ERROR] 行顺序/数量校验失败：条目数量与 part1 同步后不一致。")
                        return 1
                    for index, (k_before, _v_before) in enumerate(baseline):
                        if after_keys[index][0] != k_before:
                            print("[ERROR] 行顺序校验失败：检测到 key 顺序被打乱。")
                            return 1

                if not unfinished_ini:
                    break

                print(f"[WARN] 当前仍有 {len(unfinished_ini)} 个未完成条目。")
                input("请继续完成翻译后再次回车：")

        if not args.skip_order_check:
            after_keys = _parse_ini_order(latest_ini_text, baseline_keys)
            if len(baseline) != len(after_keys):
                print("[ERROR] 行顺序/数量校验失败：条目数量与 part1 同步后不一致。")
                return 1
            for index, (k_before, _v_before) in enumerate(baseline):
                if after_keys[index][0] != k_before:
                    print("[ERROR] 行顺序校验失败：检测到 key 顺序被打乱。")
                    return 1

        unfinished_ini = _collect_unfinished_in_ini(latest_ini_text)
        if unfinished_ini:
            print(f"[ERROR] 当前仍有 {len(unfinished_ini)} 个未完成项（应已为 0）。")
            return 1

        if latest_ini_text != ini_before:
            ts_paths["ini"].write_text(latest_ini_text, encoding="utf-8")

    if args.part in ("2", "all"):
        return run_legacy_script("0_update_ts.py", ["-p2"], repo_root=repo_root)

    return 0


if __name__ == "__main__":
    sys.exit(main())
