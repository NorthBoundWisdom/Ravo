# ADR-0175: Exact RAW preparation work reduction

- Status: Accepted
- Date: 2026-10-11
- Extends: [ADR-0173](0173-exact-cpu-spatial-processing.md)

## Context and decision

White-balance changes invalidate RAW preparation. Stage measurements identify
opposed highlight reconstruction as its dominant CPU owner. Its final scan reads
previously updated neighbours, so ordinary row parallelism or an immutable-copy
rewrite would change the accepted algorithm.

Engine keeps the reconstruction and chrominance reduction in source order.
The private CFA lookup may specialize validated 2×2 Bayer and 6×6 X-Trans
periods using exact integer arithmetic; other periods retain the generic lookup.
The opposed reference average may omit the current channel's cube root because
only the other two channel means participate in its result. Sum order, clamped
neighbourhood bounds, rounding, clip tests and write order remain unchanged.

The existing caller owns the float scratch and prepared RAW copy. Opposed
processing gains cancellation checks at row boundaries in mask construction,
dilation, chrominance reduction and reconstruction. It returns a structured
failure before float-to-RAW publication; the enclosing Engine request discards
its owned preparation on any error. No new executor, cache, dependency, schema,
GPU algorithm, fallback or downsample-before-reconstruction route is introduced.

## Validation

Compare specialized CFA lookup to generic modulo over Bayer permutations,
X-Trans periods and boundary coordinates. Compare reference-average float bits
to an independent original three-root evaluator over signed/HDR, edges and
short dimensions. Retain RAW/highlight fixture and cancellation tests. Real RAW
CLI probe RGB/profile hashes must equal the preceding binary, with unchanged
source/recipe/history. Release stage and complete white-balance sweeps must
improve on the same input; full unit/contract and source-size checks apply.
Windows/Linux require separate build evidence.
