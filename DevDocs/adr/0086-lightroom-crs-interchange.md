# ADR-0086: Fail-closed Lightroom CRS interchange

- Status: Accepted; response mapping partially superseded by ADR-0088
- Date: 2026-08-29
- Extends: [ADR-0085](0085-interchange-ready-grading-tools.md),
  [ADR-0063](0063-explicit-no-automatic-sidecar-policy.md),
  [ADR-0065](0065-versioned-recipe-style-artifact.md)
- Relates to: [ADR-0017](0017-explicit-raw-temperature.md)

## Context

A Lightroom Classic preset is Adobe Camera Raw Settings XMP (`crs:`), not a
darktable history sidecar and not a Ravo `.rstyle.json`. `recipe import-xmp`
used the leftover darktable importer, treated missing `darktable:history` as
empty history, and wrote only default colour in/out. That swallowed the
preset. ADR-0085 completed Ravo-owned destinations; this tranche is the
adapter.

## Decision

- Adapters own CRS parse and mapping. Recipe still owns `DevelopParams`.
  The leftover darktable importer rejects `crs:` rather than succeeding as
  empty history.
- `recipe import-xmp` detects the CRS namespace and writes a canonical recipe
  through `recipe_from_develop`. `catalog develop --from-xmp` overlays the
  mapped look onto a catalog photo. Studio keeps imported presets in a
  `Ravo Presets` folder beside the open library and lists them above History
  on the Edit left rail; **Import…** / **File → Import Preset…** copy the file
  there and apply it to the selected photo. Crop, masks, Retouch, profiles, and
  RAW repair are left unless the CRS document itself carries a mapped field in
  that group.
- Mapping is onto accepted Ravo operations with documented numeric scales. It
  is not a PV2012 colour engine, Adobe DCP, or Kelvin/tint schema.
- Unknown `crs:` keys fail closed. Identity-default keys (Texture 0, Upright
  0, extra NR Detail/Contrast/Smoothness at Lightroom defaults, and similar)
  may be omitted. `CameraProfile` Adobe Standard / Adobe Color / Embedded /
  Camera Settings / Default is not applied and is listed in `omitted`. Named
  custom DCP profiles fail. White Balance other than As Shot fails.
  `PostCropVignetteStyle` is approximated by Ravo radial vignette and listed
  in `omitted`; non-zero Highlight Contrast fails.
- Parametric amounts plus independent RGB point curves together fail closed.
  ADR-0088 supersedes the initial master-plus-channel mapping: those curves are
  composed into one independent display-sRGB `rgb_curve` after sigmoid.

## Consequences

A CRS preset either becomes a real recipe/look or returns `unsupported` with
the key and reason. Empty-history success is not a CRS result. `.rstyle.json`
remains the portable Ravo preset; CRS is an explicit import/apply dialect.

## Rejected alternatives

- Teaching the leftover darktable importer to ignore `crs:`. That is the
  empty-history swallow.
- Cloning PV2012 or loading Adobe Standard. Those are a second colour engine.
- Best-effort drop of unknown keys. A photographer cannot tell what was lost.
