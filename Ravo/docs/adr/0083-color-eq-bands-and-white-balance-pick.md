# ADR-0083: Eight-band Color Equalizer editor and RAW white-balance pick

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0017](0017-explicit-raw-temperature.md),
  [ADR-0079](0079-develop-set-inventory-and-probe-png.md),
  [ADR-0082](0082-studio-develop-grading-workspace.md)

## Context

The default grading stack still hid Color Equalizer behind a 0–7 band index, so
neither photographers nor agents could see all eight hue partitions at once.
White balance remained four-channel coefficients with no way to sample a
neutral patch. ADR-0017 forbids storing Kelvin/tint as the canonical schema.

## Decision

- Studio presents Color Equalizer as eight named bands (Red through Magenta)
  with a Saturation / Hue / Lightness channel switch. Values remain
  `colorEqSatN` / `colorEqHueN` / `colorEqLightN`. QML does not invent band
  mathematics.
- `ravo inspect` reports as-shot and camera-reference four-channel
  coefficients for RAW so agents can start from camera metadata.
- White-balance pick samples a Bayer CFA window at preview-normalized
  coordinates (mapped through crop, user flip/rotate, then camera orientation)
  and writes **manual** `channel_scale_v4` coefficients. Green is the
  reference. Raster originals, straighten, and Canvas fail closed.
- CLI `catalog develop --pick-white=x,y` uses the same CatalogService path.
  Coordinates are in `[0, 1]` on the currently cropped preview.

## Consequences

Everyday HSL grading matches the CLI field inventory. Neutral sampling does
not revive the retired Kelvin/tint approximation. Picker, histogram, and
Kelvin/tint as stored schema remain later product decisions.

## Rejected alternatives

- Kelvin/tint as canonical temperature fields: rejected by ADR-0017.
- Sampling display-referred preview RGB: that would bake Sigmoid into WB.
- Raster JPEG pick in this tranche: JPEG is already white-balanced; a RGB
  multiply is a different contract.
