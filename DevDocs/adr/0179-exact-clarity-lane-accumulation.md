# ADR-0179: Exact clarity lane accumulation

- Status: Accepted
- Date: 2026-10-11
- Extends: [ADR-0173](0173-exact-cpu-spatial-processing.md)

## Decision

Engine's clarity lightness blur retains two radius-eight horizontal/vertical
passes, clamped edges, division by 17 and the existing Lab conversion/mix order.
Horizontal interior pixels accumulate eight independent lanes; borders retain
the scalar clamped evaluator. Vertical rows accumulate 32 independent lanes
from precomputed clamped row addresses and publish each sum once. Scalar tails
retain the same neighbourhood and addition order as every vector lane.

Only iteration across independent pixels changes. Each pixel still adds all
17 samples from negative to positive offset using float arithmetic; no running
sum, reassociation, approximate Gaussian or quality mode is introduced. Existing
render-owned row tasks and their joins/cancellation remain the sole executor.
The five full-size scratch planes are unchanged; lane sums and 17 row addresses
are bounded stack locals. No retained cache, dependency, schema or fallback.

## Validation

Compare final RGB float bits to the independent preceding three-channel scalar
oracle over sub-radius images, each lane/tail boundary, odd/portrait dimensions,
signed/HDR values and positive/negative amounts. Preserve cancellation and
scratch-preflight tests. Compare real RAW CLI probe pixels/profile/source and
recipe/history to the preceding binary. Run Release parameter/burst comparisons
with identical dimensions, backend and ICC; preserve all samples. Build and run
full Ravo contracts on the available host, reporting other platforms untested.
