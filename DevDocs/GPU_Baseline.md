# Ravo GPU: baseline and admission rules

This document defines the measurements that must be fixed before a GPU adapter
such as Metal is added to the Ravo CPU engine. It is not a shader implementation
guide. Do not begin GPU operation implementation before workloads, CPU goldens,
and the first contiguous hot chain have completed review.

> Current status (2026-07-21): the existing runner, float-pixel comparison, and
> GPU-routing statistics are frozen 0.9 read-only measurement assets. Version
> 0.9 receives no OpenCL-to-Metal change, pixelpipe refactor, or second cleanup
> pass. Once the Ravo base product and later CPU editing paths meet their root
> TODO acceptance gates, the GPU phase reuses this method with a runner that
> compares Ravo CPU and Ravo GPU. This does not authorize old-code changes or
> GPU implementation work today.

## Frozen 0.9 observability

-d perf provides per-IOP host-side timing and final execution backend; -d opencl
provides OpenCL command-queue event timing. This repository also emits one
dev_pixelpipe_summary record with:

- execution attempts for the pixelpipe;
- executed CPU/GPU module counts and processed pixels, including retries after
  failure;
- contiguous GPU segment count and CPU/GPU backend-switch count;
- theoretical endpoints of GPU segments, useful for segmentation analysis but
  not equivalent to actual copy count;
- cumulative host-side wall time for CPU/GPU modules.

GPU host timing can include only asynchronous submission rather than real kernel
execution. OpenCL event profiling is authoritative for kernel/queue timing;
complete pixelpipe and process time decide end-to-end tradeoffs. The fields are
research input to Ravo report schema and do not require Ravo to reproduce old
logs, module ABI, or OpenCL scheduling.

## Fixed workloads

A formal baseline has four version-fixed, redistributable inputs and their XMP.
The originals need not be in Git, but the baseline manifest records SHA-256,
camera, dimensions, source/license, and XMP SHA-256.

| ID | Content | Required processing coverage |
| --- | --- | --- |
| raw-detail | High-pixel-count, fine-texture, low-ISO Bayer RAW | demosaic, lens correction, input/output colour, local contrast or sharpening |
| raw-noise | High-ISO Bayer/X-Trans RAW with substantial shadows | demosaic, chroma/luma denoise, exposure, tone mapping |
| raw-geometry | Architectural or grid RAW | rotate/crop, perspective or lens geometry, scaling/interpolation |
| raw-mask | RAW with both bright/dark regions and saturated colours | drawn/parametric mask, blend, colour selection, and at least one expensive IOP |

Each workload measures:

1. Fast preview: 1600px long edge with product-defined preview policy.
2. 100% darkroom: fixed ROI without window-size or zoom-state drift.
3. Full-size export: high-quality processing, 32-bit float TIFF, no downscale.

The frozen legacy CLI runner covers only full-size export. Ravo GPU must put
preview and export into the same versioned report schema; full-size export data
cannot substitute for interactive first-frame or update latency.

## Repeatable execution

The following is solely for reproducing the historical frozen-0.9 CPU/OpenCL
report. Never modify src to make it pass. Do not load plugins from a long-lived
incremental-build directory: stale .so files can be dynamically discovered and
pollute results. Build the frozen commit and install it into a fresh staging
prefix:

~~~sh
cmake --build --preset mac_clang_release -j 8
cmake --install build/mac_clang_release --prefix /tmp/dtn-gpu-stage
python3 benchmarks/gpu_baseline.py --input /corpus/raw-detail.dng --xmp /corpus/raw-detail.dng.xmp --output-dir /tmp/dtn-gpu-raw-detail --cli /tmp/dtn-gpu-stage/bin/darktable-cli --data-dir /tmp/dtn-gpu-stage/share/darktable --module-dir /tmp/dtn-gpu-stage/lib/darktable --warmups 2 --runs 7 --require-opencl
~~~

The legacy runner creates isolated CPU/OpenCL config/cache, uses an in-memory
database, disables custom presets, and forces 32-bit float TIFF. Output
directory must be empty. It creates:

- report.json: Git/binary/input hashes, complete command, per-run timing,
  per-module routing, GPU segments, OpenCL queue time, float-pixel error, and
  output hashes;
- summary.md: medians and slowest modules;
- raw .log and measured .tif for every run.

With libtiff, the runner reads decoded float scanlines from the first measured
CPU/OpenCL outputs, ignores volatile TIFF timestamp/path metadata, and computes
whole-image and per-channel error. RMSE and maximum error scan all samples. For
images above one million samples, P99 uses fixed-stride deterministic sampling
and records count/stride in JSON. Defaults match the complex-operation admission
line below; --rmse-limit and --max-abs-limit may tighten a workflow.

Keep machine, macOS version, power/low-power mode, thermal state, release build,
thread count, input/XMP, output dimensions, and background load fixed. Formal
reports use two warmups and at least seven measurements, reporting median, P90,
and dispersion. A single run is a smoke check only.

## CPU goldens and correctness gates

Generate goldens on a designated release commit in CPU-only mode and version
them with input/XMP/binary hashes, command, and colour configuration. Do not
store only final 8-bit JPEG: store at least 32-bit float TIFF and progressively
add linear-float dumps at critical IOP boundaries.

Every Ravo GPU candidate first satisfies:

- exactly identical dimensions, channels, ROI, and geometry mapping; all output
  is finite and introduces no NaN/Inf;
- matching alpha, inside/outside mask semantics, and unprocessed channels;
  discrete labels/indices are bit-exact;
- normal floating-point operation: normalized-channel RMSE <= 2e-6 and
  max_abs <= 2e-5;
- iterative, atomic-reduction, denoise, or demosaic operation: RMSE <= 2e-5 and
  max_abs <= 2e-4;
- final display reference: CIEDE2000 P99 <= 0.25 and maximum <= 1.0, followed
  by manual inspection of edges, shadows, saturated highlights, mask edges, and
  geometry boundaries.

These are first-round admission lines, not permission to loosen all-operation
precision. If CPU itself has platform floating-point drift, quantify it with
repeated CPU runs and cross-machine data, then record a narrower exception as
operation + parameter range + reason. Do not expand global tolerance because a
GPU candidate failed.

## Performance and first-chain selection

Group candidate chains in actual execution order, not .cl-file order. Select
chains that simultaneously:

- are major end-to-end time in at least one representative workload;
- can remain on GPU across adjacent operations and amortize boundary/format
  conversion cost;
- reuse shared colour/format, interpolation, blend/mask, Gaussian/NLM, or other
  dependencies across multiple IOPs;
- have clear CPU-fallback and trusted-input retry boundaries after error.

Write concrete Ravo GPU admission targets after collecting the baseline. At a
minimum, core workflows need repeatable end-to-end median benefit, no P90
regression, and no unexplained peak-memory or energy regression. A single
kernel may be faster but the candidate fails if boundaries, waits, or cache
invalidation slow a complete Ravo render.

## Ravo GPU admission exit checklist

- [ ] Versioned manifest for four workloads and three product paths.
- [ ] Formal CPU goldens and a versioned manifest.
- [x] 32-bit float TIFF pixel comparator with configurable tolerance gates.
- [x] CPU/OpenCL CLI runner and structured report.
- [x] Per-module backend, contiguous-GPU-segment, and routing-endpoint stats.
- [ ] Preview first-frame/interactive-update, peak-memory, and energy samples.
- [ ] First contiguous module chain and end-to-end benefit gate selected from
  reports.

Only after every item is complete may Ravo engine add backend-neutral ports and
GPU-adapter implementation. Frozen 0.9 pixelpipe and OpenCL paths remain
unchanged.
