# ADR-0187: Dehaze for declared raster sources

- Status: Accepted
- Date: 2026-10-12
- Scope: user-requested real-library editing reliability
- Extends: [ADR-0057](0057-source-linear-dark-channel-dehaze.md)

## Decision

JPEG, PNG, HEIC and other accepted RGB8 raster sources may use the existing
dehaze operation after profile-gamma and Input Color have converted their
declared source profile into linear Rec.709 or Rec.2020 RGB. Encoded samples
never enter the atmospheric algorithm directly. These images already contain
camera/display rendering; inverse transfer and colour conversion do not
recover a sensor scene or make raster results equivalent to RAW results.
RAW retains its camera-RGB stage before Input Color and its export mathematics.

The Engine owns this raster preparation and invokes the existing dark-channel
and guided-filter owner once. The service's existing preprocess key includes
all dehaze parameters for both source families. Preview and direct raster
export disable the consumed operation before subsequent recipe dispatch;
ordinary raster white balance and light adjustments remain active. No recipe
schema, database format, thread, cache owner, dependency or fallback is added.

Persistent CPU previews prepare at their planned output dimensions. The larger
interactive basis remains admitted for responsiveness, with a distinct working
cache identity and no reuse by settled output. Preview artifact contract v13
invalidates earlier persisted results which prepared at 1600 then scaled to
960, including RAW results. This fixes same-size settled/export comparison
without changing RAW export mathematics or asserting cross-scale interactive
pixel equality.

Declared-profile conversion, dimensions, canonical scale, buffer validation,
allocation failures and cancellation retain their structured errors. Missing
profiles require an explicit user profile choice. Nonlinear direct dispatch,
unsupported working profiles and unaccepted mask state remain errors.

## Validation

Synthetic tests compare the raster preparation with independently sequenced
Input Color and the existing dehaze owner in both accepted working profiles,
verify scaled geometry, active downstream exposure and exactly-once export,
and retain missing-profile, nonlinear-input, cancellation and original-byte
contracts. Existing independent dehaze oracle and RAW fixture tests remain
unchanged. Real-library read-only probes and isolated-catalog
edit/save/reopen/export cover the service/CLI path. macOS validation does not
qualify Windows/Linux or full photographic quality.
