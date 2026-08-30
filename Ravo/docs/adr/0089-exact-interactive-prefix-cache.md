# ADR-0089: Exact interactive prefix cache and owner-scoped row team

- Status: Accepted
- Date: 2026-08-30
- Extends: [ADR-0047](0047-first-frame-raw-cache-lifecycle.md),
  [ADR-0087](0087-progressive-develop-preview.md)

## Context

The 960px progressive preview removed decode, RAW preprocessing, PNG encoding,
and settlement from a slider update, but a complex recipe still reran every RGB
operation. In particular, changing Exposure recomputed unchanged calibration,
profile denoise, and lens/canvas work before reaching the changed light stage.
Each row-parallel operation also created and joined another temporary worker
set. The result remained sensitive to recipe complexity even though the source
generation and the operation prefix were unchanged.

Filmulator and ART provide static reference evidence for stage invalidation and
retained processing resources. Ravo still requires its own explicit ownership,
cancellation, colour, and error contracts; those projects are not runtime
dependencies or pixel oracles.

## Decision

- Each foreground 960px `CachedLinearWorking` owns at most one exact RGB prefix
  buffer. The prefix contains the canonical source/input, Primaries,
  calibration, RAW/detail denoise, and lens/canvas stages which precede live
  light controls. The remainder always executes the complete current recipe.
- Engine derives the prefix boundary and fingerprints the serialized prefix.
  A hit therefore requires the same ordered operation instances, parameters,
  enabled state, masks, and asset recipe identity. CatalogService additionally
  binds the cache to the existing asset/source fingerprint, dimensions, and
  preprocess key by storing it inside that working generation.
- A changed prefix is built in separate owned state. Cancellation, validation,
  allocation, or worker-start failure retains the prior cache and returns the
  structured error. Publication occurs only after the new prefix completes.
  A source/preprocess change, slot replacement, catalog close, or destruction
  destroys the prefix with its working buffer.
- The same cache owns one reusable CPU row team for its serialized foreground
  caller. It uses static contiguous partitions, caller participation, and at
  most 16 workers. Destruction joins every worker. Other synchronous renders
  use one render-scoped team; there is no process-global executor or detached
  thread.
- Independent Vibrance/Saturation and Vignette rows, and production Sharpen
  RGB/Lab conversion rows, use that row contract. Sharpen's controlled test
  path retains serial checkpoint order. Pixel arithmetic and per-pixel output
  are unchanged.
- Studio posts the foreground pixel job before broadcasting the broad
  `editChanged` notification, allowing QML binding refresh to overlap the C++
  work. Cancellation and revision acceptance remain the publication gate.
- Release validation uses a private catalog copy. The opt-in service probe and
  Presenter intent-to-owned-image probe enforce a 30ms P90 gate after the
  Develop warm-up, in addition to exact cached/uncached pixel equality and
  cancellation tests.

## Consequences

Exposure and other post-prefix controls no longer pay for unchanged denoise or
calibration. Preview pixels, profiles, masks, resolution, and settled/export
paths remain exact. The foreground 960px slot now retains one additional float
RGB image plus a bounded worker team; the 1600px settled and Gallery browse
lanes do not acquire that state. Prefix edits correctly rebuild once and then
become the next reusable generation.

The latency gate is a host-local Release performance contract, not a claim that
an overloaded system or display refresh interval can never produce a longer
wall-clock frame. Normal tests remain deterministic and skip the opt-in timing
fixture when its environment is absent.

## Rejected alternatives

- Reduce interactive resolution or skip expensive effects. Either creates a
  visibly different recipe and a correction jump on settlement.
- Cache post-Exposure pixels. They depend on the live value and cannot be
  reused for an exact brightness update.
- Add a process-global pool. Its lifetime, concurrency, and shutdown would no
  longer match the one selected-photo working generation.
- Introduce a GPU path solely for this latency target. GPU ownership remains
  gated by the separate CPU-gold and platform requirements.
