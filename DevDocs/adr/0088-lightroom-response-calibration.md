# ADR-0088: Lightroom response-calibrated light controls

- Status: Accepted
- Date: 2026-08-30
- Partially supersedes: [ADR-0085](0085-interchange-ready-grading-tools.md),
  [ADR-0086](0086-lightroom-crs-interchange.md)
- Extends: [ADR-0084](0084-studio-grading-curves.md),
  [ADR-0087](0087-progressive-develop-preview.md)

## Context

The first CRS adapter mapped Lightroom slider values onto similarly named Ravo
fields, but equal numbers did not produce equal response. In particular, a RAW
CRS contrast edit ran through the old scene-linear power curve instead of the
RAW sigmoid owner, highlights used a narrow Lab mask, and Adobe point curves
ran in scene-linear RGB before the display transform.

Calibration evidence consists of one Bayer RAW, Lightroom Classic 14.2 / Process
Version 15.4 endpoint exports for Exposure +1.12, Contrast +100, Highlights
+100, and Shadows -100, plus one 16-bit sRGB point-curve export and its CRS
sidecar. There is no neutral Lightroom export in that set. Adobe Standard and
Adobe Color are also deliberately absent from Ravo, so this evidence identifies
relative response, not an absolute Adobe pixel baseline.

## Decision

- `Exposure2012` remains an exact EV mapping. The engine continues to multiply
  scene-linear light by `2^EV` through the existing exposure owner.
- A CRS contrast edit applied to a RAW recipe writes the existing sigmoid
  contrast, not `ravo.core.contrast`. For normalized slider value `s` in
  `[-1, 1]`, the mapping is `1.5 * (3.25 / 1.5)^s`; therefore zero retains the
  1.5 default and +100 reaches 3.25. A raster recipe without sigmoid retains the
  normalized `ravo.core.contrast` mapping.
- Highlights and shadows use one scene-linear, hue-preserving exposure envelope
  around 18.42% grey. Highlight weight is `smoothstep(-4.5, 2.75, EV)` and
  shadow weight is `1 - smoothstep(-6.0, 0.75, EV)`. Slider endpoints apply
  +0.9/-1.8 EV for highlights and +2.0/-2.9 EV for shadows. The engine retains
  row cancellation and publishes no partial image on failure.
- `ravo.color.rgbcurve` schema v1 gains optional `application_space`; omission
  means `scene_linear`, preserving existing recipes and authored Ravo curves.
  `display_srgb` is restricted to independent R/G/B channels, no preserve-color
  mode, no middle-grey compensation, and no parametric curve. Unsupported
  combinations fail validation.
- A CRS master curve followed by R/G/B curves is composed into one 20-node
  `display_srgb` curve and ordered immediately after `ravo.display.sigmoid`.
  For node input `x`, the calibrated reference coordinate is `r = x^0.62` and
  each channel stores `clamp(x + 1.4 * (channel(master(r)) - r), 0, 1)`. The
  engine encodes the linear sigmoid result with the sRGB transfer function,
  evaluates that curve, then decodes it back before the output-profile owner.
- The CRS parser consumes nested `crs:Look` as one subtree so embedded Adobe
  look curves cannot replace the document's top-level curves. The built-in
  Adobe Color look and Adobe Standard profile remain explicit omissions.
  `PointColors` is accepted only at its all-`-1` identity state; any active
  point-color state fails closed.

## Consequences

CRS light sliders and point curves now follow the measured Lightroom response
shape substantially more closely while preserving Ravo's explicit colour
engine. This is not Lightroom rendering equivalence: camera DCP matrices,
Adobe Color's look table, highlight reconstruction, demosaic, and neutral
baseline tone remain different. A future calibration corpus must include a
neutral export, more cameras and scenes, and intermediate slider values before
changing these frozen endpoint constants. The supplied evidence directly pins
only positive Highlights and negative Shadows; their opposite directions and
negative Contrast are bounded Ravo extrapolations pending that corpus.

The highlights/shadows response changes for all current Ravo recipes because
those operations have one engine owner. Existing RGB curves remain
scene-linear unless their recipe explicitly requests `display_srgb`. The new
application-space field does not add a runtime fallback: invalid display-curve
policies return a structured validation error.

## Rejected alternatives

- Import Adobe Standard, Adobe Color, or clone PV2012. Those would introduce a
  second colour engine and violate the accepted interchange boundary.
- Keep imported point curves before sigmoid. The provided Lightroom curve is
  defined on an encoded display-like axis and produced the wrong response when
  evaluated in scene-linear RGB.
- Fit absolute output without a neutral Lightroom reference. That would bake
  unknown profile/look differences into unrelated Ravo controls.
- Ignore nested Look or active Point Colors. Silent best-effort import would
  make the reported preset differ from the applied preset.
