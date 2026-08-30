#!/usr/bin/env python3
"""Synchronize locale-specific translation memories with Qt TS catalogs."""

from __future__ import annotations

from collections import Counter
from dataclasses import dataclass
from pathlib import Path
import tempfile
import xml.etree.ElementTree as ET


@dataclass(frozen=True)
class Message:
    context: str
    source: str
    translation: str


def _messages(tree: ET.ElementTree) -> list[Message]:
    result: list[Message] = []
    for context in tree.getroot().findall("context"):
        context_name = context.findtext("name", default="")
        for message in context.findall("message"):
            source = message.findtext("source", default="")
            translation_node = message.find("translation")
            translation = "" if translation_node is None else "".join(translation_node.itertext())
            if translation.strip() == "<unfinished>":
                translation = ""
            result.append(Message(context_name, source, translation))
    return result


def _escaped(text: str) -> str:
    return text.replace("\n", "\\n")


def _unescaped(text: str) -> str:
    return text.replace("\\n", "\n")


def _active_keys(messages: list[Message]) -> list[str]:
    counts = Counter(message.source for message in messages)
    return [
        f"{message.context}::{message.source}" if counts[message.source] > 1 else message.source
        for message in messages
    ]


def _split(line: str, known_keys: set[str]) -> tuple[str, str] | None:
    matches = [(len(key), key, line[len(key) + 1 :]) for key in known_keys if line.startswith(f"{key}=")]
    if matches:
        _, key, value = max(matches)
        return key, value
    position = line.rfind("=")
    return None if position < 0 else (line[:position], line[position + 1 :])


def _load_memory(path: Path, known_keys: set[str]) -> tuple[dict[str, str], list[str]]:
    values: dict[str, str] = {}
    order: list[str] = []
    if not path.exists():
        return values, order
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        if not raw_line or raw_line.lstrip().startswith("#"):
            continue
        pair = _split(raw_line, {_escaped(key) for key in known_keys})
        if pair is None:
            continue
        key, value = (_unescaped(pair[0]), _unescaped(pair[1]))
        if key not in values:
            order.append(key)
        values[key] = "" if value.strip() == "<unfinished>" else value
    return values, order


def _write_memory(path: Path, active_keys: list[str], values: dict[str, str], old_order: list[str]) -> None:
    active_set = set(active_keys)
    lines = [f"{_escaped(key)}={_escaped(values.get(key, '')) or '<unfinished>'}" for key in active_keys]
    historical = [key for key in old_order if key not in active_set]
    if historical:
        lines.extend(["", "# Historical translations retained for future reuse."])
        lines.extend(f"{_escaped(key)}={_escaped(values[key])}" for key in historical)
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", dir=path.parent, delete=False) as handle:
        handle.write("\n".join(lines) + "\n")
        temporary = Path(handle.name)
    temporary.replace(path)


def sync_memory(ts_path: Path, memory_path: Path) -> tuple[int, int]:
    messages = _messages(ET.parse(ts_path))
    keys = _active_keys(messages)
    existing, old_order = _load_memory(memory_path, set(keys))
    for key, message in zip(keys, messages):
        if key not in existing:
            existing[key] = message.translation
    _write_memory(memory_path, keys, existing, old_order)
    return sum(bool(existing[key].strip()) for key in keys), len(keys)


def _write_ts(tree: ET.ElementTree, path: Path) -> None:
    ET.indent(tree, space="    ")
    with tempfile.NamedTemporaryFile("wb", dir=path.parent, delete=False) as handle:
        handle.write(b'<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE TS>\n')
        tree.write(handle, encoding="utf-8", xml_declaration=False, short_empty_elements=True)
        temporary = Path(handle.name)
    temporary.replace(path)


def apply_memory(ts_path: Path, memory_path: Path) -> tuple[int, int]:
    tree = ET.parse(ts_path)
    messages = _messages(tree)
    keys = _active_keys(messages)
    values, _ = _load_memory(memory_path, set(keys))
    completed = 0
    index = 0
    for context in tree.getroot().findall("context"):
        for message in context.findall("message"):
            value = values.get(keys[index], "").strip()
            translation = message.find("translation")
            if translation is None:
                translation = ET.SubElement(message, "translation")
            translation.text = value
            if value:
                translation.attrib.pop("type", None)
                completed += 1
            else:
                translation.set("type", "unfinished")
            index += 1
    _write_ts(tree, ts_path)
    return completed, len(keys)


def make_source_identity(ts_path: Path) -> int:
    tree = ET.parse(ts_path)
    count = 0
    for message in tree.getroot().iterfind("./context/message"):
        source = message.findtext("source", default="")
        translation = message.find("translation")
        if translation is None:
            translation = ET.SubElement(message, "translation")
        translation.text = source
        translation.attrib.pop("type", None)
        count += 1
    _write_ts(tree, ts_path)
    return count
