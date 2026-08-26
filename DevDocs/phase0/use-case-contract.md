# Phase 0 Use-Case Contract

## Status and authority

This is a historical Phase 0 contract. It still records valid CLI behaviours
and service-boundary evidence, but [ADR-0007](../../Ravo/docs/adr/0007-first-usable-catalog-viewer.md)
and the root [active migration TODO](../../TODO_LEGACY_MIGRATION.md) supersede its original delivery order.
The implementation-state column remains evidence; only the root roadmap can
promote an unfinished use case.

The command names, JSON envelope, exit codes, and no-write failure semantics
are part of the `ravo-cli/v1` contract. Adding a machine-visible command or
changing a completion condition requires a versioned contract decision. The
implementation-state column prevents a documented future command from being
mistaken for a current capability.

## Headless use cases

| ID | User or automation outcome | Machine-verifiable completion condition | Failure and mutation rule | Implementation state |
| --- | --- | --- | --- | --- |
| HC-01 inspect | Determine whether a local RAW, JPEG, PNG, or TIFF input is decodable without creating catalog state. | `ravo inspect <input> --json` returns a `ravo.cli.result` envelope containing the input URI, format, dimensions, decode capability, colour metadata, and available RAW metadata. The command changes no input or catalog file. | Missing input is exit `3`; malformed or unsupported input is structured non-zero. Cancellation is exit `7`. | Implemented for the first LibRaw 16-bit Bayer RAW slice; raster inputs, broader sensor layouts, and complete colour metadata remain Phase 2/3 work. |
| HC-02 operations | Discover stable operation IDs and parameter schemas for automation and future clients. | `ravo operations --json` emits only versioned JSON on stdout, with a unique ID and parameter schema version for each descriptor. | Unknown command/arguments are exit `2`; human diagnostics never contaminate JSON stdout. | Implemented in Phase 1. |
| HC-03 import legacy XMP | Convert an approved legacy sidecar into a canonical recipe without linking or loading legacy code. | `ravo recipe import-xmp <legacy.xmp> --asset-id <id> --input <uri> --output <recipe> --json` writes a schema-versioned recipe that validates through the Ravo facade. | An unproven operation is `unsupported_legacy_operation`; an existing output path is `conflict`/exit `6` and remains unchanged. | Phase 1 supports empty history and the schema-6/v5 manual, zero-black, unblended, mask-free singleton exposure subset; full histories and all other mappings remain Phase 2/3 work. |
| HC-04 validate and upgrade | Reject malformed or incompatible recipe data before any decode or output write. | `ravo recipe validate <recipe> --json` parses through the schema-upgrade path and reports recipe schema version, asset ID, and operation count. | Unknown fields, malformed values, and invalid ranges are exit `4`; newer unknown schemas are structured `unsupported`. | Implemented in Phase 1 for recipe schema version 1. |
| HC-05 deterministic CPU render | Produce a preview or full-size image from a canonical recipe using only the CPU reference engine. | `ravo render <input> --recipe <recipe> --output <image> --backend cpu --json` records output dimensions, colour policy, worker count, memory budget, interpolation policy, and deterministic option in machine-readable results or test evidence. Repeating the same request in one environment meets the documented tolerance. | Cancellation, decode, operation, memory, and write failures leave no successful-looking output. | Initial synchronous CPU slice implemented for nop/exposure, bounded dimensions, memory rejection, per-row cancellation and atomic PNG; golden tolerance, full colour contract, fixed worker/determinism options and full-size evidence remain incomplete. |
| HC-06 local export | Export JPEG, PNG, TIFF, or an unchanged original-file copy to local storage. | Each output type has a successful contract test covering pixel format/alpha, intended metadata and colour policy, output dimensions, and atomic commit. | Existing-target policy, disk-full, and write failures are structured and leave the old target unchanged. | CatalogService/CLI/Studio export JPEG, PNG, optional TIFF, and original-byte copy. Existing targets are `conflict`; cancel leaves no dest file. Disk-full is `io` with `reason=disk_full` when the OS reports ENOSPC. Metadata/ICC embedding is not in this slice. |
| HC-07 batch processing | Process an explicitly supplied finite set of independent render requests without catalog state. | The public batch form reports one versioned result per request, has a deterministic aggregate exit status, and supports a fixed worker and memory budget. The first implementation may be an explicitly documented script over HC-05; no speculative filter language is introduced. | One request failure is reported with its correlation ID; cancellation stops undispatched work and does not mark incomplete outputs successful. | Phase 3 required. |
| HC-08 cancellation and recovery | Stop active work and safely retry after a process or I/O failure. | A test cancels during decode, evaluation, and output; a subsequent fresh request can run with no stale buffer, task, or temporary output visible. | SIGINT or the platform equivalent maps to exit `7`; task errors remain structured. | Deadline, pre-render and row-boundary cancellation exist; process-signal wiring, decode/output interruption and retry-after-failure coverage remain Phase 2/3 work. |
| HC-09 offline execution | Use local files and already materialised dependencies without runtime network access. | A network-denied test environment can run HC-01 through HC-08 against local fixtures. | Lack of a network is never silently replaced with a download or cloud request. | Phase 2/3 required. |

## Desktop service contracts

ADR-0007 authorizes the `domain`, `services`, and `desktop` targets needed for
DC-01. The remaining rows are contract evidence, not blanket implementation
authorization for editing, masks, styles, or export scope.

| ID | Desktop outcome | Required engine/service contract | Required lifecycle and error proof |
| --- | --- | --- | --- |
| DC-01 catalog import and browse | Import a local directory, then browse a Grid and Loupe without invoking the CLI. | Versioned import/query commands, immutable photo/version snapshots, preview resource handles, and repository ports. | Duplicate import, empty directory, corrupt file, cancellation, and shutdown leave catalog state consistent. |
| DC-02 single-image edit | Change an operation parameter and view a new preview for one photo version. | Immutable recipe replacement, operation schema query, preview request, correlation/version IDs, and progress events. | A stale preview after a new edit, fast photo switch, or window destruction is discarded. |
| DC-03 history and undo/redo | Traverse non-destructive edits for a selected photo version. | Versioned recipe snapshots and history commands; no widget owns mutable operation parameters. | Undo/redo after cancellation or persistence failure produces a readable result and no half-applied edit. |
| DC-04 masks and blend | Attach, modify, and remove a canonical mask/blend graph from an operation. | Mask IDs, schema versions, validity rules, and CPU render semantics. | Invalid references and unsupported mask forms are rejected before render; expired previews are released. |
| DC-05 styles and presets | Apply a stored reusable recipe fragment to selected photos. | Versioned style data and explicit merge/conflict policy, independent of legacy opaque blobs. | Unsupported legacy data has a migration report; no partial style application remains hidden. |
| DC-06 batch export | Queue multiple local exports from selected versions. | Export task handles, durable command/result records, progress, cancellation, and output-conflict policy. | Disk-full, target conflict, retry, application exit, and restart preserve trustworthy task state. |

## First vertical-slice evidence gate

The first CPU vertical slice is intentionally not declared accepted until Ravo
has produced its own image, metadata, recovery, and performance evidence. Its
static candidate input set is already complete and hash-protected; no old
binary, toolchain, or runtime reproduction is required:

| Evidence | Candidate | Current state | Gate before Phase 2 acceptance |
| --- | --- | --- | --- |
| Baseline input | `legacy/tests/images/mire1.cr2` | Hash recorded in `tests/fixtures/legacy_manifest.json`. | Verify the recorded hash before every Ravo comparison. |
| Neutral fixture | `legacy/tests/0000-nop/nop.xmp` and `expected.png` | Ravo strictly absorbs the exact built-in RAW baseline and renders a bounded PNG; frozen hashes remain `not-run-by-policy` for the old CPU. | Save the Ravo canonical recipe and compare full-size pixels/metadata to the committed reference. |
| Visible-operation fixture | `legacy/tests/0001-exposure/exposure.xmp` and `expected.png` | The adapter proves only a schema-6/v5 manual, zero-black, unblended, mask-free singleton exposure entry; this full history still has prerequisite operations and blend data. | Prove the full ordered pipeline and blend/mask semantics, then save the canonical recipe, synthetic boundary test, and pixel/metadata comparison. |
| Float and metadata evidence | Ravo-owned 32-bit float TIFF plus decoded metadata summary | Not created. | Store output hashes, dimensions, colour/ICC policy, alpha/orientation, metadata summary, and tolerances beside the Ravo test data. |
| Performance and recovery evidence | Fixed CPU, worker count, memory budget, and output location | Rendering exists, but no representative latency, peak-memory, cancellation-latency or retry measurement has been recorded. | Record first-preview and full-export latency, peak memory, cancellation latency, and retry outcome using the same request. |

The old project and its runners must not be invoked. Frozen source, XMP, RAW,
and `expected.png` assets are immutable reference inputs; Ravo output is stored
separately and never used to replace them. The candidate paths and hashes are checked against the inventory by
[`check_vertical_slice_plan.py`](../../Ravo/tools/check_vertical_slice_plan.py); the
candidate remains `candidate-not-frozen` until every table gate is met.
