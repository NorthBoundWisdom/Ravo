# ADR-0174: Bounded exact profile-denoise basis for interactive mixing

- Status: Accepted
- Date: 2026-10-11
- Extends: [ADR-0173](0173-exact-cpu-spatial-processing.md)

## Context

The numeric-control matrix identifies profile denoise as a slow interactive
prefix. Its wavelet/MAD estimation, thresholding and reconstruction depend on
the input, radius, shadows, bias and noise model, but not on strength or chroma.
Those two controls participate only in the final luma/chroma delta mix.
The current public recipe schema exposes strength/chroma/radius only; internal
noise-model controls remain private, and unknown recipe fields still reject.
LightCraft's dependency-keyed stages motivate separating true dependencies;
its image algorithms and tolerance policy are not adopted.

## Decision

The existing Engine `InteractivePreviewRenderCache` may own one private denoise
basis: the immutable input immediately before an unmasked profile-denoise
operation and its unblended reconstructed RGB candidate. Admission requires
that denoise be the last enabled, non-bypassed operation in the existing cached
prefix. Other recipes keep their ordinary evaluation path. Operation order,
all wavelet/noise mathematics, reconstruction and mix rounding stay unchanged.
Only strength/chroma are normalized in the serialized prefix key; every other
operation field, mask graph and asset identity remains in it. Working-source
identity follows the existing contract: callers serialize cache access and
destroy/recreate it whenever the immutable working generation changes.

Retained float payload is capped at 64 MiB per cache, excluding existing bounded
recipe/profile/analysis metadata. At most one input and candidate are retained.
A true dependency change evicts the old basis before building its replacement;
there is no double-basis replacement peak. Cold capture adds one candidate RGB
plane and one original-input RGB plane to ordinary denoise scratch. Service
foreground working slots retain their existing bounded ownership (at most
128 MiB extra float payload for two slots). Profile and serialized-key capacities
are each capped at 1 MiB. No persistent
export/preview cache or global state is added.

The renderer uses its existing synchronous owned row executor. Allocation and
validation happen on the caller; workers write disjoint rows and join before
publication. A complete finite candidate is published only after cancellation
checks. Failure leaves the preceding accepted prefix/frame intact, although an
evicted basis may need rebuilding on Undo. Cache hits use the same mix helper
and return independently owned pixels. No detached tasks, new pool, public
machine schema, GPU algorithm, silent fallback or quality reduction is added.

## Validation

Require float-bit equality of captured/reused candidates and ordinary denoise
for strength/chroma sweeps, radius/noise changes, signed/HDR input, tiny frames,
odd portrait geometry and canonical scale. Cover payload admission/eviction,
invalid input/parameters, cancellation, move/destruction and absent-basis errors.
Engine tests compare cached/ordinary output through real recipe dispatch and
exclude masked/nonterminal recipes. Retain existing denoise goldens, GPU bounds
and full unit/contract tests. Release A/B measurements must use real parameter
changes, the same source/monitor/size and retained source/recipe/history hashes.
Windows/Linux require separate build evidence; cross-host budgets remain open.
