# ADR-0158: Mask-scoped Develop workspace

- Status: Accepted
- Date: 2026-09-05
- Extends: ADR-0043/0044/0045, ADR-0145, LOCAL-01

## Decision

Develop has one explicit editing scope: Global or a stable local-adjustment ID.
The desktop C++ presenter owns that ephemeral selection and gesture lifecycle.
Recipe v4 adds `ravo.local.adjustment` v1: a named, enabled/bypassable operation
with one canonical mask root and a bounded ordered `children` operation array.
Groups cannot nest. Children are photographic RGB adjustments; RAW decoding,
geometry, display transforms, profiles, output decoration, and Retouch remain
global. Existing v1-v3 recipes remain readable and retain their rendering order.

Canonical paths retain their 32-point limit. Continuous brush strokes admit up
to 1,024 bounded control points, with the existing tessellation/work limits.
The immutable Engine mapping composes exact integer Canvas/crop layouts,
rotation/flip, Perspective and output-frame placement. Shared attachments are
read-only; cloning reserves every new ID before descending into children.

Engine runs the local RGB chain on an immutable group input, evaluates one mask
against the group's input/output, and performs one normal alpha mix. New groups
are inserted after global photographic adjustments and before final geometry
and display mapping. Imported groups retain their recipe position. CPU owns
this path, including alpha, memory bounds, cancellation, resources, and pixels.
No GPU error is caught and retried on CPU.

All local controls use neutral independent parameters. Reset, bypass, history
coalescing and gestures bind their scope ID. Completed gestures save through
the existing CatalogService revision-bound transaction and preview owners.
Done exits after pending commit; it is not a second save/history operation.
Selection changes and close clear ephemeral scope/gesture state and reject late
results. QML forwards bounded pointer intents and displays C++ geometry.

CLI/control expose the same scope and structural/geometry intents with explicit
asset and observed revisions. The transport owns no business state. History,
styles, copying, recovery and export retain groups and their referenced masks.

## Validation

Recipe/group bounds and round trips; zero/all/spatial alpha and nested-operation
rejection; global/local isolation; duplicate/delete ownership; gesture and
coordinate contracts; history/reopen/style/copy/recovery; stale/cancel/close
failures; actual CLI PNG artifacts; QML load and localization. Public changes
run the full available Ravo suite and report untested platforms.
