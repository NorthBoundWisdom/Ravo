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
- Apple used Metal in the first tranche. ADR-0134 replaces per-platform
  kernels with one QRhi compute backend. Other hosts report `gpu_unavailable`
  until that backend can create a device with compute and buffer readback.
  Cache identity will include backend and pipeline version when GPU pixels are
  published.
- Admission is staged:
  1. Device, queue, library, cancellation, and destruction.
  2. GPU apply of post-demosaic RGB on an existing linear working / ROI.
     Consecutive unmasked Exposure, light controls (highlights/shadows/whites/
     blacks), Lab USM Sharpen, and Sigmoid stay on one SSBO session (one
     upload, in-place passes, one download) so default RAW 1:1 grading does
     not drop quality or remosaic. Remaining kernels and masks stay CPU and
     interleave around that GPU batch. CPU remains the correctness reference
     (RMSE-gated).
  3. GPU Bayer window RCD with CPU-gold RMSE gates. Viewport ROI 1:1 uses
     that path when a compute backend exists; PPG stays on CPU.
- Export stays CPU until stage 3 goldens exist for the same recipe.

## Consequences

Studio 1:1 can later reuse CPU CFA decode plus GPU RGB without a 60MP PNG.
Device loss and teardown are explicit. OpenCL stays leftover.

## Rejected alternatives

- Reviving `GPU_Baseline.md` four-workload checklist before any adapter.
- Silent CPU copy when Metal init fails.
- QML/Qt Quick RHI as a second image pipeline. Engine-owned QRhi compute is
  ADR-0134.
- Porting leftover OpenCL kernels.
