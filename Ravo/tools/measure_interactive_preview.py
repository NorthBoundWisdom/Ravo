#!/usr/bin/env python3
"""Run Release preview probes on an isolated catalog through supported Ravo clients."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
from pathlib import Path
import subprocess
import sys
import tempfile


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def validate_burst_row(row: dict, runs: int) -> None:
    workload = row["workload"]
    count = workload["intent_count"]
    if (workload["schema"] != "ravo.perf01.burst-workload/v1"
            or not isinstance(count, int) or not 2 <= count <= 128
            or not 1 <= workload["requested_interval_ms"] <= 16
            or workload["warmup_kind"] != "complete_bursts"
            or workload["accepted_intent_order"] != "strictly_increasing"
            or workload["latest_target_verified"] is not True
            or workload["includes_monitor_presentation"] is not True
            or workload["includes_native_frame_swap"] is not False):
        raise RuntimeError("Invalid burst measurement contract")
    for key in ("intent_values", "intent_offsets_us", "accepted_frames", "frame_counts",
                "intents_without_publication", "start_revisions"):
        if len(workload[key]) != runs:
            raise RuntimeError("Uncorrelated burst metadata: " + key)
    for index in range(runs):
        values = workload["intent_values"][index]
        offsets = workload["intent_offsets_us"][index]
        frames = workload["accepted_frames"][index]
        if (len(values) != count or not all(math.isfinite(v) for v in values)
                or len(set(values)) != count or len(offsets) != count
                or any(not isinstance(v, int) or v < 0 for v in offsets)
                or any(a > b for a, b in zip(offsets, offsets[1:]))
                or not 1 <= len(frames) <= count
                or len(frames) != workload["frame_counts"][index]
                or len(frames) + workload["intents_without_publication"][index] != count):
            raise RuntimeError("Invalid burst intent/frame trace")
        previous_revision = workload["start_revisions"][index]
        previous_intent = -1
        previous_time = -1
        for frame in frames:
            intent = frame["intent_index"]
            if (not 0 <= intent < count or not intent + 1 <= frame["intents_sent"] <= count
                    or intent <= previous_intent or frame["revision"] <= previous_revision
                    or frame["elapsed_us"] < max(previous_time, offsets[intent])
                    or frame["width"] <= 0 or frame["height"] <= 0):
                raise RuntimeError("Stale, duplicate or unbound accepted burst frame")
            previous_intent, previous_revision, previous_time = intent, frame["revision"], frame["elapsed_us"]
        last = frames[-1]
        elapsed = (frames[0]["elapsed_us"] - offsets[0]
                   if row["case"].startswith("burst_first_") else last["elapsed_us"] - offsets[-1])
        if (last["intent_index"] != count - 1 or row["samples"][index] != elapsed
                or workload["values"][index] != values[-1]
                or workload["output_sizes"][index] != [last["width"], last["height"]]
                or workload["gpu_backends"][index] != last["gpu_backend"]):
            raise RuntimeError("Latest burst publication or timing does not match the trace")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True, help="One original RAW; never modified")
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--report-dir", type=Path, help="New directory outside the repository")
    parser.add_argument("--runs", type=int, default=30)
    parser.add_argument("--cli", type=Path)
    parser.add_argument("--studio-tests", type=Path)
    parser.add_argument("--catalog-tests", type=Path)
    parser.add_argument("--include-bursts", action="store_true", help="Measure repeated bursts through final image publication")
    parser.add_argument("--burst-intents", type=int, default=16)
    parser.add_argument("--burst-interval-ms", type=int, default=1)
    args = parser.parse_args()
    if not 30 <= args.runs <= 1000:
        parser.error("--runs must be between 30 and 1000")
    if not 2 <= args.burst_intents <= 128 or not 1 <= args.burst_interval_ms <= 16:
        parser.error("Burst intents must be 2–128 and interval must be 1–16 ms")
    repo = Path(__file__).resolve().parents[2]
    source = args.input.resolve(strict=True)
    build = args.build_dir.resolve(strict=True)
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
    if "CMAKE_BUILD_TYPE:STRING=Release\n" not in cache:
        parser.error("A configured single-config Release build is required")
    suffix = ".exe" if os.name == "nt" else ""
    cli = (args.cli or build / "Ravo/cli" / ("ravo" + suffix)).resolve(strict=True)
    studio = (args.studio_tests or build / "Ravo/tests" / ("ravo_desktop_command_tests" + suffix)).resolve(strict=True)
    catalog_tests = (args.catalog_tests or build / "Ravo/tests" / ("ravo_catalog_tests" + suffix)).resolve(strict=True)
    if any(not path.is_relative_to(build) for path in (cli, studio, catalog_tests)):
        parser.error("Executables must belong to the selected Release build directory")
    report = args.report_dir.resolve() if args.report_dir else Path(tempfile.mkdtemp(prefix="ravo-preview-matrix-"))
    if report.is_relative_to(repo):
        parser.error("Report artifacts must be outside the repository")
    if args.report_dir:
        report.mkdir(mode=0o700, parents=True, exist_ok=False)
    print(report, flush=True)
    manifest = {
        "schema": "ravo.interactive-benchmark/v1", "status": "running",
        "source": str(source), "source_sha256": digest(source),
        "source_sha": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip(),
        "worktree_diff_sha256": hashlib.sha256(subprocess.check_output(["git", "diff", "HEAD", "--"], cwd=repo)).hexdigest(),
        "untracked_source_files_sha256": {
            name: digest(repo / name)
            for name in sorted(subprocess.check_output(
                ["git", "ls-files", "--others", "--exclude-standard", "-z"], cwd=repo
            ).decode("utf-8").split("\0"))
            if name.startswith(("Ravo/", "configs/", "DevDocs/")) and (repo / name).is_file()
        },
        "harness_sha256": digest(Path(__file__)), "build_dir": str(build),
        "configuration": "Release", "runs": args.runs,
        "measurement_only": True,
        "bursts": {"enabled": args.include_bursts, "intent_count": args.burst_intents,
                   "requested_interval_ms": args.burst_interval_ms},
        "host": {"platform": platform.platform(), "machine": platform.machine(),
                 "logical_cpus": os.cpu_count()},
        "binaries": {str(path): digest(path) for path in (cli, studio, catalog_tests)},
    }
    env = os.environ.copy()
    # Never let an inherited performance gate or fixture redirect this run.
    for name in tuple(env):
        if name.startswith(("RAVO_INTERACTIVE_", "RAVO_PRESET_PERF_")):
            del env[name]
    env.update(RAVO_INTERACTIVE_PERF_RUNS=str(args.runs),
               RAVO_INTERACTIVE_PERF_REPORT_PATH=str(report / "samples.jsonl"),
               RAVO_LIVE_CONTROL_DIR=str(report / "control"))
    env.update(RAVO_INTERACTIVE_BURST_INTENTS=str(args.burst_intents),
               RAVO_INTERACTIVE_BURST_INTERVAL_MS=str(args.burst_interval_ms))
    catalog = report / "catalog.sqlite"

    command_index = 0

    def command(*words: str):
        nonlocal command_index
        proc = subprocess.run([str(cli), *words, "--json"], env=env, text=True,
                              encoding="utf-8", capture_output=True, timeout=120)
        command_index += 1
        (report / f"cli_{command_index:03d}.json").write_text(json.dumps({
            "arguments": list(words), "exit_code": proc.returncode,
            "stdout": proc.stdout, "stderr": proc.stderr}, indent=2), encoding="utf-8")
        if proc.returncode:
            raise RuntimeError(f"CLI exit {proc.returncode}: {proc.stdout[-4096:]} {proc.stderr[-4096:]}")
        result = json.loads(proc.stdout)
        if not result.get("ok"):
            raise RuntimeError(result)
        return result["data"]

    before: dict[str, object] = {}
    asset = ""
    errors: list[str] = []
    try:
        command("catalog", "create", "--path", str(catalog))
        command("catalog", "import", "--catalog", str(catalog), "--input", str(source))
        assets = command("catalog", "list", "--catalog", str(catalog))["assets"]
        if len(assets) != 1 or assets[0]["media_type"] != "image/x-raw":
            raise RuntimeError("The matrix requires exactly one imported RAW asset")
        asset = assets[0]["id"]
        manifest["asset"] = assets[0]
        env.update(RAVO_INTERACTIVE_PERF_CATALOG=str(catalog), RAVO_INTERACTIVE_PERF_ASSET_ID=asset)
        for kind in ("recipe", "history"):
            before[kind] = command("catalog", kind, "--catalog", str(catalog), "--asset-id", asset)
            (report / f"baseline_{kind}.json").write_text(json.dumps(before[kind], indent=2), encoding="utf-8")
        jobs = [(studio, "StudioInteractivePreviewPerformanceProbe.MeasuresNumericControlMatrixWithMonitor", "studio_matrix")]
        if args.include_bursts:
            jobs.append((studio, "StudioInteractivePreviewPerformanceProbe.MeasuresRepeatedNumericBurstsWithMonitor", "studio_bursts"))
        jobs += [(catalog_tests, "InteractivePreviewPerformanceProbe.MeasuresWarmExposureSweepWithoutCatalogMutation", f"service_{edge}")
                 for edge in (960, 1600, 1920)]
        for executable, case, name in jobs:
            local = env.copy()
            if name.startswith("service_"):
                local["RAVO_INTERACTIVE_PERF_MAX_EDGE"] = name.split("_")[1]
            with (report / f"{name}.log").open("w", encoding="utf-8") as log:
                subprocess.run([str(executable), "--gtest_filter=" + case], env=local,
                               stdout=log, stderr=subprocess.STDOUT, timeout=1800, check=True)
        rows = [json.loads(line) for line in (report / "samples.jsonl").read_text(encoding="utf-8").splitlines()]
        expected = {"numeric_control_" + name for name in (
            "exposure", "contrast", "highlights", "saturation", "whiteBalanceRed", "sharpen",
            "denoise", "clarity", "dehaze", "straighten")}
        expected |= {f"{name}_{edge}" for name in ("service_request", "service_owned_pixels_histogram")
                     for edge in (960, 1600, 1920)}
        if args.include_bursts:
            expected |= {f"burst_{boundary}_{field}" for boundary in ("first", "latest")
                         for field in ("exposure", "denoise", "clarity", "whiteBalanceRed")}
        if len(rows) != len(expected) or {row["case"] for row in rows} != expected:
            raise RuntimeError("Missing, duplicate or unexpected measurement rows")
        for row in rows:
            if (row["schema"] != "ravo.perf01.report/v1" or row["unit"] != "us"
                    or row["sample_count"] != args.runs or len(row["samples"]) != args.runs):
                raise RuntimeError("Invalid measurement contract")
            for key in ("values", "output_sizes", "gpu_backends"):
                if len(row["workload"][key]) != args.runs:
                    raise RuntimeError("Uncorrelated sample metadata: " + key)
            if row["case"].startswith(("numeric_control_", "burst_")):
                changed = row["workload"]["pixel_changed_from_previous"]
                if len(changed) != args.runs or not any(changed):
                    raise RuntimeError("No qualified pixel response: " + row["case"])
                manifest.setdefault("unresponsive_sample_indices", {})[row["case"]] = [
                    i for i, value in enumerate(changed) if not value]
            if row["case"].startswith("burst_"):
                validate_burst_row(row, args.runs)
            print(f'{row["case"]}: P50={row["p50"]/1000:.2f} ms P90={row["p90"]/1000:.2f} ms', flush=True)
        if args.include_bursts:
            by_case = {row["case"]: row for row in rows}
            for field in ("exposure", "denoise", "clarity", "whiteBalanceRed"):
                if by_case[f"burst_first_{field}"]["workload"] != by_case[f"burst_latest_{field}"]["workload"]:
                    raise RuntimeError("First/latest burst rows describe different trials")
        manifest["status"] = "complete"
    except Exception as error:
        manifest["status"] = "failed"
        errors.append(str(error))
    finally:
        try:
            manifest["source_unchanged"] = digest(source) == manifest["source_sha256"]
        except OSError as error:
            manifest["source_unchanged"] = False
            errors.append("Source verification: " + str(error))
        for kind, value in before.items():
            try:
                after = command("catalog", kind, "--catalog", str(catalog), "--asset-id", asset)
                manifest[kind + "_unchanged"] = after == value
            except Exception as error:
                manifest[kind + "_unchanged"] = False
                errors.append(kind + " verification: " + str(error))
        if not all(manifest.get(key, False) for key in ("source_unchanged", "recipe_unchanged", "history_unchanged")):
            manifest["status"] = "failed"
            errors.append("Source, recipe or history preservation was not proven")
        if errors:
            manifest["errors"] = errors
            for error in errors:
                print(error, file=sys.stderr)
        (report / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return 0 if manifest["status"] == "complete" else 1


if __name__ == "__main__":
    sys.exit(main())
