# ADR-0069: Dither and posterize run after Output Color

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0019](0019-explicit-output-color-profiles.md)

## Context

The frozen Dither IOP is display-referred output mathematics, not a
scene-linear creative approximation or an encoder-specific byte filter. It
offers random noise, Floyd–Steinberg quantization, and posterization. Ravo must
place the operation after the recipe's Output Color transform but before
RGB8/RGB16/float packing, while keeping preview and export target behavior
explicit.

## Decision

- `ravo.output.dither` schema v1 declares `encoded_output_rgb`,
  `frozen_dither_v2`, one of all 18 frozen method names, and random damping from
  -200 through 0 dB. It is explicit-presence state and must be the final recipe
  operation immediately after Output Color. Masks and duplicate instances
  reject.
- Floyd–Steinberg keeps the frozen round-half-down quantizer, Rec.601 gray
  weights, 7/16–3/16–5/16–1/16 row-major diffusion, tiny-image no-diffusion
  rule, and the export level counts for 1/2/4/6/8/16-bit methods. It uses the
  source's compatibility row order, not the alternate two-row fast order.
- Auto resolves RGB8 to 256 levels and RGB16 to 65,536 levels. Preview and
  float output retain the frozen no-dither branch and clamp finite samples to
  [0,1]. Ravo's export buffer is RGB; conditional TIFF grayscale conversion is
  a later encoder concern and does not silently change auto to gray diffusion.
- Posterize preserves 2–8 per-channel levels and the same quantizer for every
  target, including float output.
- Random mode preserves the frozen 8-round TEA keys, triangular distribution,
  one noise value shared by RGB, `2^(dB/10)` amplitude, and clamp. Ravo fixes
  one logical serial TEA stream; this is the source one-thread sequence and
  removes the old hardware-thread-count dependence without changing the PRNG
  or distribution.
- Input dimensions/layout/profile and every sample are validated before
  mutation. Rows and pre-publication check cancellation; failures publish no
  caller-visible result. No GPU path or silent non-finite repair is added.
- Strict XMP import accepts only the three enabled schema-v1 singleton records
  evidenced by 0043 Floyd–Steinberg, 0044 random, and 0136 posterize, including
  their exact reserved bytes and versioned default blends. Disabled 0086,
  modified/cross-paired/masked/multi/duplicate state rejects.
- Recipe/Develop/CLI/Catalog/Studio use the same operation. Explicit methods
  appear in preview; auto remains target-aware. Styles, history, cache, reopen,
  and export preserve the full canonical state.

## Consequences

The old `iop/dither.c`, registration, and exclusive darkroom icons are removed.
Shared TEA helpers and `extended.cl` remain for Vignette; old order/modulegroup/
manual names remain D0 cleanup. O1 is accepted. G8 may now consume this output
quantization contract without reimplementing dithering in Borders or an
encoder.

## Rejected alternatives

- Ordered/blue-noise substitution. It is not the frozen owner being migrated.
- Applying dither in linear Rec.709 or after integer packing. Both change the
  quantization domain and colour result.
- Seeding from thread ID or hardware concurrency. Identical recipes must not
  vary with worker count.
- Treating posterize as an 8-bit encoder option. It is visible recipe state and
  must survive float export and style reuse.
