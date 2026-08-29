# ADR-0071: Watermark uses deterministic built-in text instead of external SVG state

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0070](0070-canvas-and-output-frame-contract.md)

## Context

The old Watermark IOP discovers SVG/PNG files from installation and user
configuration directories, delegates SVG and font rendering to host libraries,
and expands a broad mutable variable namespace. Its only frozen fixture names
`promo.svg`, which is absent from the frozen repository; the old failure path
silently copies the input. None of that supplies a portable positive resource
or deterministic cross-machine font contract.

Ravo has no obligation to preserve an unversioned resource lookup ABI. A useful
watermark still needs visible text/signature placement, recipe/style
persistence, preview/export equality, and explicit failure behavior.

## Decision

- Introduce `ravo.output.watermark` schema v1 in `encoded_output_rgb` with a
  versioned built-in 5×7 monospaced glyph set. State owns bounded text,
  foreground RGB, opacity, scale, rotation, nine-position alignment, and
  normalized offsets.
- Text accepts a documented printable ASCII subset plus newline and only the
  deterministic `{stem}` and `{asset_id}` tokens derived from the recipe source.
  Unsupported characters, unknown tokens, empty expansion, and oversized
  layout fail before output allocation.
- Watermark runs after final Frame and before sample packing. The engine owns
  glyph coverage, rotation, placement, premultiplied-style alpha composition,
  finite validation, memory bounds, cancellation, and publication. Recipe,
  Catalog, CLI, Studio, history, styles, preview, and export share that stage.
- Arbitrary SVG/PNG lookup, user configuration directories, system fonts,
  Pango descriptions, EXIF/tag variable expansion, and silent missing-resource
  no-op are explicitly unsupported first-version capabilities. The missing
  `promo.svg` legacy record rejects structurally rather than mapping to an
  invisible edit.
- Once the vertical slice is accepted, retire `iop/watermark.c`, its
  registration/icons, and all six unconsumed legacy watermark SVGs. Old order,
  module-group, manual, crop, imageio, and develop diagnostic strings remain
  with their shared owners until their own gates close.

## Consequences

Text watermarks are portable and reproducible without a new runtime dependency.
Historic arbitrary logos and metadata cards do not import; a later independent
version may add an embedded, checksum-addressed vector resource if it has
cross-platform pixel and security evidence.

## Rejected alternatives

- Reuse the old config/data-directory search. It is mutable, machine-specific,
  and silently hides missing resources.
- Use a system font through Qt, Pango, or CoreText. Glyph selection and raster
  results would vary by installed fonts and platform.
- Treat the missing frozen `promo.svg` fixture as a successful no-op. Recipe
  state must not claim a visible watermark that was never rendered.
