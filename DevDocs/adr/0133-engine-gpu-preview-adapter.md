# ADR-0133: Engine GPU preview adapter

- Status: Accepted
- Date: 2026-09-03
- Extends: [ADR-0132](0132-viewport-roi-full-resolution-inspect.md)
- Relates to: [MIGRATION.md](../MIGRATION.md) GPU row

## Context

Viewport ROI 1:1 is CPU CFA-window demosaic (ADR-0132). On a 60MP Sony RAW a
typical 2100×1400 inspect crop still costs seconds in Debug because RCD and
RGB ops run on the CPU. RapidRAW is fast because a quality demosaic lives in
RAM and adjustments run as GPU compute. Ravo must not port 0.9 OpenCL or add a
second renderer in QML/catalog.

## Decision

- GPU is an Engine-owned adapter. Recipe, CatalogService, CLI, and QML do not
  hold device objects or GPU-specific business state. The Metal device, queue,
  and identity pipeline are created once per process; `EngineFacade` construction
  stays CPU-only.
- There is no silent CPU fallback. A GPU request that cannot run returns a
  structured error (`gpu_unavailable`, `gpu_pipeline_failed`, cancellation).
  CPU preview remains the default path until a caller opts in.
- Apple uses Metal. Other hosts report `gpu_unavailable` until their adapter
  exists. Cache identity will include backend and pipeline version when GPU
  pixels are published.
- Admission is staged:
  1. Device, queue, library, cancellation, and destruction.
  2. GPU apply of post-demosaic RGB on an existing linear working / ROI.
     Manual Exposure is the first opt-in kernel; CPU remains the default preview
     path and the correctness reference (RMSE-gated).
  3. GPU Bayer window demosaic with CPU-gold RMSE gates.
- Export stays CPU until stage 3 goldens exist for the same recipe.

## Consequences

Studio 1:1 can later reuse CPU CFA decode plus GPU RGB without a 60MP PNG.
Device loss and teardown are explicit. OpenCL stays leftover.

## Rejected alternatives

- Reviving `GPU_Baseline.md` four-workload checklist before any adapter.
- Silent CPU copy when Metal init fails.
- QML/Qt RHI as a second image pipeline.
- Porting leftover OpenCL kernels.
