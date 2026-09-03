# ADR-0125: PTP/MTP ingest transport (decision stub)

- Status: Proposed
- Date: 2026-09-03
- Relates: PRO-INGEST remaining work in [TODO.md](../TODO.md);
  [ADR-0102](0102-planned-managed-import-workspace.md),
  [ADR-0104](0104-bounded-rename-and-verified-second-copy-ingest.md)
- Extends: existing Add/Copy/Move planner publication rules

## Context

Filesystem Add/Copy/Move ingest is accepted. Photographers still need camera and
card readers exposed over PTP/MTP. Device appearance/disappearance, partial
object reads, and source immutability interact with ADR-0104’s no-publish-on-
conflict and verified second-copy gates. This stub records the decision shape
only; it does not authorize a transport implementation.

## Proposed decision (narrow)

1. **Transport is an ingest source adapter**, not a second catalog authority.
   Enumeration yields candidate paths/object ids that feed the existing
   Add/Copy/Move planner; catalog publication stays ADR-0104.
2. **Device lifecycle is fail-closed.** Disconnect, cancel, or I/O error mid-
   batch must leave the next ingest reusable: no half-published asset rows, no
   silent source mutation, partial completion reported by item.
3. **Source bytes remain read-only.** No camera-side delete/move/format in this
   tranche. Second-copy verification still hashes destination bytes.
4. **Out of scope here:** DNG conversion, Smart Previews, HEIC owned decode
   (ADR-0123), background device watchers that auto-import without an explicit
   user-confirmed plan.

## Open questions before Accepted

- Host API choice (libmtp / platform PhotoKit / WinRT) and SPDX/package record.
- Stable device + object identity across reconnect for resume.
- Whether PTP objects map to temporary staging files before planner copy, or
  stream into the planner’s verified publish path.

## Rejected alternatives (preview)

- Auto-import on plug without an explicit plan.
- Treating the camera store as a live library folder.
- Mutating or deleting camera objects as part of Copy/Move success.
