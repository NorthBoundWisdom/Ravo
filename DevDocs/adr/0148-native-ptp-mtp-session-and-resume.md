# ADR-0148: Native PTP/MTP session identity, platform matrix, and ingest resume

- Status: Accepted
- Date: 2026-09-04
- Relates: INGEST-01 in [TODO.md](../TODO.md)
- Extends: [ADR-0125](0125-ptp-mtp-ingest-transport.md),
  [ADR-0102](0102-planned-managed-import-workspace.md),
  [ADR-0104](0104-bounded-rename-and-verified-second-copy-ingest.md)
- Does not supersede ADR-0125 filesystem-card; that adapter remains first-ship
  for mounted volumes/DCIM folders.

## Context

INGEST-01 needs a native camera/session transport on the same ingest URI and
Copy planner as ADR-0125. Packaging ImageCaptureCore / WinRT Portable Devices /
libmtp is a Dependency Workflow + Packaging gate. Until an adapter is packaged,
live PTP/MTP must fail closed with an explicit platform support matrix rather
than treating a filesystem mount as a native session. Incomplete batches must
resume after reconnect without publishing unverified copies or inventing a
second planner.

## Decision

### 1. Platform support matrix (first Ready)

`probe_native_ingest_support()` reports:

| Platform | Planned PTP stack | Planned MTP stack | First Ready state |
| --- | --- | --- | --- |
| macOS | ImageCaptureCore / PTP USB | host MTP when packaged | `unsupported` until packaged |
| Windows | WinRT Portable Devices / WPD | WinRT MTP | `unsupported` until packaged |
| Linux | libmtp (PTP mode) | libmtp | `unsupported` until packaged |

`native_ingest_adapter_is_packaged()` is false until Dependency Workflow records
a named binary/library with SPDX/notices. Absent packaging, open/enumerate of
`ravo-ingest:ptp-usb:…` and `ravo-ingest:mtp:…` fails closed with reason
`native_ingest_adapter_not_packaged` and machine context (`platform`,
`adapter_packaged=false`). Never pretend a card mount is a native session.

### 2. Session and object identity

- PTP USB URI: `ravo-ingest:ptp-usb:<vendor_id>:<product_id>:<serial>`
- MTP URI: `ravo-ingest:mtp:<bus_or_vendor>:<dev_or_product>:<serial>`
- Session identity is the URI plus optional display name; one selected
  device/session per ingest call (no auto-import on plug).
- Object identity maps onto existing `IngestObjectId.relative_path`
  (`storage/<storage_id>/<object_path>`). Optional `object_handle` is advisory.
- Catalog publication remains ADR-0104: Copy/Add planner only; no Move; no
  camera-side delete/format.

### 3. Fixture stub for contract tests (`ptp-stub`)

`ravo-ingest:ptp-stub:<absolute-fixture-root>` is a **test/fixture adapter**
that enumerates a local tree with PTP-shaped object ids and feeds the same
Copy planner. It never claims `adapter_packaged` or live USB support. Production
CLI defaults reject inventing stub URIs as a substitute for native sessions;
tests use it to prove resume and per-item reports without hardware.

### 4. Resume checkpoints

Durable checkpoints live under
`{catalog}.ravo/ingest-resume/<batch_id>.json` (`ravo.ingest.resume/v1`):

- Record `source_uri`, transport, destination/second-copy/rename options, and
  completed object `relative_path`s only after primary (+ second copy when
  requested) verification succeeds.
- Incomplete or failed items are never listed as completed and never publish
  catalog rows (ADR-0104 / ADR-0125).
- Resume after reconnect loads the checkpoint, skips completed objects
  (`ImportItemStatus::kSkipped`, reason `ingest_resume_already_completed`),
  re-checks source liveness, and continues remaining objects.
- Cancel/disconnect leaves the checkpoint reusable; successful full completion
  clears the checkpoint file.
- Idempotent repeated ingest of already-catalogued destinations still relies on
  existing duplicate detection; resume skips are orthogonal.

### 5. Structured per-item report

Ingest reports per item: `imported`, `duplicate`, `skipped` (resume),
`unsupported`, `failed` (including `cancelled` / `ingest_source_disconnected`
reasons). Batch totals expose `skipped` alongside existing counters.

### 6. Out of scope (explicit)

- Shipping libmtp / ImageCaptureCore / WinRT packaging in this tranche.
- Packaged native adapters (Studio source-selector + filesystem-card/stub C2 is in-tree; live USB remains residual).
- Move mode, camera delete, auto-import watchers, DNG-at-ingest.

## Consequences

INGEST-01 gains an accepted native session/resume contract and a fail-closed
probe with clear machine state. Fixture `ptp-stub` + resume tests prove the
planner path without claiming hardware support. A later packaged adapter plugs
into the same URI, checkpoint, and Copy planner.

## Rejected alternatives

- Treating filesystem-card mounts as “native PTP” when USB stacks are absent.
- Publishing catalog rows for incomplete copies and “fixing up” later.
- Auto-delete on camera after Copy success.
- Embedding opaque third-party session handles as catalog primary keys.
