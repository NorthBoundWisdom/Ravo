# ADR-0061: Preview scopes use fixed engine-owned RGB and D50 u*v* contracts

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0060](0060-studio-navigation-lifecycle.md)

## Context

Studio initially exposed the frozen 256-bin RGB histogram and RGB Parade.
The old GTK owner also offered overlaid waveform, several vectorscope color
models, split presentation, profile selection, harmony editing, and exposure
dragging. L8 required complete useful diagnostic modes without carrying GTK,
global pixelpipe, or picker/edit behavior into QML.

## Decision

- Engine accepts one exact RGB8 processed-preview raster and owns all scope
  pixel statistics. Buffer dimensions and size are exact; undersized, oversized,
  empty, or overflowing inputs fail structurally.
- Histogram retains 256 display-code bins and the frozen bin-zero-excluded
  maximum. Waveform and Parade retain 160 tones, at most 360 integral-width
  bins, 1.0 at 8/9 height, frozen area brightness, and HLG display encoding.
  Waveform adds RGB planes; Parade places them in three columns.
- Vectorscope is fixed to linear-scale D50 CIE L*u*v*. It averages 2×2 encoded
  RGB samples, applies the sRGB transfer and fixed linear-Rec.709-to-XYZ-D50
  matrix, subtracts the D50 white chromaticity, bins into a 384-square plot at
  a fixed ±200 u*/v* radius, and uses the frozen count gain plus HLG intensity.
- Split is an engine-produced overlaid RGB Waveform plus max-preserving
  downsampled Vectorscope. Studio adds only grids, labels, and mode selection.
- Studio modes are Histogram, Waveform, Parade, Vectorscope, and Split. All
  refresh from the same processed preview or current Gallery thumbnail and use
  the existing revisioned image provider.
- AzBz/RYB modes, logarithmic scaling, hue-ring backgrounds, harmony editing,
  profile selection, point samples, and scope-driven exposure dragging are
  unsupported. They are not diagnostic requirements and get no inactive UI.

## Consequences

The old GTK histogram/scopes module, shared scope header, and registration are
removed. Preview scope calculations remain synchronous and bounded by the
already bounded preview/thumbnail; they do not mutate a recipe, original, or
catalog. Generic color-science and pixelpipe owners remain for other consumers.

## Rejected alternatives

- Computing waveform/vectorscope math in Canvas. QML may draw presentation but
  must not own pixel/color algorithms.
- Porting every old vectorscope option. Those controls combined creative color
  harmony and exposure editing with diagnostics and are outside the Ravo
  product contract.
- Sampling the original independently. Scopes must describe exactly the
  processed pixels currently shown.
