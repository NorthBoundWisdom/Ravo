# ADR-0084: First-class grading curves

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0082](0082-studio-develop-grading-workspace.md),
  [ADR-0052](0052-legacy-rgbcurve-contract.md),
  [ADR-0053](0053-rgbcurve-middle-grey-uncompensate.md),
  [ADR-0009](0009-p1-develop-recipe.md)

## Context

Everyday stills grading needs a real curve tool: RGB and per-channel point
curves, a histogram behind the plot, interpolation choice, and Lightroom-style
region sliders. Studio only authored one linked RGB-luma point list on
`ravo.core.tonecurve`. `ravo.color.rgbcurve` already evaluated linked and
independent working-RGB curves but had no editor, and both operations rejected
Catmull-Rom and cubic interpolators even though leftover `curve_tools.c`
implements them (CUBIC_SPLINE=0, CATMULL_ROM=1, MONOTONE_HERMITE=2, max 20
nodes). Folding rgbcurve into tonecurve remains rejected by ADR-0052.

## Decision

- Studio's default Develop order is White Balance, Light, **Curves**, Color
  Equalizer, Color, then the existing later stack. Curves is its own recipe
  section (`curves`) with an independent bypass lamp. Paste Light applies
  White Balance, Light, and Curves.
- The editor authors two existing operations without merging schemas:
  - **RGB** (default): `ravo.color.rgbcurve`. Channel RGB (linked) or R/G/B
    (independent). Preserve-colors applies only when linked. Compensate
    middle grey remains the ADR-0053 flag, under Curves · more.
  - **Tone**: `ravo.core.tonecurve`. Working space RGB-linked, Lab, XYZ, or
    Lab independent (L/a/b). Preserve-colors applies to RGB-linked Tone.
- Interpolation is `monotone_hermite` (default), `catmull_rom` (leftover
  centripetal spline), or `cubic_spline` (leftover natural cubic). Recipe
  evaluates the spline; engine LUTs and Studio samples share that evaluator.
  Leftover rgbcurve v1 maps per-payload `curve_type` when every used channel
  agrees; mixed types fail closed.
- Linked RGB gains a parametric map (Shadows/Darks/Lights/Highlights plus
  three splits). Identity amounts omit the map. Composition is
  `point_curve(parametric(x))`. Independent R/G/B ignore parametric.
- QML draws the plot, histogram, and nodes. C++ owns points, interpolation,
  parametric values, and commits. Histogram bins come from the existing
  engine-owned display RGB8 histogram plus Rec.709 luma; QML does not bin
  pixels. Node count is 2–20, matching leftover `MAX_ANCHORS`.
- CLI `--set` exposes interpolation, preserve, mode, compensate, and
  parametric numbers. Point lists stay recipe JSON / Studio commands.

## Consequences

The default grading path has a Lightroom-class RGB curve without a new
operation ID or catalog schema. Filmic, AgX, 3D LUT, and basecurve stay
queued leftovers. The leftover rgbcurve IOP remains until the freeze census
is zero.

## Rejected alternatives

- Folding rgbcurve into `ravo.core.tonecurve`. Payloads, working space, and
  linked-channel behaviour still differ.
- Computing histogram bins in QML. ADR-0061 keeps pixel statistics in the
  engine.
- A new recipe schema version for optional curve fields. Additive optional
  parameters on schema v1 keep existing recipes valid.
- Porting leftover GTK curve zoom, picker-driven node sets, or per-channel
  mixed interpolators.
