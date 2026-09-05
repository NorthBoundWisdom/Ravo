# ADR-0157: AGPL RapidRAW tone-pipeline assimilation

- Status: Accepted
- Date: 2026-09-05
- Extends: [ADR-0096](0096-reference-algorithm-assimilation-boundary.md),
  [ADR-0134](0134-engine-qrhi-gpu-backend.md), and
  [ADR-0151](0151-iq-cpu-gpu-consistency-gate.md)

## Context

RapidRAW commit `d6d8daa999f81198fb49e99b7e8ff43b47a6ffcd`
demonstrates both the interaction latency and the photographic response sought
for Ravo. Its processing sources are AGPLv3, while Ravo was distributed under
GPLv3. Source-faithful reuse therefore needs an explicit whole-product licence
decision, exact provenance, and integration into Ravo's existing C++ ownership
rather than a second Rust/Tauri renderer.

RapidRAW's similarly named sliders are not numerically identical to Ravo's:
its GPU inputs normalize Exposure by 0.8, Contrast by 100, Highlights and
Shadows by 120, Whites by 30, and Blacks by 40 before evaluating the tone
functions. Its Basic RAW tone mapper also applies a fixed display response
after scene-linear controls. Copying formulas without these units would not
copy the response.

## Decision

- Ravo is distributed under the GNU Affero General Public License, version 3.
  Packages carry the root licence and tracked third-party notices.
- RapidRAW material is source-faithfully assimilated only from the pinned
  commit and paths recorded above. Adapted files identify the source, licence,
  copyright owner, and Ravo changes. RapidRAW is not a runtime dependency.
- Recipe continues to own versioned operation state, Engine owns CPU and QRhi
  pixel mathematics, Services own cache/cancellation/publication, and Desktop
  only presents state and intents. Rust/Tauri workers, wgpu device state, UI,
  masks, caches, and sidecars do not enter Ravo.
- Production uses two separately identified operations. The identity-present
  `ravo.core.rapidraw-tone-controls` v1 owns EV Shift, Exposure, Contrast,
  Highlights, Shadows, Whites, and Blacks in RapidRAW UI units and preserves
  the source normalization and evaluation order. `ravo.display.rapidraw-basic`
  v1 wraps the Basic RAW display-sRGB response back into linear sRGB so Ravo's
  output-profile owner performs the single final encoding. Existing explicit
  Sigmoid and first-tranche Basic-only recipes retain their exact meaning; no
  stored Recipe is silently reinterpreted.
- CPU remains the durable preview/export gold. The QRhi interactive pass uses
  the same constants and equations and is gated against CPU display RGB. A GPU
  initialization, dispatch, surface, or numerical failure remains structured;
  there is no silent alternative algorithm.
- New RAW Studio controls expose RapidRAW's `[-5,5]` EV Shift/Exposure and
  `[-100,100]` tonal units. Shadows/Blacks use the scale-aware 3.5-pixel
  Gaussian tonal reference, with RapidRAW's current-short-edge/1080 scale rather
  than Ravo's original-image density scale. CPU and Metal execute the same
  separable horizontal/vertical Gaussian; Metal materializes the horizontal
  plane before the tone pass so no dispatch reads adjacent pixels being written.
  Studio and history label `EV Shift` separately from filmic `Exposure`; stored
  `ev_shift` and `exposure` values are never reinterpreted or exchanged.

## Consequences

Ravo reuses RapidRAW's global Basic response while keeping one C++ service and
rendering architecture. New RAW baselines select the control and display pair,
while old Sigmoid-authored and Basic-only edits reopen unchanged. Network distribution and remote use
must follow AGPLv3 section 13; package and source publication must not describe
the product as GPL-only.

## Rejected alternatives

- Translating AGPL code into a GPL-only tree without notices or licence change.
- Changing the implementation behind `ravo.display.sigmoid` and silently
  altering stored recipes.
- Adding RapidRAW's Rust/Tauri/wgpu graph as a second product runtime.
- Porting all controls in one unversioned batch without per-stage goldens.
