# ADR-0015: Migrate every remaining non-UI algorithm and delete legacy UI

- Status: Accepted
- Date: 2026-08-26
- Supersedes: ADR-0010 final leftover disposition; product-decision register algorithm leftovers
- Relates to: ADR-0004, ADR-0010, root `TODO_LEGACY_MIGRATION.md`

## Context

The migration policy previously selected one default display transform and one
HSL zoning operation, then treated many alternative, creative and diagnostic
modules as permanent leftover. The product owner clarified that the end state
is broader: legacy UI is deleted, while the remaining image algorithms are
rewritten in C++ and verified by Ravo tests.

## Decision

- Every remaining pixel, color, RAW, geometry, mask/blend, metadata-transform,
  codec or output algorithm in `legacy/src` is a migration candidate. Default
  choices such as Sigmoid and `colorequal` stay defaults; alternatives become
  explicit optional operations rather than hidden fallback or permanent
  leftover.
- Migration remains serial under the active root TODO. Each capability gets an
  explicit schema/workspace/ROI contract, the frozen default CPU mathematics,
  synthetic and real-fixture UT, cancellation/resource errors and formal
  CLI/Studio/service consumers before its old owner is removed.
- Existing simplified Ravo controls do not prove migration of a same-named old
  module. The old owner retires only after the full frozen path is implemented,
  or after an evidence-backed consolidation proves the accepted operation
  strictly subsumes it.
- GTK Lighttable/Darkroom, dtgtk/Bauhaus, module layout, chart/picker UI,
  diagnostic presentation, map/tether/print/publish shells and other legacy UI
  are not ported. Their code is deleted when no unaccepted algorithm still
  depends on it.
- Lua, dynamic IOP/plugin ABI, writable global `darktable` state and old OpenCL
  are deleted, not wrapped. Ravo GPU remains an independent post-CPU adapter.
- Legacy catalog/styles/preset/XMP binary compatibility is still a separate
  data decision. Rejecting an old serialization format does not waive migration
  of its pixel algorithm.

## Consequences

- `filmicrgb`, `agx`, `colorzones`, creative modules, mask-dependent modules
  and diagnostic computations leave the permanent-leftover list and enter
  dependency-ordered migration planning.
- The capability inventory's `queued` state means product scope is accepted but
  execution is blocked by the named owner/contract, not by another product
  decision.
- Final deletion of `legacy/src` happens only after all algorithm owners are
  accepted/retired and shared UI/infrastructure has no remaining consumer.
- The frozen fixtures remain read-only evidence throughout; no legacy binary is
  configured or executed.

## Rejected alternatives

- Keep a curated subset and archive the rest: this contradicts the requested
  full algorithm migration.
- Port the GTK application and swap algorithms underneath it: Ravo Studio is
  the only presentation architecture.
- Bulk-copy C sources or expose the old dynamic ABI: ownership, cancellation,
  schemas and tests would remain legacy-shaped.
