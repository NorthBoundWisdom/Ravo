# Ravo Professional Photo Management TODO

> **Status: active**

This file contains unfinished execution work only. The accepted catalog,
recovery-sidecar, and verifiable-backup baseline is owned by
`Ravo/docs/adr/0097-catalog-recovery-sidecars-and-verifiable-backups.md` and the
current architecture, testing, migration, and capability documents.

## 1. Restore and operator workflows

- Add backup restore to a caller-selected absent destination. Verify the
  manifest and every payload before publication, open the restored database
  through the ordinary schema path, reject symlinks and existing destinations,
  and remove only the owned temporary on failure or cancellation.
- Define retention and scheduling policy without turning Studio startup or
  shutdown into an unbounded blocking operation. Expose last-success, next-run,
  bytes, and failure state through the shared service contract.
- Add Studio commands, progress, cancellation, and structured errors for
  recovery status/sync and backup/verify/restore. QML must only present C++
  state and forward intents.
- Add an explicit preview rebuild/status command for selected assets or the
  whole catalog, with bounded foreground/background priority and clean window
  destruction.

## 2. Durability failure coverage

- Extend schema-v6 recovery tests with injected failures for support-root
  creation, temporary create/write/sync/rename, acknowledgement, cleanup, and
  cancellation. Assert that a committed edit remains readable and its exact
  generation remains pending after every filesystem failure.
- Strictly validate every nested recovery-document value after checksum
  verification, including recipe/history bounds and canonical ordering. Reject
  malformed and newer documents without publishing or acknowledging them.
- Extend backup tests with concurrent catalog mutation, destination publication
  races, disk-full/write/sync/rename failures, cancellation during each
  interruptible phase, and cleanup. Decide whether the SQLite snapshot step
  needs a cancellable replacement for `VACUUM INTO` before scheduled backups.
- Add restore tests for empty and populated catalogs, tampered database,
  manifest and sidecars, unsupported/newer formats, destination conflicts,
  missing originals, and preview rebuild after reopen.

## 3. Import and preview scalability

- Add stable telemetry for enumeration, metadata probe, decode, catalog commit,
  browse-preview publication, first visible thumbnail, total throughput, peak
  bytes, cancellation latency, and queue depth. Record metric definitions and
  gates, not a private corpus path or per-run diary.
- Use those measurements to decide whether import needs bounded parallel
  probe/decode. If added, retain deterministic enumeration and per-item results,
  serialized catalog commits, bounded preview work, foreground Develop
  priority, and cancellation of undispatched work.
- Give folders stable catalog identity independent of display paths. Add
  missing-root and relink state, and justify every query/index change with
  `EXPLAIN QUERY PLAN` evidence.
- Cover duplicate-heavy trees, unsupported sidecars/video, Unicode and
  case-sensitive paths, corrupt or disappearing sources, preview-cache
  exhaustion, close/reopen, and destruction with active work.
- Exercise at least 10,000 synthetic catalog rows and a large mixed real-photo
  corpus. Listing memory and latency must depend on viewport/page size rather
  than total row count.

## 4. Gallery-to-Edit latency

- Keep Release probes for cold selection, cached Gallery selection, first owned
  960 px Edit frame, settled 1600 px frame, first slider frame, rapid A/B
  selection, and reopen. Report P50/P90/max by RAW/raster class and attribute
  decode, preprocess, recipe, histogram, and publication time.
- Freeze absolute host-local P90 gates for warm Gallery-to-Edit and first slider
  response, plus separate cold RAW/raster gates. Require exact cached/uncached
  pixels, bounded peak memory, and no import-to-first-thumbnail regression.
- Virtualize Gallery rows and request only viewport thumbnails plus bounded
  look-ahead. Entering Edit must supersede background work without starvation;
  selection alone must not start an unnecessary settled Develop render.
- Cover late-result rejection, rapid selection, source/preprocess changes,
  cache corruption and eviction, missing original with a valid browse preview,
  cancellation, worker-start failure, and window/catalog close.

## 5. Professional library workflows

- Add managed copy/move only with explicit collision, cross-volume,
  source-mutation, rollback, cancellation, and recovery-sidecar contracts.
  Originals remain read-only for ordinary referenced import.
- Decide measured user outcomes and ownership before adding collections,
  stacking/versions, duplicate-content detection, face/GPS search, removable or
  network catalogs, and legacy catalog import. Do not add placeholder tables.
- Treat adjacent standard XMP interoperability as a separate product decision.
  It must not silently consume or overwrite pre-existing XMP, and it must not
  become a second live authority beside SQLite.

## 6. Minimum validation

```text
python configs/source_roots.py show --format json
python configs/source_roots.py resolve --format json
python configs/source_roots.py verify
python3 Ravo/tools/check_ravo_dependency_boundary.py
cmake --preset mac_clang_debug -DBUILD_TESTING=ON
cmake --build --preset mac_clang_debug
ctest --test-dir build/mac_clang_debug --output-on-failure
cmake --preset mac_clang_release -DBUILD_TESTING=ON
cmake --build --preset mac_clang_release
ctest --test-dir build/mac_clang_release --output-on-failure
git diff --check
```

For a private real-photo run, set `RAVO_PHOTO_CORPUS`, write every generated
catalog, preview, sidecar, backup, and report into a unique temporary directory,
and compare a source metadata/SHA-256 manifest before and after. Probes must not
modify the source tree or a user catalog.

## 7. Completion gate

- Restore, retention/scheduling, Studio operations, and the durability failure
  matrix pass through the shared C++ service and versioned CLI contracts.
- Import, large-library browsing, and Gallery-to-Edit meet frozen Release gates
  on the same corpus with source integrity, exact pixels, bounded resources,
  cancellation, and destruction tests.
- Current capability, architecture, migration, testing, roadmap, ADR index, and
  documentation index agree; this TODO then contains only any separately
  accepted professional-library work.
- macOS evidence is reported only for macOS. Windows and Linux remain explicitly
  untested until their configure, build, test, and staged-install loops run.
