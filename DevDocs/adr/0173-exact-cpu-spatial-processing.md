# ADR-0173: Exact CPU spatial processing with contiguous lanes and owned row jobs

- Status: Accepted
- Date: 2026-10-11
- Extends: [ADR-0057](0057-source-linear-dark-channel-dehaze.md)

## Context

The interactive numeric-control matrix identifies dehaze and clarity as slow
CPU stages. Dehaze's compensated box means traverse interleaved statistics one
column/channel at a time. Clarity blurs three identical copies of lightness and
performs its independent Lab conversions serially. Perspective's
Lanczos sampler repeats horizontal weight evaluation for every vertical tap,
and its safe-crop search recomputes intersections for each ordinate pair.
LightCraft's contiguous spatial passes are a useful data-layout reference;
its blur approximation and numerical tolerances
are not Ravo's algorithm contract.

## Decision

Engine may batch independent dehaze box-mean lanes into contiguous loads while
retaining each lane's original Kahan recurrence, sample order, division and
boundary population. Tile geometry, overlap, covariance solve and atmospheric
equation remain unchanged. Scratch ownership and memory preflight are unchanged;
lane accumulators use bounded stack storage. No parallel dehaze callback or tile
execution is introduced.

Clarity may store and blur one lightness plane instead of three identical RGB
planes. It retains the original two radius-eight horizontal/vertical passes,
clamped borders, float addition order and Lab equations. Ordinary running sums
or Gaussian approximations are not admitted. Lab conversion, blur and mixing
use the existing Engine row executor. RAW preflight accounts for the five
float planes (Lab, lightness and pass scratch); the preceding thirteen-plane
implementation had no clarity-specific accounting. Render/cache owners still
own and join that executor; there is no new
pool, detached thread, asynchronous pixel lease or global cache.

All buffers are allocated by the calling render before a row job. Jobs borrow
them synchronously, write disjoint rows and finish before their next pass or
return. Workers perform no allocation or throwing callback. Cancellation is
checked at row boundaries and between passes; executor startup/allocation and
cancellation errors propagate through the existing Result boundary. Failed
requests publish no partial image.

Perspective sampling may evaluate each horizontal Lanczos weight once per
output pixel and reuse that exact double for each vertical tap. Its safe-crop
grid may evaluate each ordinate's intersection once per layout. Pair order,
strict tie selection, refinement, clipping, sample bounds, multiplication and
accumulation order remain unchanged. These arrays are bounded call-local stack
storage; no geometry cache, interpolation approximation or new thread is added.

## Validation and limits

Compare dehaze float bytes against the preceding implementation over short,
odd, portrait and tile-boundary dimensions, positive/negative strengths and HDR
values. Keep the independent oracle, frozen RAW and cancellation tests. Clarity
must equal the preceding serial RGB blur reference exactly, including borders,
signed amounts and HDR samples. Perspective float pixels and layout fields must
equal the preceding implementation; retain all interpolation goldens and compare
Lanczos output to an independent unhoisted evaluator. Exercise cancellation,
then run the full unit/contract suite and the Release numeric-control matrix.
Same-source end-to-end P90 must improve without changing size, ICC, recipe order
or pixel admission bounds. Windows/Linux require their own build evidence.

No GPU algorithm, fallback, quality mode, persisted recipe/schema, public API or
dependency changes are admitted. RAW preparation and other spatial stages remain
separate measured optimization work.
