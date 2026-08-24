#!/usr/bin/env python3
"""Run repeatable darktable-cli CPU/OpenCL export baselines."""

from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import hashlib
import json
import math
import os
import platform
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


MODULE_RE = re.compile(
    r"\[dev_pixelpipe\]\s+took\s+(?P<wall>[0-9.]+)\s+secs\s+"
    r"\((?P<cpu>[0-9.]+)\s+CPU\)\s+\[(?P<pipe>[^]]+)]\s+"
    r"processed\s+`(?P<module>[^']+)'\s+on\s+(?P<backend>CPU|GPU)(?P<details>.*)"
)
PIPELINE_RE = re.compile(
    r"\[dev_process_(?:image|export)]\s+pixel pipeline(?: processing)? took\s+"
    r"(?P<wall>[0-9.]+)\s+secs\s+"
    r"\((?P<cpu>[0-9.]+)\s+CPU\)"
)
SUMMARY_RE = re.compile(r"\[dev_pixelpipe_summary]\s+\[(?P<pipe>[^]]+)]\s+(?P<values>.*)")
OPENCL_QUEUE_RE = re.compile(
    r"\[opencl_profiling]\s+spent\s+(?P<seconds>[0-9.]+)\s+seconds totally in command queue"
)
OPENCL_EVENT_RE = re.compile(
    r"\[opencl_profiling]\s+spent\s+(?P<seconds>[0-9.]+)\s+seconds in (?P<tag>.+)"
)
KEY_VALUE_RE = re.compile(r"(?P<key>[a-z_]+)=(?P<value>[-0-9.]+)")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Export one image on CPU and OpenCL, then write JSON/Markdown reports."
    )
    parser.add_argument("--input", required=True, type=Path, help="Input image")
    parser.add_argument("--output-dir", required=True, type=Path, help="New or empty report directory")
    parser.add_argument("--xmp", type=Path, help="Optional XMP sidecar")
    parser.add_argument("--cli", type=Path, default=Path("build/mac_clang_release/bin/darktable-cli"))
    parser.add_argument("--data-dir", type=Path, help="Installed share/darktable directory")
    parser.add_argument("--module-dir", type=Path, help="Installed lib/darktable directory")
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--runs", type=int, default=3, help="Measured runs per backend")
    parser.add_argument("--warmups", type=int, default=1, help="Unmeasured warm-up runs per backend")
    parser.add_argument("--width", type=int, default=0, help="Maximum export width; 0 keeps full size")
    parser.add_argument("--height", type=int, default=0, help="Maximum export height; 0 keeps full size")
    parser.add_argument("--style", help="Optional darktable style")
    parser.add_argument("--timeout", type=int, default=600, help="Timeout in seconds for each export")
    parser.add_argument("--libtiff", type=Path, help="Optional path to libtiff for pixel comparison")
    parser.add_argument("--rmse-limit", type=float, default=2e-5, help="Pixel RMSE gate")
    parser.add_argument(
        "--max-abs-limit", type=float, default=2e-4, help="Maximum absolute pixel error gate"
    )
    parser.add_argument(
        "--require-opencl",
        action="store_true",
        help="Exit with an error if the OpenCL run processes no modules on a GPU",
    )
    args = parser.parse_args()
    if args.runs < 1 or args.warmups < 0:
        parser.error("--runs must be >= 1 and --warmups must be >= 0")
    if args.rmse_limit < 0 or args.max_abs_limit < 0:
        parser.error("comparison limits must be >= 0")
    return args


def checked_path(path: Path, label: str, executable: bool = False) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.exists():
        raise SystemExit(f"{label} does not exist: {resolved}")
    if executable and not os.access(resolved, os.X_OK):
        raise SystemExit(f"{label} is not executable: {resolved}")
    return resolved


def prepare_output_dir(path: Path) -> Path:
    resolved = path.expanduser().resolve()
    if resolved.exists() and any(resolved.iterdir()):
        raise SystemExit(f"output directory must be empty: {resolved}")
    resolved.mkdir(parents=True, exist_ok=True)
    return resolved


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def resolve_libtiff(explicit: Path | None) -> str:
    if explicit:
        return str(checked_path(explicit, "libtiff"))

    discovered = ctypes.util.find_library("tiff")
    candidates = [
        discovered,
        "/opt/homebrew/opt/libtiff/lib/libtiff.6.dylib",
        "/opt/homebrew/opt/libtiff/lib/libtiff.dylib",
        "/usr/local/opt/libtiff/lib/libtiff.6.dylib",
        "/usr/local/opt/libtiff/lib/libtiff.dylib",
    ]
    for candidate in candidates:
        if not candidate:
            continue
        try:
            library = ctypes.CDLL(candidate)
            del library
            return candidate
        except OSError:
            continue
    raise SystemExit("libtiff was not found; pass its path with --libtiff")


class FloatTiff:
    """Minimal libtiff-backed reader for the runner's 32-bit float RGB outputs."""

    def __init__(self, library_path: str, path: Path) -> None:
        self.path = path
        self.library = ctypes.CDLL(library_path)
        self.library.TIFFOpen.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        self.library.TIFFOpen.restype = ctypes.c_void_p
        self.library.TIFFClose.argtypes = [ctypes.c_void_p]
        self.library.TIFFGetField.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        self.library.TIFFGetField.restype = ctypes.c_int
        self.library.TIFFScanlineSize64.argtypes = [ctypes.c_void_p]
        self.library.TIFFScanlineSize64.restype = ctypes.c_uint64
        self.library.TIFFReadScanline.argtypes = [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_uint16,
        ]
        self.library.TIFFReadScanline.restype = ctypes.c_int
        self.handle = self.library.TIFFOpen(os.fsencode(path), b"r")
        if not self.handle:
            raise RuntimeError(f"cannot open TIFF: {path}")

        self.width = self._field(256, ctypes.c_uint32, "width")
        self.height = self._field(257, ctypes.c_uint32, "height")
        self.bits_per_sample = self._field(258, ctypes.c_uint16, "bits per sample")
        self.samples_per_pixel = self._field(277, ctypes.c_uint16, "samples per pixel")
        self.planar_config = self._field(284, ctypes.c_uint16, "planar configuration")
        self.sample_format = self._field(339, ctypes.c_uint16, "sample format")
        if self.bits_per_sample != 32 or self.sample_format != 3 or self.planar_config != 1:
            raise RuntimeError(
                f"unsupported TIFF layout in {path}: bits={self.bits_per_sample}, "
                f"sample_format={self.sample_format}, planar_config={self.planar_config}"
            )

        self.sample_count = self.width * self.samples_per_pixel
        expected_size = self.sample_count * ctypes.sizeof(ctypes.c_float)
        scanline_size = self.library.TIFFScanlineSize64(self.handle)
        if scanline_size != expected_size:
            raise RuntimeError(
                f"unexpected scanline size in {path}: {scanline_size}, expected {expected_size}"
            )
        self.buffer = (ctypes.c_float * self.sample_count)()

    def _field(self, tag: int, kind: Any, label: str) -> int:
        value = kind()
        if self.library.TIFFGetField(self.handle, tag, ctypes.byref(value)) != 1:
            raise RuntimeError(f"missing TIFF {label} in {self.path}")
        return int(value.value)

    def row(self, index: int) -> Any:
        if self.library.TIFFReadScanline(self.handle, self.buffer, index, 0) != 1:
            raise RuntimeError(f"cannot read row {index} from {self.path}")
        return self.buffer

    def close(self) -> None:
        if self.handle:
            self.library.TIFFClose(self.handle)
            self.handle = None

    def __enter__(self) -> "FloatTiff":
        return self

    def __exit__(self, *_: Any) -> None:
        self.close()


def compare_float_tiffs(library_path: str, cpu_path: Path, gpu_path: Path) -> dict[str, Any]:
    with FloatTiff(library_path, cpu_path) as cpu, FloatTiff(library_path, gpu_path) as gpu:
        cpu_layout = (cpu.width, cpu.height, cpu.samples_per_pixel)
        gpu_layout = (gpu.width, gpu.height, gpu.samples_per_pixel)
        if cpu_layout != gpu_layout:
            raise RuntimeError(f"TIFF layout differs: CPU={cpu_layout}, OpenCL={gpu_layout}")

        sum_squares = 0.0
        max_abs = 0.0
        quantile_differences: list[float] = []
        total_samples = cpu.width * cpu.height * cpu.samples_per_pixel
        quantile_stride = max(1, math.ceil(total_samples / 1_000_000))
        finite_sample_count = 0
        cpu_nonfinite = 0
        gpu_nonfinite = 0
        channel_squares = [0.0] * cpu.samples_per_pixel
        channel_max = [0.0] * cpu.samples_per_pixel
        channel_samples = [0] * cpu.samples_per_pixel

        for row_index in range(cpu.height):
            cpu_row = cpu.row(row_index)
            gpu_row = gpu.row(row_index)
            for sample_index in range(cpu.sample_count):
                cpu_value = float(cpu_row[sample_index])
                gpu_value = float(gpu_row[sample_index])
                cpu_finite = math.isfinite(cpu_value)
                gpu_finite = math.isfinite(gpu_value)
                cpu_nonfinite += not cpu_finite
                gpu_nonfinite += not gpu_finite
                if not (cpu_finite and gpu_finite):
                    continue
                difference = abs(cpu_value - gpu_value)
                channel = sample_index % cpu.samples_per_pixel
                flat_index = row_index * cpu.sample_count + sample_index
                if flat_index % quantile_stride == 0:
                    quantile_differences.append(difference)
                finite_sample_count += 1
                sum_squares += difference * difference
                channel_squares[channel] += difference * difference
                channel_samples[channel] += 1
                max_abs = max(max_abs, difference)
                channel_max[channel] = max(channel_max[channel], difference)

        quantile_differences.sort()
        quantile_samples = len(quantile_differences)
        p99_index = max(0, math.ceil(quantile_samples * 0.99) - 1)
        return {
            "cpu_output": str(cpu_path),
            "opencl_output": str(gpu_path),
            "width": cpu.width,
            "height": cpu.height,
            "samples_per_pixel": cpu.samples_per_pixel,
            "finite_samples_compared": finite_sample_count,
            "quantile_samples": quantile_samples,
            "quantile_stride": quantile_stride,
            "cpu_nonfinite_samples": cpu_nonfinite,
            "opencl_nonfinite_samples": gpu_nonfinite,
            "rmse": math.sqrt(sum_squares / finite_sample_count)
            if finite_sample_count
            else None,
            "max_abs": max_abs if finite_sample_count else None,
            "p99_abs": quantile_differences[p99_index] if quantile_samples else None,
            "channels": [
                {
                    "index": index,
                    "rmse": math.sqrt(channel_squares[index] / channel_samples[index])
                    if channel_samples[index]
                    else None,
                    "max_abs": channel_max[index] if channel_samples[index] else None,
                }
                for index in range(cpu.samples_per_pixel)
            ],
        }


def git_metadata(repo: Path) -> dict[str, Any]:
    def git(*arguments: str) -> str:
        result = subprocess.run(
            ["git", *arguments], cwd=repo, text=True, capture_output=True, check=False
        )
        return result.stdout.strip() if result.returncode == 0 else "unknown"

    status = git("status", "--porcelain")
    return {
        "commit": git("rev-parse", "HEAD"),
        "describe": git("describe", "--always", "--dirty", "--tags"),
        "dirty": bool(status and status != "unknown"),
    }


def parse_log(log: str) -> dict[str, Any]:
    modules: list[dict[str, Any]] = []
    for match in MODULE_RE.finditer(log):
        modules.append(
            {
                "module": match.group("module"),
                "pipe": match.group("pipe"),
                "backend": match.group("backend"),
                "wall_seconds": float(match.group("wall")),
                "cpu_seconds": float(match.group("cpu")),
                "details": match.group("details").strip(),
            }
        )

    pipelines = [
        {"wall_seconds": float(match.group("wall")), "cpu_seconds": float(match.group("cpu"))}
        for match in PIPELINE_RE.finditer(log)
    ]

    summaries: list[dict[str, Any]] = []
    integer_keys = {
        "attempts",
        "cpu_modules",
        "gpu_modules",
        "gpu_segments",
        "backend_switches",
        "gpu_segment_endpoints",
    }
    for match in SUMMARY_RE.finditer(log):
        values: dict[str, Any] = {"pipe": match.group("pipe")}
        for item in KEY_VALUE_RE.finditer(match.group("values")):
            key, raw = item.group("key"), item.group("value")
            values[key] = int(raw) if key in integer_keys else float(raw)
        summaries.append(values)

    return {
        "modules": modules,
        "pipelines": pipelines,
        "pipeline_summary": summaries,
        "opencl_events": [
            {"tag": match.group("tag").strip(), "seconds": float(match.group("seconds"))}
            for match in OPENCL_EVENT_RE.finditer(log)
        ],
        "opencl_queue_seconds": [
            float(match.group("seconds")) for match in OPENCL_QUEUE_RE.finditer(log)
        ],
    }


def build_command(
    args: argparse.Namespace,
    mode: str,
    output: Path,
    config_dir: Path,
    cache_dir: Path,
) -> list[str]:
    command = [str(args.cli), str(args.input)]
    if args.xmp:
        command.append(str(args.xmp))
    command.extend(
        [
            str(output),
            "--apply-custom-presets",
            "false",
            "--out-ext",
            "tif",
            "--hq",
            "true",
            "--width",
            str(args.width),
            "--height",
            str(args.height),
        ]
    )
    if args.style:
        command.extend(["--style", args.style])
    command.extend(
        [
            "--core",
            "--configdir",
            str(config_dir),
            "--cachedir",
            str(cache_dir),
            "--conf",
            "opencl=true",
            "--conf",
            "plugins/imageio/format/tiff/bpp=32",
            "-d",
            "perf",
            "-d",
            "opencl",
        ]
    )
    if args.data_dir:
        command.extend(["--datadir", str(args.data_dir)])
    if args.module_dir:
        command.extend(["--moduledir", str(args.module_dir)])
    if mode == "cpu":
        command.append("--disable-opencl")
    return command


def run_backend(args: argparse.Namespace, mode: str, root: Path) -> dict[str, Any]:
    mode_dir = root / mode
    config_dir = mode_dir / "config"
    cache_dir = mode_dir / "cache"
    mode_dir.mkdir()
    config_dir.mkdir()
    cache_dir.mkdir()

    measured: list[dict[str, Any]] = []
    all_runs = args.warmups + args.runs
    for index in range(all_runs):
        warmup = index < args.warmups
        ordinal = index + 1 if warmup else index - args.warmups + 1
        kind = "warmup" if warmup else "run"
        output = mode_dir / f"{kind}-{ordinal}.tif"
        log_path = mode_dir / f"{kind}-{ordinal}.log"
        command = build_command(args, mode, output, config_dir, cache_dir)
        started = time.monotonic()
        try:
            result = subprocess.run(
                command,
                cwd=args.repo,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=args.timeout,
                check=False,
            )
        except subprocess.TimeoutExpired as error:
            partial = error.stdout or ""
            if isinstance(partial, bytes):
                partial = partial.decode(errors="replace")
            log_path.write_text(partial, encoding="utf-8")
            raise SystemExit(f"{mode} {kind} {ordinal} timed out; see {log_path}") from error

        process_seconds = time.monotonic() - started
        log_path.write_text(result.stdout, encoding="utf-8")
        if result.returncode != 0 or not output.is_file():
            raise SystemExit(
                f"{mode} {kind} {ordinal} failed with exit code {result.returncode}; "
                f"see {log_path}"
            )

        record = {
            "kind": kind,
            "ordinal": ordinal,
            "command": command,
            "process_seconds": process_seconds,
            "output": str(output.relative_to(root)),
            "output_bytes": output.stat().st_size,
            "output_sha256": sha256(output),
            "log": str(log_path.relative_to(root)),
            **parse_log(result.stdout),
        }
        if warmup:
            output.unlink()
        else:
            measured.append(record)
        print(f"{mode}: {kind} {ordinal}/{args.warmups if warmup else args.runs} complete")

    process_times = [record["process_seconds"] for record in measured]
    pipeline_times = [
        pipeline["wall_seconds"]
        for record in measured
        for pipeline in record["pipelines"][-1:]
    ]
    gpu_modules = sum(
        1
        for record in measured
        for module in record["modules"]
        if module["backend"] == "GPU"
    )
    return {
        "runs": measured,
        "process_timing": summarize_times(process_times),
        "pipeline_timing": summarize_times(pipeline_times),
        "gpu_module_observations": gpu_modules,
    }


def summarize_times(values: list[float]) -> dict[str, float | int | None]:
    if not values:
        return {"samples": 0, "median": None, "p90": None, "mad": None, "min": None, "max": None}

    ordered = sorted(values)
    median = statistics.median(ordered)
    position = (len(ordered) - 1) * 0.9
    lower = math.floor(position)
    upper = math.ceil(position)
    p90 = ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)
    return {
        "samples": len(ordered),
        "median": median,
        "p90": p90,
        "mad": statistics.median(abs(value - median) for value in ordered),
        "min": ordered[0],
        "max": ordered[-1],
    }


def slow_modules(backend: dict[str, Any], limit: int = 10) -> list[dict[str, Any]]:
    timings: dict[tuple[str, str], list[float]] = {}
    for run in backend["runs"]:
        for module in run["modules"]:
            key = (module["module"], module["backend"])
            timings.setdefault(key, []).append(module["wall_seconds"])
    rows = [
        {
            "module": key[0],
            "backend": key[1],
            "median_wall_seconds": statistics.median(values),
            "observations": len(values),
        }
        for key, values in timings.items()
    ]
    return sorted(rows, key=lambda row: row["median_wall_seconds"], reverse=True)[:limit]


def seconds(value: Any) -> str:
    return "n/a" if value is None else f"{value:.4f}"


def metric(value: Any) -> str:
    return "n/a" if value is None else f"{value:.3e}"


def markdown_report(report: dict[str, Any]) -> str:
    lines = [
        "# CPU / OpenCL 基线报告",
        "",
        f"- Git: `{report['git']['describe']}`",
        f"- 输入: `{report['input']['path']}`",
        f"- 输入 SHA-256: `{report['input']['sha256']}`",
        f"- 测量: 每种后端预热 {report['settings']['warmups']} 次，正式运行 {report['settings']['runs']} 次",
        "",
        "| 模式 | 进程中位数 (s) | 进程 P90 | 进程 MAD | pixelpipe 中位数 (s) | pixelpipe P90 | pixelpipe MAD | GPU 模块观测数 |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for mode in ("cpu", "opencl"):
        backend = report["backends"][mode]
        process = backend["process_timing"]
        pipeline = backend["pipeline_timing"]
        lines.append(
            f"| {mode} | {seconds(process['median'])} | {seconds(process['p90'])} | "
            f"{seconds(process['mad'])} | {seconds(pipeline['median'])} | "
            f"{seconds(pipeline['p90'])} | {seconds(pipeline['mad'])} | "
            f"{backend['gpu_module_observations']} |"
        )

    comparison = report["comparison"]
    lines.extend(
        [
            "",
            "## CPU / OpenCL 像素比较",
            "",
            "| RMSE | 最大绝对误差 | P99 绝对误差 | CPU 非有限值 | OpenCL 非有限值 | 结果 |",
            "| ---: | ---: | ---: | ---: | ---: | --- |",
            f"| {metric(comparison['rmse'])} | {metric(comparison['max_abs'])} | "
            f"{metric(comparison['p99_abs'])} | {comparison['cpu_nonfinite_samples']} | "
            f"{comparison['opencl_nonfinite_samples']} | "
            f"{'通过' if comparison['passed'] else '未通过'} |",
        ]
    )

    lines.extend(["", "## 最慢模块", ""])
    for mode in ("cpu", "opencl"):
        lines.extend(
            [
                f"### {mode}",
                "",
                "| 模块 | 最终后端 | 主机侧耗时中位数 (s) | 样本数 |",
                "| --- | --- | ---: | ---: |",
            ]
        )
        rows = report["backends"][mode]["slow_modules"]
        if rows:
            lines.extend(
                f"| {row['module']} | {row['backend']} | "
                f"{row['median_wall_seconds']:.4f} | {row['observations']} |"
                for row in rows
            )
        else:
            lines.append("| _未解析到模块日志_ | - | - | - |")
        lines.append("")

    if report["warnings"]:
        lines.extend(["## 警告", "", *[f"- {warning}" for warning in report["warnings"]], ""])
    lines.extend(
        [
            "逐次命令、输出哈希、模块路由、GPU 段与 OpenCL 队列数据见 `report.json`；",
            "`.log` 和 `.tif` 保留在对应后端目录中。输出哈希标识具体文件，画质判断以解码后的浮点像素指标为准。",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    args = parse_args()
    args.repo = checked_path(args.repo, "repository")
    args.input = checked_path(args.input, "input")
    args.cli = checked_path(args.cli, "darktable-cli", executable=True)
    if args.xmp:
        args.xmp = checked_path(args.xmp, "XMP")
    if args.data_dir:
        args.data_dir = checked_path(args.data_dir, "data directory")
    if args.module_dir:
        args.module_dir = checked_path(args.module_dir, "module directory")
    output_dir = prepare_output_dir(args.output_dir)

    report: dict[str, Any] = {
        "schema_version": 1,
        "created_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "host": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "python": platform.python_version(),
        },
        "git": git_metadata(args.repo),
        "input": {
            "path": str(args.input),
            "sha256": sha256(args.input),
            "xmp": str(args.xmp) if args.xmp else None,
            "xmp_sha256": sha256(args.xmp) if args.xmp else None,
        },
        "binary": {"path": str(args.cli), "sha256": sha256(args.cli)},
        "settings": {
            "runs": args.runs,
            "warmups": args.warmups,
            "width": args.width,
            "height": args.height,
            "style": args.style,
            "data_dir": str(args.data_dir) if args.data_dir else None,
            "module_dir": str(args.module_dir) if args.module_dir else None,
            "rmse_limit": args.rmse_limit,
            "max_abs_limit": args.max_abs_limit,
        },
        "backends": {},
        "warnings": [],
    }

    for mode in ("cpu", "opencl"):
        report["backends"][mode] = run_backend(args, mode, output_dir)
        report["backends"][mode]["slow_modules"] = slow_modules(report["backends"][mode])

    opencl_missing = report["backends"]["opencl"]["gpu_module_observations"] == 0
    if opencl_missing:
        report["warnings"].append(
            "OpenCL 模式未观测到 GPU 模块；这通常表示设备不可用、初始化失败或工作流未命中 GPU 实现。"
        )

    cpu_output = output_dir / report["backends"]["cpu"]["runs"][0]["output"]
    opencl_output = output_dir / report["backends"]["opencl"]["runs"][0]["output"]
    report["comparison"] = compare_float_tiffs(
        resolve_libtiff(args.libtiff), cpu_output, opencl_output
    )
    comparison = report["comparison"]
    comparison["rmse_limit"] = args.rmse_limit
    comparison["max_abs_limit"] = args.max_abs_limit
    comparison["passed"] = (
        comparison["cpu_nonfinite_samples"] == 0
        and comparison["opencl_nonfinite_samples"] == 0
        and comparison["rmse"] is not None
        and comparison["rmse"] <= args.rmse_limit
        and comparison["max_abs"] <= args.max_abs_limit
    )
    if not comparison["passed"]:
        report["warnings"].append(
            "CPU/OpenCL 浮点 TIFF 像素比较未通过设定的有限值、RMSE 或最大绝对误差门槛。"
        )

    (output_dir / "report.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    (output_dir / "summary.md").write_text(markdown_report(report), encoding="utf-8")
    print(f"report: {output_dir / 'summary.md'}")

    if args.require_opencl and opencl_missing:
        return 2
    if not comparison["passed"]:
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
