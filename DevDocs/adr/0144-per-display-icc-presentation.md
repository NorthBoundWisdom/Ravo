# ADR-0144: Per-display ICC presentation without recipe mutation

- Status: Accepted
- Date: 2026-09-04
- Relates: DISPLAY-01 in [TODO.md](../TODO.md), colour-critical grading in
  [ProductRoadmap.md](../ProductRoadmap.md)
- Extends: [ADR-0022](0022-final-display-packing-and-diagnostic-disposition.md)
  (final packing remains recipe-owned output encoding; this ADR adds on-screen
  monitor conversion only)

## Context

Ravo already owns input/output ICC transforms and soft-proof state on
`ravo.color.output`. On-screen monitor conversion must be a separate
**presentation** contract: moving Studio between displays, refreshing OS
monitor profiles, or injecting a test profile must never mutate recipe JSON,
history, catalog revision, settled preview authority, or export profile bytes.

## Decision

### Presentation-only ICC

Monitor ICC is presentation-only. Discovering, refreshing, injecting, or
falling back a monitor profile must not write DevelopParams, Recipe,
history entries, catalog revision, settled preview cache keys that encode
edit authority, or export `output_profile` / embedded ICC selection.

### Owner of the final preview→monitor transform

A C++ display-presentation owner (service contract + engine/CPU transform path)
owns the final preview→monitor transform. QML only presents the resulting
pixels (or a native surface already converted by C++). Soft-proof and gamut
warning remain inspectable `ravo.color.output` recipe state; the display
transform applies **after** soft-proof for on-screen pixels only.

### First Ready tranche (this ADR)

1. **macOS monitor profile discovery + change lifecycle** via CoreGraphics
   display colour space → ICC bytes, keyed by a stable screen token. Window
   move between screens refreshes presentation profile without recipe change.
2. **Explicit fallback when missing/corrupt:** **sRGB** (`fallback_srgb`),
   never a silent assumed transform. Machine-visible state records
   `source`, `reason`, fingerprint, and screen token.
3. **Synthetic matrix and LUT test profile paths** for CPU reference compare
   (injectable; headless/tests must inject rather than assume OS profiles).
4. **Headless/tests:** injectable ICC path or synthetic profile; never silent
   assumed transform without a machine-visible state.

Windows/Linux discovery, HDR policy, GPU presentation path parity, and Studio
chrome wiring beyond a thin CLI/status hook remain residual.

### Soft-proof ordering

`proof_mode` / proof profile stay on the recipe. Display presentation consumes
already soft-proofed (or output-profiled) RGB8 and converts to the active
monitor profile for on-screen use only. Export and CLI render publication
bypass display presentation.

## Non-goals (explicit)

- Mutating recipe or export colour when the window moves.
- QML-owned ICC math or monitor profile parsing.
- Claiming Windows/Linux discovery or HDR in this tranche.
- Replacing soft-proof with monitor conversion.

## Consequences

DISPLAY-01 gains a dated presentation contract and a first Ready owner that
tests prove recipe/export unchanged across presentation refreshes. Extending
OS discovery or GPU paths requires residual work under the same ADR rules (or a
superseding ADR for HDR).

## Rejected alternatives

- Identity fallback when no profile exists (leaves output-space pixels on an
  unknown display without a declared presentation target).
- Folding monitor ICC into `ravo.color.output` or export options.
- Silent sRGB assumption without `source=fallback_srgb` machine state.
