# Ravo Repository Instructions

This file applies to the entire `Ravo/` subtree and adds constraints to the
root `AGENTS.md`. When they conflict, apply the stricter and more-specific
constraint.

## Before starting work

1. Run `git status --short --branch` and preserve user changes.
2. Read `Ravo/README.md`, `../DevDocs/ARCHITECTURE.md`,
   `../DevDocs/MIGRATION.md`, `../DevDocs/TESTING.md`, and relevant ADRs.
3. For old behavior, read Ravo code, tests, and `tests/fixtures/frozen`; do
   not infer from upstream darktable habits.
4. Confirm that work belongs to an item in
   [`../DevDocs/TODO.md`](../DevDocs/TODO.md) or to a cross-cutting reliability
   gate. Current Windows/Linux/macOS package evidence lives in
   [`../DevDocs/Packaging.md`](../DevDocs/Packaging.md) and does
   not independently authorize product implementation. Desktop uses Qt 6
   Quick/QML; Qt Sql remains solely in the private SQLite adapter. Qt Widgets,
   a second presentation architecture, and an old GTK adapter are not approved.
5. Before cross-layer work, state ownership, lifecycle, thread boundaries,
   error/cancellation paths, and the smallest validation set.

## Current technical boundaries

- All first-party new code uses C++20, CMake, and FreeCM; do not add Rust/Cargo
  to the build graph.
- CPU is the correctness reference. Do not port 0.9 OpenCL. GPU is an Engine
  QRhi adapter (ADR-0133/0134), with no silent CPU fallback
  (`../DevDocs/MIGRATION.md`). Preview keeps unmasked Exposure, light
  controls, Lab USM, and Sigmoid on one GPU SSBO session; remaining RGB ops
  stay CPU. Bayer window RCD for ROI 1:1 is GPU when admitted; PPG and export
  stay CPU.
- The `ravo` CLI and Ravo Studio are both supported clients. Algorithms belong
  in the engine; catalog/import/preview orchestration belongs in services; CLI
  and UI are limited to input/output, progress, selection, and error
  presentation.
- The first operation version uses built-in registration. Do not restore the
  old dynamic IOP ABI, GTK ABI, or a plugin compatibility layer.
- Recipes, operation IDs, parameter schemas, and machine JSON must be
  versioned. Do not serialize object memory layout or UI state.

## Machine automation and agent control

- The root prohibition on Computer Use applies to every Ravo task. Do not use
  accessibility trees, synthetic UI input, or window screenshots as a fallback
  when a CLI, service, or state contract is missing.
- Use the executable from the active Ravo build and its machine JSON surface.
  For a running window, start with `studio sessions` and `studio state`; bind a
  mutation to that snapshot's session, selection, asset, and recipe revisions,
  then use `studio develop` and re-read `studio state`. For explicit stored
  state, use `catalog list`, `catalog recipe` / `catalog history`, and
  `catalog probe --set <field>=<value> --output <unique.png> --json` for a
  non-persistent parameter response. Inspect the emitted PNG directly with a
  local image reader. Use `catalog develop` only for an explicitly identified
  persistent edit, then re-read recipe/history and the structured result.
- `catalog probe` is the current visual-automation owner. It must remain
  read-only, use the same interactive preview service as Studio, publish an
  atomic no-replace artifact, report display-RGB statistics, and prove that
  recipe serialization and preview records did not change. Do not add a second
  screenshot renderer or external decoder as an oracle.
- The CLI exposes a running Studio window only through
  `ravo-studio-control/v1`. Process arguments, open-file lists, preview cache
  activity, and log messages may help diagnose a process but are not
  authoritative state or mutation targets.
- The live Studio automation contract is owned by desktop C++ for ephemeral
  session state and routes mutations through the existing
  `StudioCommandController`; persistent work continues through services. Its
  immutable snapshot must at least identify protocol version, session ID,
  catalog path/revision, primary and selected asset IDs, browse mode, saved or
  pending recipe revision, and preview resource identity. Mutations carry the
  observed session and selection revisions and fail explicitly on stale state,
  catalog close, selection change, cancellation, or command unavailability.
- A local control transport must be same-user, bounded, non-networked by
  default, multi-session aware, and destroyed with its Studio owner. It must
  not expose QObjects, raw pointers, SQLite handles, engine buffers, settings
  secrets, or a writable singleton. UI-main-thread snapshots and command
  dispatch remain short; decode/render/database work stays in existing
  owner-managed tasks.
- CLI is the mandatory protocol client because its versioned JSON, exit codes,
  and subprocess tests are cross-platform and deterministic. An MCP server is
  optional and may expose the same snapshots, commands, and image results as
  tools/resources only after the underlying contract exists. MCP must not add
  state, permissions, render paths, or mutations unavailable through the shared
  contract and CLI acceptance client.
- Large image bytes do not belong inline in CLI JSON. New or extended
  image-result contracts return a no-replace local artifact path plus MIME type,
  dimensions, color-profile identity, content hash, and lifecycle; an MCP
  adapter may return those same immutable bytes as an image resource. Tests
  cover invalid/stale sessions, wrong asset/revision, concurrent Studio and CLI
  writes, cancellation, output conflict, process exit cleanup, bounded
  messages/images, and absence of sensitive settings.
- `ravo_control` owns only protocol framing, owner-only discovery, and Qt local
  sockets. Qt Network in CLI/control is restricted to that local transport;
  assistant HTTP and credentials remain desktop-only. Image results are
  rendered by the existing CatalogService/Engine preview path after a recipe
  snapshot, never by the control transport.

## Dependency rules

- The repository-root active lock manages third-party source-root state. Before
  changing or diagnosing a dependency, run `show`, `resolve`, and `verify` as
  documented in `../DevDocs/Dependency_Workflow.md`; do not inspect only the
  template or modify `../build/dependency_source_roots/*` directly.
- Local dependency integration uses `depsMode=manual` and the matching
  `depsManualPath` only in ignored `../source_roots.lock.jsonc`. Run the root
  `configs/source_root_workflow.py --update` to confirm wiring before
  configuring or building. Publish and verify the remote SHA before updating a
  tracked dependency template.
- `foundation` does not depend on recipe, engine, CLI, catalog, UI, or platform
  implementation.
- `recipe` depends only on foundation; it knows neither pixel executors,
  databases, nor UI.
- `engine` depends on foundation/recipe and its declared ports. It may use
  QtCore directly; other third-party concrete types should remain in private
  adapters.
- `domain` depends on foundation and owns Asset/Catalog/Import/Preview state
  and repository ports. It knows neither SQLite, codecs, QML/presentation
  types, nor engine-private types.
- `services` depends on domain and the engine facade and owns
  create/open/import/list/preview use cases and task orchestration. It must not
  issue SQL or hold QML objects/presentation models.
- `control` depends only on foundation plus Qt Core/Network and owns bounded
  local protocol/discovery/transport. It knows no recipe, catalog, services,
  engine, command policy, QML, or settings state.
- `adapters` implement SQLite, filesystem, RAW/raster codec, and preview-cache
  ports. `QSqlDatabase`, `QImageReader`, and other third-party handles remain
  private.
- `cli` depends on services/engine facade, adapter composition, and control; it must not
  include algorithm source, SQL, or UI.
- `desktop` consists of a C++ composition root, desktop-owned QObject
  presenter/models, and QML views. It depends only on services and the
  read-only preview-resource contract. QML/JavaScript only presents, binds, and
  forwards input; it must not access the database, codecs, engine-private
  state, or own business rules.
- Ravo production code must not include leftover darktable headers, link old
  libraries, `dlopen` old modules, or read old global state. Tests use
  `tests/fixtures/frozen` and must not add a leftover tree or leftover runner.

## C++ implementation rules

- Use value semantics, immutable snapshots, RAII, and explicit owners. Raw
  pointers to owned resources must not cross public boundaries.
- Every `view`/`span`/`string_view` needs a provable lifetime documented in its
  interface.
- Asynchronous work uses an owner-managed executor, task handle, and
  cancellation token; detached threads are prohibited.
- Use inspectable results and structured errors. Exceptions must not cross
  target ABI, C callbacks, tasks, or future FFI boundaries.
- Do not introduce a writable service set equivalent to global `darktable`, or
  use a singleton to bypass dependency injection.
- Format only touched code. Update ADRs, architecture, and validation notes
  with public API, dependency, thread, or data-format changes. Installed commit
  hooks format staged `Ravo/` C/C++ and, when configured, QML/JS; do not
  manually redo their pure formatting changes.

## Algorithm migration

Unaccepted leftover image algorithms are leftovers, not ports (ADR-0106).
Do not rewrite leftover C, OpenCL, GTK, or dynamic ABI to finish leftover.
New photographic tools are independent Ravo product work under `TODO.md`
and `ProductRoadmap.md`.

Accepted Ravo operations keep their current contracts. Frozen fixtures live
in `tests/fixtures/frozen`.

Do not port leftover OpenCL. A future GPU path is an Engine adapter only, with
no silent CPU fallback, and needs a dated ADR (`../DevDocs/MIGRATION.md`).

## Validation and delivery

- Run the `RavoCodeQuality` target after adding or splitting C++ translation
  units or production QML. Keep both source-size debt manifests empty and
  preserve the frozen test-case inventory and target membership while splitting
  tests.
- On Windows, new project helpers use only Python or PowerShell; do not add an
  old application runner. CMake/MSVC configures and compiles only `Ravo/`
  targets.
- For documentation-only changes, check real paths, relative links, commands,
  terminology, and the diff with `git diff --check`.
- New C++ units link only Ravo targets and run relevant unit/contract tags and
  feasible sanitizer sets.
- Catalog/import work covers schema create/reopen/migrate, URI idempotence,
  transaction rollback, partial failure, cancellation, source disappearance,
  atomic preview cache, and resource destruction. Hash the source image before
  and after tests to prove it was not modified.
- New targets update `tools/check_ravo_dependency_boundary.py` in the same
  change: scan CMake targets and QML imports; `Qt6::Sql` is adapters-only,
  `Qt6::Gui` is raster adapters, desktop, and the Engine QRhi GPU adapter,
  `Qt6::Qml`/`Qt6::Quick`,
  `QtQuick.Controls`/`QtQuick.Dialogs`/`QtQuick.Layouts`, and production `.qml`
  are desktop-only, `Qt6::QuickTest` and QML tests are desktop-test-only; every
  Ravo target rejects Qt Widgets and frozen `src`/GTK dependencies.
- For desktop, validate business behavior through service integration tests
  first, then exercise supported intents through the CLI/control contract and
  run QML load/offscreen smoke. Do not use Computer Use to satisfy a manual
  acceptance step; add the missing machine interface or report the user-run
  visual check as untested. Qt Quick Test, CLI artifacts, and UI smoke do not
  replace the domain/service contract.
- For an operation, run parameter/schema, synthetic-boundary, old-XMP mapping,
  and committed RAW/PNG/metadata fixture checks. Do not launch an old process
  for live differential output.
- For public headers or broad scheduling changes, run the full Ravo
  unit/contract suite, retain fixtures, and build every feasible platform.
- Do not report unrun tests as passing or a macOS result as cross-platform
  success. Do not commit, amend, rebase, or push the parent repository without
  an explicit request. A commit with seed changes follows the root
  `review-and-commit` skill: push the dependency first, pin its published SHA,
  then commit the parent.

Ravo's Windows/MSVC build commands and FreeCM project commands live in
`README.md` and `../configs/freecm.commands.jsonc` respectively. These entries
may only configure, build, run, and test Ravo.
