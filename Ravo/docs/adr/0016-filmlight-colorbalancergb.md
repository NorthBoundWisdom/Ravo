# ADR-0016: Color Balance RGB keeps the Filmlight and perceptual gamut contract

- Status: Accepted
- Date: 2026-08-26
- Relates to: ADR-0006, ADR-0015, root `TODO_LEGACY_MIGRATION.md` C2
- Supersedes: ADR-0009's lift/gamma/gain approximation

## Context

The former `ravo.color.colorbalance` lift/gamma/gain control was an
unversioned approximation. It did not implement the frozen
`colorbalancergb.c` operation's D50/D65 conversion, Filmlight Yrg masks,
offset/slope/power grading, fulcrumed luminance controls or perceptual gamut
mapping. The distinct frozen `colorbalance.c` operation remains a later C10
migration and cannot justify keeping the approximation.

## Decision

- `ravo.color.colorbalancergb` v1 requires
  `working_space=linear_srgb_d50`, `algorithm=filmlight_ych_v5`, all 32 frozen
  numeric parameters and an explicit `saturation_formula`.
- The default formula is `dt_ucs_2022`; `jzazbz_2021` remains an explicit
  supported historical formula. Unknown fields/modes, non-finite or
  out-of-range values, zero grey fulcrums and singular midtone power fail
  before pixel execution.
- The C++20 CPU owner preserves working RGB D50 → CAT16 D65 → CIE 2006 LMS →
  Filmlight Yrg/Ych, the three internal luminance opacities, hue/chroma,
  grading-RGB offset/slope/power, luminance power/contrast and the selected
  perceptual saturation/brilliance path.
- Each synchronous operation derives an immutable 512-element gamut boundary
  LUT for its explicit working space. DT UCS uses the frozen primary-boundary
  construction and soft clip; JzAzBz uses the frozen 92³ sampler and negative
  LMS chroma clip.
- Pixel output is accumulated separately and published only after every row
  succeeds. Cancellation, allocation failure and non-finite input/output are
  structured failures and cannot expose a partially processed image.
- Studio exposes the complete schema through one read-only presenter map; QML
  only forwards numeric intents. CLI, preview and export execute the same
  recipe/engine operation.
- The simplified `ravo.color.colorbalance` operation and Lift/Color
  gamma/Gain controls are removed as a hard cut. No compatibility shim is
  retained. Frozen `colorbalance.c` remains queued independently.
- Checkerboard/mask preview, GTK pickers, presets, OpenCL, the dynamic IOP ABI
  and legacy XMP binary ABI are not migrated.

## Consequences

- The statically decoded `0083-colorbalancergb` schema-v4 and
  `0093-colorbalancergb-ucs` schema-v5 blobs are Ravo-owned test inputs.
  Synthetic math tests, a `mire1.cr2` channel-sum reference, catalog reopen and
  QML smoke form the acceptance evidence without running the old application.
- The module's three luminance masks remain internal operation math; they do
  not create or delay the canonical drawn/parametric mask graph.
- `legacy/src/iop/colorbalancergb.c` and its CMake registration are retired
  after the complete automated gate passed. Shared color-science owners remain
  until their other consumers are migrated.

## Rejected alternatives

- Keeping lift/gamma/gain under a similar product name: it creates two
  incompatible meanings and cannot satisfy the frozen CPU contract.
- Replacing Filmlight/DT UCS with HSL or ASC CDL: both omit required scene and
  gamut behavior.
- Caching legacy ICC or global module pointers: derived state must be owned by
  the operation render lifetime.
