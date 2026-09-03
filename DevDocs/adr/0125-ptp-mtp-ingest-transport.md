# ADR-0125: Ingest transport URI, disconnect semantics, and filesystem-card first adapter

- Status: Accepted
- Date: 2026-09-03
- Relates: PRO-INGEST remaining work in [TODO.md](../TODO.md);
  [ADR-0102](0102-planned-managed-import-workspace.md),
  [ADR-0104](0104-bounded-rename-and-verified-second-copy-ingest.md)
- Extends: existing Add/Copy/Move planner publication rules
- Defers: native PTP/USB and MTP session stacks to a later transport adapter on
  the same contracts

## Context

Filesystem Add/Copy/Move ingest is accepted (ADR-0102/0104). Photographers still
need camera and card readers. A full libmtp / PhotoKit / WinRT PTP stack is too
large for one tranche, but the product already needs a **device/source URI**,
**disconnect/cancel** rules that compose with ADR-0104’s no-publish-on-conflict
and verified second-copy gates, and a shippable first adapter that exercises
those contracts on real card mounts and DCIM folders.

## Decision

### 1. Transport is an ingest source adapter, not a second catalog authority

Enumeration yields candidate filesystem paths (or, later, staged object bytes)
that feed the existing Add/Copy planner. Catalog publication stays ADR-0104:
preflight every primary/second-copy path, publish no catalog row on unresolved
conflict, verify second copies byte-for-byte before catalog entry.

### 2. Device / source URI abstraction

Every ingest source is identified by a `ravo-ingest:` URI:

- `ravo-ingest:filesystem-card:<absolute-root>` — first-ship transport (this
  ADR). `<absolute-root>` is a user-selected mounted volume, card mount, or
  folder (often ending in `DCIM`).
- `ravo-ingest:ptp-usb:…` — reserved for a later USB PTP adapter; parsing of
  unknown transports fails closed.

Stable object identity for resume is the portable relative path under the
enumeration root (filesystem-card). A later PTP adapter must map object handles
onto the same `IngestObjectId` shape without changing catalog publication.

### 3. Disconnect / cancel / partial semantics (fail-closed)

- **Cancel** or **source disconnect** mid-batch leaves the next ingest reusable:
  no half-published asset rows for the failing item; owned pre-catalog outputs
  for that item are removed; earlier successfully imported items remain explicit
  partial delivery.
- Remaining planned items after stop are reported per item with structured
  reasons (`cancelled` or `ingest_source_disconnected`) so callers can resume
  without guessing.
- **Source bytes remain read-only** for ingest transports in this tranche. Move
  (source-deleting) mode is rejected for `execute_ingest`. No camera-side
  delete/move/format.
- Second-copy verification still hashes destination bytes (ADR-0104).

### 4. First transport adapter: filesystem card mount / DCIM folder

Labeled clearly as **filesystem-card**, not PTP USB:

1. User initiates ingest with an explicit root path (Studio/CLI); no auto-import
   on plug.
2. If the root is named `DCIM` (ASCII case-insensitive) or contains a direct
   child `DCIM/`, enumeration prefers that DCIM tree; otherwise the selected
   folder is enumerated recursively as an ordinary photo folder.
3. The adapter opens an `IngestSourceSnapshot` (URI, enumeration root, media
   paths, object ids) and builds an `ImportRequest` for Copy or Add with the
   existing organization / rename / second-copy options.
4. Before each planned item, the service re-checks that the source root still
   exists as a directory. Disappearance models device unplug / mount loss.

### 5. Out of scope (explicit)

- Native PTP/MTP USB sessions, libmtp/PhotoKit/WinRT packaging, and auto-import
  watchers.
- DNG conversion, Smart Previews, HEIC owned decode (ADR-0123 residual).
- Treating the camera store as a live library folder.
- Mutating or deleting card/camera objects as part of Copy success.

## Consequences

PRO-INGEST gains an accepted transport contract and a tested filesystem-card
adapter that already covers disconnect, cancel, partial completion, source
preservation, rename, and verified second copy via ADR-0104. A later PTP USB
adapter plugs into the same URI + liveness + planner path without reopening
catalog publication rules.
