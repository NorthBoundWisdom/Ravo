# ADR-0048: Leftover flip orientation maps to canonical rotate then flip

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0047](0047-first-frame-raw-cache-lifecycle.md)

## Context

P2 starts with everyday orientation/crop. Studio already authors
`rotate_quarters` and independent horizontal/vertical flips, and it transforms
the crop rectangle with those controls. Leftover `iop/flip.c` stores a single
`dt_image_orientation_t` bitfield (`ORIENTATION_NULL` through `TRANSVERSE`) and
applies it with `dt_imageio_flip_buffers`. The strict XMP importer treated only
the auto blob `ffffffff` as a builtin nop, so a real leftover orientation never
became a Ravo recipe.

Ravo applies camera EXIF at decode. Leftover `ORIENTATION_NULL` means “use
camera orientation at process time.” Those two defaults must not stack.

## Decision

- Leftover flip v2 is a dedicated importer, not a builtin RAW nop. Version 2,
  enabled, singleton, and the frozen unmasked default blends (`gz11` and the
  `gz14` twin already accepted for colorin) are required. Masks and other
  attributes reject.
- `ORIENTATION_NULL` (-1) and `ORIENTATION_NONE` (0) import as identity. Ravo
  does not undo decode-time EXIF.
- Bits 1–7 map onto canonical rotate-then-flip matching leftover
  `SWAP_XY`/`FLIP_X`/`FLIP_Y` composition:
  1 vertical flip, 2 horizontal flip, 3 180°, 4 transpose = 90° CW + horizontal
  flip, 5 90° CW, 6 90° CCW, 7 transverse = 90° CW + vertical flip.
- Other orientation integers reject with `unsupported_legacy_flip_orientation`.
- This does not retire leftover `flip.c` or treat 3×3/keystone as complete G1
  ALG. Leftover crop boxes are ADR-0049.

## Consequences

`0000-nop` and other auto-orientation histories stay two-operation recipes.
User-rotated leftover XMPs with evidenced blends become `ravo.geometry.rotate`
and/or `ravo.geometry.flip`. Pixel tests pin the eight leftover flags against
the canonical ops.

## Rejected alternatives

- Keeping flip as an exact-blob builtin nop, which cannot represent 90°/mirror
  histories.
- Importing leftover NONE as an EXIF undo. Decode-time orientation is the Ravo
  contract; undoing it needs an explicit later product decision.
- Encoding leftover bits as a third geometry operation instead of the existing
  rotate-then-flip recipe.
