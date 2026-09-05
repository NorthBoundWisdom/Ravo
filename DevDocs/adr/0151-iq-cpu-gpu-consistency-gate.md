# ADR-0151: IQ-00 CPU gold and GPU consistency gate

- Status: Accepted
- Date: 2026-09-04
- Relates: IQ-00 in [TODO.md](../TODO.md)
- Extends: [ADR-0133](0133-engine-gpu-preview-adapter.md),
  [ADR-0134](0134-engine-qrhi-gpu-backend.md)
- Aligns with: persist/export CPU-gold fix (`render_linear_working` /
  `render_linear_working_export` never opt into GPU)

## Context

IQ-00 requires a dated release gate so GPU latency work cannot silently change
settled preview, reopen, or export bytes. ADR-0133 already makes CPU the RMSE
reference and forbids silent CPU fallback on a failed GPU request. The Engine
now separates:

- **CPU gold path** — `render_linear_working` / `render_linear_working_export`
  (Catalog persist preview, settled cache, CLI PNG, export).
- **Interactive GPU path** — `render_interactive_linear_working` (Studio live
  develop; may report `gpu_backend` and optionally retain IOSurface pixels).

IQ-00 needs the contract, tolerances, fail-closed policy, and first Ready
hooks dated without claiming the full corpus matrix is closed.

## Decision

### Authority

1. **CPU is the gold reference** for persist preview, settled save, close/reopen
   of persisted preview, CLI probe/render that downloads pixels, and all export
   formats.
2. **Preview / export / reopen equality on the CPU gold path** is **bit-exact**
   for the same asset fingerprint, recipe JSON, geometry (including ROI),
   output colour, and dither target. Documented float working-buffer equality
   before packing uses the Engine’s existing deterministic CPU ops; packed
   RGB8/16 export and preview PNG bytes must match on reopen of the same
   persisted inputs.
3. **GPU interactive preview** may run only on the interactive path. When a
   GPU batch is admitted, its linear working result must match the CPU gold
   within **documented tolerances** (`kIqGpuCpuWorkingAbsTolerance = 2e-3` per
   channel for the current Exposure/light/Lab-USM/Sigmoid batch; RMSE gates for
   future Bayer RCD remain ADR-0133 stage 3). If a GPU request cannot meet the
   gate or the adapter is unavailable, the call **fail-closes** with a
   structured reason (`gpu_unavailable` / `gpu_pipeline_failed`) — never a
   silent lower-quality algorithm and never a silent substitute for persist or
   export bytes.
4. **Persist and export never select the interactive GPU path.**
   `gpu_backend` on those results stays empty (reported as `cpu` at CLI).

### First Ready (this ADR)

- Publish contract constants and helpers under `ravo.iq.consistency/v1`.
- Contract tests assert CPU gold for persist `render_linear_working` and
  export `render_linear_working_export`, bit-exact CPU re-render equality, and
  that interactive GPU (when available) stays within the documented abs
  tolerance versus CPU for an admissible recipe.
- `catalog probe --json` reports an `iq_consistency` object describing the
  gold policy and residual GPU live path.

### macOS Metal admitted-stage packed RGB8 (follow-on)

- Schema version **2** lists `gpu_admitted_interactive_stages` (Exposure,
  highlights/shadows/whites/blacks, Lab USM sharpen, linear-sRGB Sigmoid) and
  `non_admitted_interactive_policy`.
- Owned-pixel interactive vs CPU-gold packed RGB8 stays within
  `kIqGpuCpuPackedRgb8AbsDelta` for those stages (and an admitted develop
  stack) on macOS Metal; non-admitted ops remain CPU
  gold bit-exact on the interactive hybrid. Persist/export/reopen stay CPU
  gold. Win/Linux hosts are not claimed.

### RapidRAW tone-stage admission (follow-on)

- Schema version **3** adds `ravo.core.rapidraw-tone-controls` and
  `ravo.display.rapidraw-basic` to `gpu_admitted_interactive_stages`.
- The all-control stack is compared with CPU gold at the same working-float
  tolerance and packed-RGB8 delta. Persist, export, and reopen remain CPU gold;
  the wider platform/corpus residual is unchanged.

### Contrast interactive admit (follow-on)

- Schema version **4** adds `ravo.core.contrast` to
  `gpu_admitted_interactive_stages` with a Metal/RHI `contrast_rgb` pass that
  mirrors CPU middle-grey luminance power contrast.
- Owned packed RGB8 stays within `kIqGpuCpuPackedRgb8AbsDelta` vs CPU gold on
  macOS Metal; persist/export/reopen remain CPU gold. Other non-admitted
  interactive ops stay on the CPU hybrid.


### Gamma / vibrance / saturation interactive admit (follow-on)

- Schema version **5** adds `ravo.core.gamma`, `ravo.color.vibrance`, and
  `ravo.color.saturation` to `gpu_admitted_interactive_stages` with Metal/RHI
  `gamma_rgb` and `vibrance_saturation_rgb` passes that mirror CPU
  `apply_gamma` / fused `apply_vibrance_saturation`.
- Owned packed RGB8 stays within `kIqGpuCpuPackedRgb8AbsDelta` vs CPU gold on
  macOS Metal; persist/export/reopen remain CPU gold. Remaining non-admitted
  interactive ops (curves/levels, color-balance*, clarity/texture/dehaze,
  velvia/monochrome/split-toning, effects, equalizers, LUT, masked ops, etc.)
  stay on the CPU hybrid.



### Velvia / split-toning interactive admit (follow-on)

- Schema version **6** adds `ravo.color.velvia` and `ravo.color.splittoning` to
  `gpu_admitted_interactive_stages` with Metal/RHI `velvia_rgb` and
  `split_toning_rgb` passes that mirror CPU `apply_velvia` /
  `apply_split_toning` (pointwise HSL for split-toning).
- Owned packed RGB8 stays within `kIqGpuCpuPackedRgb8AbsDelta` vs CPU gold on
  macOS Metal; persist/export/reopen remain CPU gold. Monochrome stays hybrid
  (Lab convert + bilateral filter residual). Other non-admitted interactive ops
  (curves/levels, color-balance*, clarity/texture/dehaze, effects, equalizers,
  LUT, masked ops, etc.) stay on the CPU hybrid.



### Color Contrast interactive admit (follow-on)

- Schema version **7** adds `ravo.color.colorcontrast` to
  `gpu_admitted_interactive_stages` with a Metal/RHI `color_contrast_rgb` pass
  that mirrors CPU `apply_color_contrast` (pointwise D50 Lab a/b affine on
  linear Rec.709).
- Owned packed RGB8 stays within `kIqGpuCpuPackedRgb8AbsDelta` vs CPU gold on
  macOS Metal; persist/export/reopen remain CPU gold. Remaining non-admitted
  interactive ops (curves/levels, color-balance*/colorbalancergb, color
  zones/harmonizer/correction/reconstruction, monochrome Lab+bilateral,
  clarity/texture/dehaze, effects, equalizers, LUT, masked ops,
  non-linear-Rec709 sharpen, non-linear-sRGB sigmoid) stay on the CPU hybrid.

### Explicit residuals

- Full IQ-00 matrix (RAW corpus, ICC/proof, multi-instance locals, denoise,
  watermark/frame, GPU-unavailable/exhausted/cancelled, 1:1 ROI vs export
  overlap) still open.
- Studio window-move / monitor presentation remains ADR-0144 (presentation
  only; never mutates recipe/export).
- Expanding GPU batches beyond the current admitted RGB ops requires new
  dated tolerances before they may leave the interactive path.
- RAW viewport ROI may still report GPU; size-matched packed comparison vs
  export remains an evidence residual.

## Consequences

IQ-00 gains a testable first Ready that freezes CPU gold for durable pixels
while documenting the interactive GPU residual without claiming corpus closure.

## Rejected alternatives

- Allowing persist/export to share the interactive GPU path with download.
- Silent CPU fallback when GPU init or RMSE fails.
- Screenshot-based GPU acceptance.
