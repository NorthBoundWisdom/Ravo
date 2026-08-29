# ADR-0082: Studio Develop default grading workspace

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0078](0078-copy-paste-develop-edits.md)
- Relates to: [ADR-0003](0003-versioned-machine-contract.md)

## Context

P1/P2 everyday Develop algorithms were already accepted. The Edit pane still
presented them as a module dump: White Balance after Light, Color Equalizer
inside Graduated ND, Color Balance RGB as a long numeric list, and overlapping
Lab tools on the default path. Photographers needed a grading workflow, not
another leftover IOP.

## Decision

- Studio's default Develop order is White Balance, Light, Color Equalizer,
  Color (Vibrance/Saturation/Velvia, Color Balance RGB wheels, Split Toning,
  Monochrome), then Geometry, Tone Equalizer, Graduated ND, Detail, Effects,
  RAW, and profiles.
- Color Zones, legacy Color Balance, Color Correction, Color Contrast, Color
  Harmonizer, Color Reconstruction, and the ColorChecker LUT stay available
  under **Color · Advanced**. Color Balance RGB formula, global, and extra
  numeric fields stay under **Color Balance RGB · more**.
- Color Equalizer is its own recipe section (`colorEqualizer`). Graduated ND
  no longer owns equalizer bands or their bypass flag. Recipe compile writes
  `ravo.color.colorequal` enabled state from `color_eq_effect_enabled`.
- Color Balance RGB shadows/midtones/highlights hue+chroma are authored with
  three presentation wheels plus luminance; values remain the existing
  `colorBalance*` fields. QML does not own recipe math. A wheel drag commits
  hue and chroma together through `studio.edit.set_numbers`.
- Copy Edits remains a complete `DevelopParams` clipboard (ADR-0078). Paste
  All still replaces the whole recipe, including masks. **Paste Light** applies
  White Balance plus Light. **Paste Color** applies Color plus Color Equalizer
  parameters and leaves destination mask attachments unchanged. Unknown
  sections fail closed.

## Consequences

The default path matches a common stills grading sequence without adding
operations or changing catalog schema. P4 leftover looks (Velvia frozen math,
Filmic, AgX, LUT) stay queued. Kelvin/tint as stored schema remains later.
ADR-0083 adds a Bayer CFA neutral pick that writes manual coefficients.

## Rejected alternatives

- Continuing P4 leftover consumption before the workspace existed. That would
  add optional looks without making the existing tools usable.
- Partial IOP pickers at copy time. The clipboard stays complete; only paste
  can apply a named grade group.
- A new recipe schema version for panel order. Order is presentation; section
  reset/bypass remain recipe-owned field groups.
