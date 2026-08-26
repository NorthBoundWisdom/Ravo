# ADR-0024: Own exposure analysis and metadata in immutable RAW state

- Status: Accepted
- Date: 2026-08-26

## Context

The frozen `legacy/src/iop/exposure.c` combines a linear-RGB exposure and
black-level operation with RAW deflicker analysis, camera-metadata
compensation, GTK spot-picker interaction, dynamic module proxies, blending,
and OpenCL dispatch. Ravo's earlier `ravo.core.exposure` v1 retained only a
manual EV value and therefore could not own the complete default CPU contract.

The 158 frozen XMP files contain 110 exposure revisions in 73 files. Their
final revisions use parameter versions 5, 6, and 7. History position, blend,
multi-instance, and mask state are not equivalent to canonical processing
order, and only an exact default-unmasked singleton has evidence for direct
mapping. The general mask graph is a separately queued owner and is not a
prerequisite for defining an exposure-specific compatibility boundary.

Exposure deflicker also requires data that cannot be reconstructed from a
demosaiced or resized preview. Its histogram is collected from the original
single-channel 16-bit RAW buffer, while exposure-bias and highlight-
preservation values come from format metadata. Both need an immutable lifetime
that survives profile conversions and interactive preview reuse without
leaking decoder or metadata-library handles.

## Decision

- `ravo.core.exposure` v2 stores an explicit mode (`manual` or `deflicker`),
  black correction, manual EV, deflicker percentile and target EV, and two
  independent compensation flags. A v1 operation upgrades by retaining its EV
  and supplying explicit v2 defaults. Unknown, missing, extra, non-finite, or
  out-of-range state fails schema validation.
- Manual mode computes
  `effective = exposure_ev - clamp(exposure_bias, -5, 5) +
  clamp(highlight_preservation, -1, 4)`, applying each metadata term only when
  its flag is enabled. Pixel output is
  `(sample - black) / (exp2(-effective) - black)`. The denominator and every
  source/result sample must be finite and representable before publication.
- Deflicker mode replaces manual EV and both metadata compensation paths. A
  65,536-bin histogram is built from the original immutable `DecodedRaw`
  before hot-pixel repair, highlight reconstruction, chromatic-aberration
  repair, demosaic, resize, or input-colour conversion. The selected bin is the
  first cumulative count greater than or equal to the double-precision
  threshold `pixel_count * percentile / 100`. Its EV is
  `log2(max(raw - black_level, 1)) - log2(white_level - black_level)`, and the
  effective correction is `target_ev - raw_ev`.
- RAW decode captures an owned value-only `RawExposureMetadata`. Missing tags
  are a successful read with zero EV. A file/metadata read failure is a
  distinct state and affects only a recipe that requests metadata
  compensation; ordinary manual exposure remains usable. Canon, Fujifilm,
  Nikon, Olympus, and Pentax highlight-preservation values use the frozen
  priority and mapping in a pure value function.
- Pinned Exiv2 0.28.8 is linked privately by `ravo_engine` only. Exiv2 handles
  and types never cross an engine boundary. Library error detail may appear in
  the current structured render failure, but it is not serialized into a
  recipe, catalog, preview record, or other persistent state.
- `ExposureAnalysisContext` is an owned immutable snapshot published as
  `shared_ptr<const ...>`. It carries the original RAW histogram, pixel count,
  RAW black/white levels, and metadata values. Input-colour conversion,
  working-profile conversion, primaries, exposure, and geometry operations
  retain the same snapshot; a scene-linear interactive-preview cache therefore
  reuses it instead of analysing transformed pixels.
- RAW memory estimation includes the histogram, context/control ownership,
  metadata string capacity, and the simultaneous source/context/repair-copy
  allocations. An insufficient budget fails before analysis allocation or
  pixel/cache/file publication. Histogram construction and exposure execution
  check cancellation before allocation and at bounded row/bin intervals;
  allocation, validation, cancellation, or metadata failure never mutates the
  input or publishes a partial output.
- Raster input supports manual EV and black because those require only the
  declared linear RGB buffer. Raster deflicker and metadata compensation are
  structured unsupported states; Ravo does not synthesize a RAW histogram or
  camera metadata.
- The legacy mode labelled “automatic” is the deterministic RAW deflicker
  contract above. The GTK area picker and its measure/correct interaction are
  presentation-time analysis intents, not serialized exposure mathematics.
  They are not recreated in QML and may return only through a future
  engine/service analysis contract with explicit source pixels, ROI,
  cancellation, and recipe revision.
- The strict XMP decoder accepts proven version-5/6/7 payloads, selects the
  greatest history `num` independently of XML order, and rejects conflicting
  duplicate revisions. It maps only exact default-unmasked singleton state.
  Non-zero or non-canonical multi state, names, real masks, and custom blend
  payloads return stable structured incompatibility. This is the accepted C9
  mask policy; it neither implements nor waits for the general mask graph.
- Shared legacy exposure proxy hooks in `develop/`, ordering and module-name
  strings, and the `basic.cl` exposure kernel are not runtime exposure owners.
  They remain only for the D0.4/S4/S14 shared registry, pixelpipe, and OpenCL
  retirement batches.

## Consequences

CLI render, Catalog preview/save/reopen/export, and Studio controls consume one
versioned recipe and the same engine implementation. Studio presents the
legacy soft ranges while recipe validation retains the hard bounds. Originals
remain read-only, successful output/profile storage is owned, and cache
identity includes every canonical exposure field.

The exposure-specific unmasked-only policy is final even while the general
mask graph remains queued. The accepted CPU, schema, importer, consumer,
resource, and failure contracts allow `legacy/src/iop/exposure.c` and its exact
CMake registration to retire without deleting shared proxy/order/OpenCL files.

## Rejected alternatives

- Build deflicker from demosaiced, repaired, resized, or display-referred
  pixels: that changes the frozen percentile and RAW-EV semantics.
- Treat missing metadata and metadata read failure as the same zero value: it
  hides I/O failure when compensation was explicitly requested.
- Expose Exiv2 objects through the engine facade or cache: this leaks parser
  lifetime and third-party ABI into public state.
- Fall back to manual mode for raster deflicker or failed RAW analysis: the
  rendered result would no longer represent the recipe.
- Recreate the spot picker or histogram ownership in QML: presentation would
  gain a second image-analysis and revision lifecycle.
- Delay exposure retirement until the general mask graph exists: C9 has a
  tested exact default-unmasked mapping and explicit rejection strategy, which
  satisfies the migration policy without claiming masked compatibility.
