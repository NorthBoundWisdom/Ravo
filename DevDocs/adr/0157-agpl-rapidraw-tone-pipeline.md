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
- The first production tranche is a separately identified Basic RAW tone
  mapper. It wraps RapidRAW's display-sRGB response back into linear sRGB so
  Ravo's existing output-profile owner performs the single final encoding.
  Existing explicit `ravo.display.sigmoid` recipes retain their exact meaning;
  no stored Recipe is silently reinterpreted.
- CPU remains the durable preview/export gold. The QRhi interactive pass uses
  the same constants and equations and is gated against CPU display RGB. A GPU
  initialization, dispatch, surface, or numerical failure remains structured;
  there is no silent alternative algorithm.
- Later Exposure/Brightness and Highlights/Shadows/Whites/Blacks tranches must
  preserve RapidRAW's units and operation order, add explicit Recipe versions,
  and pass real-RAW response, monotonicity, cancellation, memory, and GPU/CPU
  consistency gates before replacing another Ravo default.

## Consequences

Ravo can reuse RapidRAW's exact response while keeping one C++ service and
rendering architecture. New RAW baselines may select the new mapper, while old
Sigmoid-authored edits reopen unchanged. Network distribution and remote use
must follow AGPLv3 section 13; package and source publication must not describe
the product as GPL-only.

## Rejected alternatives

- Translating AGPL code into a GPL-only tree without notices or licence change.
- Changing the implementation behind `ravo.display.sigmoid` and silently
  altering stored recipes.
- Adding RapidRAW's Rust/Tauri/wgpu graph as a second product runtime.
- Porting all controls in one unversioned batch without per-stage goldens.
