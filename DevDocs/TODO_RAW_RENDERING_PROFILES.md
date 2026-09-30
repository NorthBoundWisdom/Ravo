# RAW rendering profile execution gates

Design authority: [ADR-0162](adr/0162-versioned-raw-rendering-profiles.md).
This is pending work, not an implemented Camera Matching or AgX capability.

- Expose decoder normalization and applicable camera/DNG exposure metadata in
  the Engine inspect/CLI contract; reject malformed values and test absence.
- Define versioned immutable calibration/rendering resource descriptors, hashes,
  licensing and size limits. Establish recipe expansion and parameter-stage
  ownership without changing existing operation semantics.
- Add baseline binding to the catalog with create/reopen/migrate/rollback and
  recovery tests. Preserve explicit old recipes and pin the prior implicit
  baseline before enabling any new import default.
- Implement Standard/AgX as a versioned candidate through CPU Engine and the
  supported CLI. Verify reference provenance, input/output space, single
  encoding, cancellation, peak memory and synthetic tone/gamut boundaries.
- Establish a reproducible RAW/JPEG corpus and inspect default brightness,
  highlight roll-off, neutral colour and skin before selecting a new Standard
  default. Private samples must not be committed without permission.
- Implement revision-checked profile selection/batch selection, history/undo,
  default-policy resolution reasons, missing-resource errors and cache identity.
- Add Studio profile selection and labelled quick/settled preview transitions
  over the shared command contract; validate headlessly and with QML smoke.
- Admit Camera Matching per camera/style only after independent corpus checks.
  Keep unsupported models/styles explicit and verify resource redistribution.
- Validate CPU export/preview/ROI and separately gate QRhi parity and supported
  Windows/Linux/macOS builds. Record unavailable hosts as untested.

Minimum existing checks to extend:

```text
cmake --build --preset mac_clang_debug
cmake --build --preset mac_clang_debug --target RavoCodeQuality
ctest --test-dir build/mac_clang_debug --output-on-failure
```

Acceptance requires versioned CLI/service tests for each new contract, source
hash preservation, old-recipe output preservation, and profile-aware cache
invalidation. Current tests alone cannot certify the unimplemented capability.
