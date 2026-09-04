# ADR-0153: SPECIALIZE-01 tethered-studio deferred P2 (fail-closed stub)

- Status: Accepted
- Date: 2026-09-04
- Relates: SPECIALIZE-01 in [TODO.md](../TODO.md)
- Extends: product freeze in TODO (no preferred vertical from momentum)

## Context

SPECIALIZE-01 forbids preselecting tethered-studio or HDR/Panorama from
implementation momentum. An external photographer cohort, representative
workflow, hardware/corpus availability, and measurable product advantage are
still absent. Meanwhile the codebase must not grow a second live vertical
surface or imply packaged tethered capture.

## Decision

1. **Selected track (deferred):** tethered-studio is recorded as the **P2
   preferred specialization candidate** only as a product-planning label. It
   remains **deferred** until cohort evidence is attached in a later dated ADR.
2. **HDR/Panorama** stays unselected; do not start both.
3. **First Ready stub:** CatalogService exposes a fail-closed tethered-session
   probe (`ravo.specialize.tethered/v1`) that always returns
   `tethered_deferred` / `unsupported` until a future ADR admits a real
   adapter. No Studio chrome, no device I/O, no network listener.
4. No USB/network lifecycle, live view, client display, or package dependency
   is authorized by this ADR.

## Consequences

SPECIALIZE-01 gains an explicit deferred choice and a machine-visible
fail-closed stub so callers cannot mistake absence for silent success.
Implementation of real tethered capture requires a new dated ADR with cohort
evidence.

## Rejected alternatives

- Starting HDR/Panorama because stitching research exists
- Shipping Studio tethered UI over a stub session
- Claiming camera support from the stub
