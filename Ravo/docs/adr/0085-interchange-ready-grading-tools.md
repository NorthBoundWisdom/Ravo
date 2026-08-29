# ADR-0085: Interchange-ready everyday grading tools

- Status: Accepted; response mapping partially superseded by ADR-0088
- Date: 2026-08-29
- Extends: [ADR-0082](0082-studio-develop-grading-workspace.md),
  [ADR-0084](0084-studio-grading-curves.md),
  [ADR-0020](0020-working-profile-rgb-primaries.md)
- Relates to: [ADR-0017](0017-explicit-raw-temperature.md),
  [ADR-0063](0063-explicit-no-automatic-sidecar-policy.md),
  [ADR-0065](0065-versioned-recipe-style-artifact.md)

## Context

A Lightroom Classic PV2012 preset (Adobe CRS XMP) is not a darktable leftover
sidecar and is not a Ravo `.rstyle.json`. `recipe import-xmp` currently treats
a CRS file as empty darktable history and writes only default colour in/out.
Photographers still need the *Ravo-owned* tools that such a preset would map
onto before any adapter exists.

This tranche completes those tools. It does not parse `crs:` XML, does not add
Kelvin/tint as canonical schema, and does not ship Adobe camera profiles.

## Decision

- Color Equalizer remains the eight-band hue/saturation/lightness mixer.
  Display names are Red, Orange, Yellow, Green, Aqua, Blue, Purple, Magenta so
  a later CRS HSL mapper has a 1:1 band list. Math stays dt UCS 8-node RBF.
- Camera Calibration is the existing `ravo.color.primaries` section, placed on
  the default grading path after Color and before Geometry. Studio labels
  achromatic tint as shadow tint. Paste Color also applies primaries.
- Vignette authors the parameters the engine already evaluates: signed amount
  (−1..1, positive darkens), midpoint, falloff, shape, and centre. Recipe no
  longer hard-codes midpoint 0.8 / falloff 0.5. Highlight-priority / colour-
  priority / paint-overlay styles stay unsupported.
- Detail exposes luminance denoise, chroma denoise, and radius beside the
  accepted Lab USM sharpen (amount / radius / threshold). Lightroom “Detail”
  and “Masking” are not new algorithms; a later adapter may map masking onto
  threshold.
- Explicitly out of this tranche, and fail-closed for a future CRS adapter:
  Adobe Standard / DCP camera profiles, Process Version 2012 as a colour
  engine, Kelvin/tint storage, Texture, Upright/perspective profiles, and
  silent empty-history import of `crs:` files.

## Consequences

Studio can author every PV2012 look-group that Ravo already owns. A later
adapter writes only these fields and rejects Adobe-only state. Existing
recipes with vignette amount in 0..1 remain valid; positive amount still
darkens. Profile denoise (`ravo.detail.denoiseprofile`) follows the Detail
section lamp, not RAW bypass.

A later fail-closed CRS adapter maps onto these Ravo owners. Numeric scale,
default omission, and Adobe-only keys are adapter work; this table is the
destination contract:

| CRS group | Ravo destination | Adapter notes |
| --- | --- | --- |
| WhiteBalance As Shot | `temperature.mode = as_shot` | Kelvin/tint storage stays forbidden (ADR-0017). |
| Exposure2012 / Contrast2012 / Highlights2012 / Shadows2012 / Whites2012 / Blacks2012 | `exposure_ev`, RAW `sigmoid_contrast` or raster `contrast`, `highlights`, `shadows`, `whites`, `blacks` | ADR-0088 owns the calibrated light-response mapping. |
| Vibrance / Saturation / Clarity2012 / Dehaze | `vibrance`, `saturation`, `clarity`, `dehaze` | Texture remains unsupported. |
| Hue/Saturation/LuminanceAdjustment Red…Magenta | `color_eq_hue/sat/light` bands 0–7 | Display names are 1:1 with that list. |
| SplitToning* | `split_toning` | Zero saturation is identity and may be omitted. |
| ParametricShadows/Darks/Lights/Highlights and splits | `rgb_curve` parametric fields | Linked RGB only. |
| ToneCurvePV2012 | `rgb_curve` | ADR-0088 supersedes the provisional master-plus-channel mapping with one composed display-sRGB curve. |
| ToneCurvePV2012Red/Green/Blue | `rgb_curve` independent channels | |
| Sharpness / SharpenRadius / SharpenEdgeMasking | `sharpen`, `sharpen_radius`, `sharpen_threshold` | SharpenDetail is not a separate algorithm. |
| LuminanceSmoothing / ColorNoiseReduction | `denoise`, `denoise_chroma` | Extra NR Detail/Contrast/Smoothness keys stay unsupported. |
| PostCropVignette Amount/Midpoint/Feather/Roundness | `vignette`, `vignette_midpoint`, `vignette_falloff`, `vignette_shape` | Positive Ravo amount darkens; CRS negative Amount maps onto that. Styles (highlight-priority / colour-priority / paint-overlay) stay unsupported. |
| Red/Green/Blue Hue+Saturation, ShadowTint | `primaries` | CameraProfile Adobe Standard / DCP fail closed. |

## Rejected alternatives

- Parsing Lightroom XMP in the leftover darktable importer. CRS is a different
  schema; empty-history success would keep swallowing presets.
- Cloning PV2012 tone math or Adobe DCP. That is a second colour engine, not
  a mapping onto accepted Ravo operations.
- Storing Kelvin/tint. ADR-0017 still forbids it as canonical schema.
