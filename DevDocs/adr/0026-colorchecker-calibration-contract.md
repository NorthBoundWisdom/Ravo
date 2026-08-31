# ADR-0026: Preserve Color Checker as an explicit D50 Lab calibration fit

- Status: Accepted
- Date: 2026-08-27

## Context

The frozen `legacy/src/iop/colorchecker.c` is a patch-calibration operation,
not a three-channel matrix control. Its declared input and output are D50 Lab;
it fits one mapping for each Lab output channel from ordered measured/target
patch pairs and evaluates that fit for every pixel. Ravo privately bridges its
explicit linear-Rec709 working buffer through D50 Lab around this owner. Its
special
solutions for zero through four patches and thin-plate radial-basis solution
for larger sets depend on the frozen fast-log approximation, Gaussian solver,
float promotion and accumulation order, and non-uniform singular fallback.

Existing `ravo.color.channelmixerrgb`, `ravo.color.primaries`, and
`ravo.color.colorbalancergb` own different schema, mathematics, and cache
identity. None can represent the ordered patch fit without losing behavior.
An explicitly present default Color Checker also still executes the canonical
RGB/Lab bridge and frozen fit, while the old module is disabled by default;
Ravo therefore must distinguish absence from present-default state.

Across all 158 frozen XMP documents there is exactly one Color Checker record,
in 0098. It is enabled parameter version 2 with 24 active patches, priority
zero, empty instance name, blend version 11, and exact default-unmasked blend
state. The containing history also includes an unrelated earlier unsupported
operation, so the whole document is negative compatibility evidence. Parameter
version 1 exists in repository history but has no positive frozen XMP fixture.

## Decision

- `ravo.color.colorchecker` schema v1 is independent from other colour
  operations. It declares `working_space=lab_d50`,
  `algorithm=thin_plate_rbf_v2`, and an ordered array of zero through 49
  patches, each containing exactly one finite, float-representable source Lab
  triple and target Lab triple. Every patch, component, order position, enabled
  state, and explicit presence participates in serialization, equality, reset,
  and cache identity. Whole-operation reset removes presence; patch/component
  editing enables the operation.
- Canonical Develop order places Color Checker after the accepted preceding
  input/profile, primaries/calibration, exposure, tone-equalizer, and graduated
  stages and before later colour grading. History `num` does not define this
  processing order.
- The engine requires the explicit linear-Rec709 RGB working profile and owns a
  private RGB↔XYZ D50↔Lab bridge. A zero-patch fit retains the Lab identity
  polynomial; one patch scales each matching component; two and three patches
  solve their frozen reduced per-output systems; four patches solve one shared
  affine matrix; and larger sets solve the shared `(N+4)` thin-plate RBF matrix.
  Pixel evaluation forms `constant + (L term + a term + b term)` in that order,
  then accumulates RBF terms in patch order.
- The kernel reproduces the frozen bit-level `fastlog2`/`fastlog` mantissa mask
  and constants rather than using libm. Gaussian storage, pivoting, division,
  back-substitution, the N=3 float sum before double promotion, coefficient
  layout, and float stores are source-exact. Singular N=2/N=3 solving preserves
  already-computed earlier output channels and leaves the failed and later
  channels at identity; singular N=4 or N>4 leaves every channel at identity.
- Fit construction, patch/source copies, coefficients, Gaussian matrices,
  pivots, right-hand scratch, and owned output participate in the render memory
  contract. Dimensions, buffer length, profile/model, patch count, components,
  one-patch denominators, kernel/fit/result finiteness, allocation, and
  cancellation fail structurally before publication. Cancellation is checked
  before fitting, while constructing a large fit, before output allocation, and
  by image row. The source pixels, profile, and immutable RAW exposure-analysis
  snapshot remain unchanged; success publishes owned output.
- Eight built-in presets retain their frozen float bits: IT8 skin tones,
  Expanded Color Checker, Helmholtz/Kohlrausch monochrome, Fuji Astia, Fuji
  Classic Chrome, Fuji Monochrome, Fuji Provia, and Fuji Velvia. Presets and
  direct source/target Lab patch editing are Studio intents; fitting remains in
  the engine.
- The strict legacy decoder accepts exactly one enabled (`1`) singleton with
  the evidenced default-unmasked presentation. Version 2 validates signed
  active count 0–49 and only active ordered planes; inactive tail planes are
  ignored because frozen patch removal decrements the count without clearing
  the prior final slot, including when those stale bits decode as non-finite.
  A synthetic version-1 payload upgrades its 24 targets using the adapter-
  private historical source table and is labelled synthetic-only. Unsupported
  versions, disabled state, duplicates, names, priorities, hand-edited state,
  masks, custom blend, malformed length/count, and non-finite active data return
  stable structured errors. A minimal document with the verbatim 0098 record is
  the positive v2 fixture; the full 0098 history stays unsupported.
- The general mask graph does not expand in this migration. The GTK live chart,
  color picker, add/remove interaction, and display helpers are presentation or
  analysis UI, not serialized CPU mathematics, and are not recreated as hidden
  engine state. Shared `common/colorchecker.h`, Gaussian helpers,
  `extended.cl`, program/order/registry strings, and calibration resources keep
  their remaining D0.3/D0.4/S1/S4 owners and are not deleted with the exclusive
  module.

## Consequences

Recipe/Develop round trips, the operation descriptor, CLI render, Catalog
preview/save/reopen/export, Studio preset/patch bindings and localization all
consume one canonical operation and engine implementation. Independent
scalar/Gaussian tests plus fixed goldens distinguish the frozen fast-log,
matrix orientation, float promotion, accumulation order, all patch-count modes,
and singular policies from plausible substitutes. The verbatim 0098 patch set
also has a pinned RAW render reference whose source hash, size, and modification
time remain unchanged.

This accepted schema, CPU, importer, preset, consumer, resource, error, and
presentation boundary permits the exact old `iop/colorchecker.c` source and
CMake registration to retire atomically. It does not claim that the complete
0098 history, masked edits, picker analysis, or GPU execution is supported.

## Rejected alternatives

- Approximate patch calibration with Channel Mixer, Primaries, Color Balance,
  or a three-parameter control: none preserves the nonlinear ordered fit.
- Treat a present default or zero-patch operation as absent: the private
  RGB/Lab round trip is observable and serialized presence must survive reopen.
- Replace the frozen fast-log/Gaussian path with libm or a modern solver: fixed
  kernel and fit goldens show different results.
- Reject nonzero inactive v2 tail storage: legitimate frozen patch removal can
  leave stale data that commit and processing ignore.
- Treat the full 0098 history as a positive import fixture, infer operation
  order from history position, or silently drop masks/blend/multi state: each
  would claim semantics that the canonical graph does not own.
- Port the GTK picker/chart or OpenCL path as part of C11: those require
  separately owned presentation, analysis, or backend contracts.
