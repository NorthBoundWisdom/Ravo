# Ravo Repository Instructions

This file applies to the entire `Ravo/` subtree and adds constraints to the
root `AGENTS.md`. When they conflict, apply the stricter and more-specific
constraint.

## Before starting work

1. Run `git status --short --branch` and preserve user changes.
2. Read `Ravo/README.md`, `ARCHITECTURE.md`, `MIGRATION.md`, `TESTING.md`, and
   relevant ADRs.
3. For old behavior or algorithms, read the corresponding `legacy/src/`
   implementation and fixtures; do not infer from upstream darktable habits.
4. Confirm that work belongs to the current queue item in
   [`../TODO_LEGACY_MIGRATION.md`](../TODO_LEGACY_MIGRATION.md) or to a
   cross-cutting reliability gate. Desktop uses Qt 6 Quick/QML; Qt Sql remains
   solely in the private SQLite adapter. Qt Widgets, a second presentation
   architecture, and an old GTK adapter are not approved.
5. Before cross-layer work, state ownership, lifecycle, thread boundaries,
   error/cancellation paths, and the smallest validation set.

## Current technical boundaries

- All first-party new code uses C++20, CMake, and FreeCM; do not add Rust/Cargo
  to the build graph.
- CPU is the correctness reference and reliable fallback. GPU may be added as
  an adapter only after the CPU/golden/performance gates in `MIGRATION.md` and
  `../DevDocs/GPU_Baseline.md` are met.
- The `ravo` CLI and Ravo Studio are both supported clients. Algorithms belong
  in the engine; catalog/import/preview orchestration belongs in services; CLI
  and UI are limited to input/output, progress, selection, and error
  presentation.
- The first operation version uses built-in registration. Do not restore the
  old dynamic IOP ABI, GTK ABI, or a plugin compatibility layer.
- Recipes, operation IDs, parameter schemas, and machine JSON must be
  versioned. Do not serialize object memory layout or UI state.

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
- `adapters` implement SQLite, filesystem, RAW/raster codec, and preview-cache
  ports. `QSqlDatabase`, `QImageReader`, and other third-party handles remain
  private.
- `cli` depends on services/engine facade and adapter composition; it must not
  include algorithm source, SQL, or UI.
- `desktop` consists of a C++ composition root, desktop-owned QObject
  presenter/models, and QML views. It depends only on services and the
  read-only preview-resource contract. QML/JavaScript only presents, binds, and
  forwards input; it must not access the database, codecs, engine-private
  state, or own business rules.
- Ravo production code must not include `legacy/src/` headers, link old
  libraries, `dlopen` old modules, or read old global state. Tests may read
  frozen fixtures and source only; they must not configure, compile, or run the
  old CLI, old CTest, or `legacy/tests/run`.
- The old application does not reuse Ravo and receives no adapter. Production
  dependencies remain completely independent. Delete old owners only after the
  active migration TODO accepts them.

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
  manually redo their pure formatting changes or format `legacy/`.

## Algorithm migration

- The migration unit is a user- and CLI-observable capability/operation batch,
  not a directory or line count.
- Read frozen C source directly and reproduce its default CPU path: formulas,
  color space, filters, and default mode must match. Then cover key branches
  with focused Ravo unit tests. Do not compile or run old unit tests.
- GUI, old module lifecycle, configuration shims, dynamic registration, OpenCL
  types, and unconsumed code may be removed. A simplified substitute algorithm
  must not be presented as a completed migration (for example, HSL for UCS,
  neighborhood average for opposed reconstruction, three-level Gaussian for
  a-trous Y0U0V0, or five bands for nine-band toneequal).
- After the active root migration TODO accepts a Ravo item, delete the
  corresponding legacy owner (CMake, registration, resources, documentation,
  and checks). Do not delete unaccepted items or leftovers early. Search the
  whole repository to confirm there are no consumers.
- Do not port old OpenCL to Ravo or replace it with Metal in 0.9. Ravo GPU may
  only be implemented as an independent adapter after its own CPU path is
  accepted.

## Validation and delivery

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
  `Qt6::Gui` is raster adapters/desktop-only, `Qt6::Qml`/`Qt6::Quick`,
  `QtQuick.Controls`/`QtQuick.Dialogs`/`QtQuick.Layouts`, and production `.qml`
  are desktop-only, `Qt6::QuickTest` and QML tests are desktop-test-only; every
  Ravo target rejects Qt Widgets and frozen `src`/GTK dependencies.
- For desktop, validate business behavior through service integration tests
  first, then conduct minimum manual Create/Open, Import, select, Fit, and 100%
  acceptance. Qt Quick Test and UI smoke do not replace the domain/service
  contract.
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
