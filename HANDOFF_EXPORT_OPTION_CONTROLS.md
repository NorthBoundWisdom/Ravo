# Development prompt: complete explicit export controls

Implement this whole bounded tranche, validate it, and leave the working tree
ready for review. Do not commit, amend, rebase, or push unless the user
explicitly asks.

## Goal

Expose the already accepted typed JPEG, PNG, and TIFF export options through
the remaining public control surfaces without changing any encoder algorithm.
Complete both of these consumers in one tranche:

1. extend `ravo catalog export` with explicit JPEG subsampling and TIFF
   resolution, and make every render-format option fail closed outside its
   owning format; and
2. replace Ravo Studio's implicit-default-only export action with an explicit
   format/options intent that reaches the existing `ExportRequest` as one
   immutable typed snapshot.

This is independent adapter/UI work permitted while the C14 algorithm queue is
paused. It must not reopen C14, S2.2, C15, masks, or another image algorithm.
The work is primarily a translation layer over values already owned by
`JpegExportOptions`, `PngExportOptions`, and `TiffExportOptions`; do not create
a second set of codec settings.

The complete accepted control matrix for this tranche is:

| Format | Existing typed fields exposed by CLI and Studio |
| --- | --- |
| JPEG | quality `5..100`, subsampling `auto|444|440|422|420` |
| PNG | bit depth `8|16`, compression `0..9` |
| TIFF | sample type `uint8|uint16|float16|float32`, compression `none|deflate|deflate_predictor`, compression level `1..9`, conditional grayscale boolean, resolution `72..9600` dpi |
| Original copy | no rendered-format options; exact source bytes only |

Defaults remain exactly the domain defaults: JPEG quality 95/auto, PNG
8-bit/compression 5, TIFF uint8/Deflate-predictor/level 6/RGB/300 dpi. Every
Studio export invocation begins from those defaults. This tranche adds no
preset, remembered last value, database row, settings key, batch job, path
template, or compatibility state.

Acceptance requires all of the following:

- CLI and Studio construct the same typed `ExportRequest` values for the same
  selections. CatalogService and the private encoders remain the only semantic
  validators/consumers downstream.
- Existing default CLI commands and default Studio exports remain byte-for-byte
  compatible except for the already accepted TIFF `DocumentName` destination
  difference between distinct paths.
- Every explicit option demonstrably configures the intended codec contract:
  JPEG SOF sampling factors, PNG IHDR/compression behavior, and TIFF
  sample/compression/resolution/photometric tags remain independently parsed in
  tests. Do not prove forwarding only by checking a UI map.
- Wrong-format, wrong-subcommand, malformed, missing, unknown, duplicate
  boolean, out-of-range, unknown-key, and path/format mismatch inputs fail with
  stable structured errors before rendering and publish no destination.
- The selected format and all options are validated and copied before the
  Studio executor task is posted. Later QML changes, dialog reuse, selection
  changes, or window shutdown cannot mutate an in-flight request.
- Existing encoded metadata, ICC/cICP, pixels, precision, compression,
  cancellation, resource destruction, and ADR-0032 atomic no-replace
  publication behavior remain unchanged.
- No fallback, localized-filter parsing, hidden default PNG choice, second
  export pipeline, new dependency, schema migration, or change under `legacy/`
  or `build/dependency_*` is added.

The durable authorities remain
[`TODO_LEGACY_MIGRATION.md`](TODO_LEGACY_MIGRATION.md),
[`Ravo/README.md`](Ravo/README.md),
[`Ravo/ARCHITECTURE.md`](Ravo/ARCHITECTURE.md),
[`Ravo/MIGRATION.md`](Ravo/MIGRATION.md),
[`Ravo/TESTING.md`](Ravo/TESTING.md), and the accepted export ADRs. This
handoff supplements them; it does not override them.

## Product and architecture boundary

### One typed request owner

- Reuse the existing domain enums, canonical-name parsers, defaults, and
  validators. Do not add Qt types to domain/services, codec enums to QML, or a
  parallel `Studio*ExportOptions` public model.
- A desktop-private conversion helper may consume a strict Qt presentation map
  and return an owned value containing `ExportFormat`, `JpegExportOptions`,
  `PngExportOptions`, and `TiffExportOptions`. Keep this helper synchronous,
  deterministic, directly unit-testable, and free of service/file writes.
- The Studio command controller validates the command payload shape. The C++
  presenter/conversion owner performs semantic parsing, format isolation,
  bounds, and path normalization before posting work. QML displays state and
  forwards one intent only.
- Once validation succeeds, capture the normalized output path, selected asset
  id, explicit format, and all typed options by value in the existing serial
  executor closure. Reuse the existing shutdown cancellation token and one
  `CatalogService::export_asset` call.
- Original copy must construct no rendered metadata/options state beyond
  domain defaults and must stay on the exact-byte service path.

### CLI contract

Keep existing canonical flags and add exactly:

```text
--jpeg-subsampling auto|444|440|422|420
--tiff-resolution-dpi 72..9600
```

The full command becomes conceptually:

```text
ravo catalog export --catalog <library.sqlite> --asset-id <id> \
  --output <file> --format jpeg|png|tiff|tif|original \
  [--quality 5..100] [--jpeg-subsampling auto|444|440|422|420] \
  [--png-bit-depth 8|16] [--png-compression 0..9] \
  [--tiff-sample-type uint8|uint16|float16|float32] \
  [--tiff-compression none|deflate|deflate_predictor] \
  [--tiff-compression-level 1..9] \
  [--tiff-grayscale-if-neutral] [--tiff-resolution-dpi 72..9600] --json
```

Freeze these rules:

- `--quality` and `--jpeg-subsampling` are JPEG-only. They may no longer be
  silently accepted for PNG, TIFF, original copy, or non-export catalog
  subcommands.
- PNG and TIFF flags retain their format-qualified behavior. Add TIFF
  resolution to the same owner/checks as the other TIFF flags.
- Value flags use the repository's existing last-value-wins convention.
  `--tiff-grayscale-if-neutral` remains a one-shot boolean and rejects a
  duplicate.
- Missing values retain the generic option/value error shape. Invalid canonical
  enum text must come from the domain parser. Integer syntax and range errors
  must retain the option, supplied value where available, owning format, and a
  stable reason.
- Format isolation is checked before opening the Catalog or rendering. A
  failure writes only the versioned JSON error envelope to stdout, publishes no
  destination, and does not alter source/catalog/sidecar state.
- Do not rename existing flags or add aliases. Do not expose libjpeg/libpng/
  LibTIFF numeric constants.

### Studio interaction contract

Replace filter-string inference with one explicit format intent. A practical
implementation is a two-step app-owned options dialog followed by the existing
native save dialog, but an equivalent design is acceptable if it keeps the
same ownership and lifecycle:

1. `Export Photo...` opens an Ravo-owned Qt Quick dialog with a format selector
   and only the controls belonging to that format.
2. Continuing opens the native save dialog with one matching name filter.
3. Accepting the path dispatches one strict command payload containing the
   path, canonical format name, and complete relevant options map.
4. Cancelling either dialog performs no export. Reopening starts from domain
   defaults; stale pending path/options must not survive.

Use an app-owned QML component such as `ExportOptionsDialog.qml`; do not modify
the GeoControls dependency or copy its `QmlFileDialogPage` implementation.

The visible controls are:

- JPEG: quality integer control and a subsampling selector whose labels may
  show `Automatic`, `4:4:4`, `4:4:0`, `4:2:2`, `4:2:0`, while its forwarded
  canonical data remains `auto`, `444`, `440`, `422`, `420`.
- PNG: bit-depth selector and compression integer control.
- TIFF: sample-type selector, compression selector, compression-level integer,
  conditional-grayscale checkbox, and integer DPI control. Compression level
  may be visually disabled when compression is `none`, but the complete typed
  request must still carry the default validated level.
- Original copy: a concise exact-byte explanation and no rendered controls.

The C++ presentation owner must expose or build the canonical choices/defaults;
do not duplicate enum ordinal assumptions or bounds in QML JavaScript. QML may
retain temporary field values while the two dialogs are open, but it must not
parse formats, choose codec defaults, clamp invalid values, or construct domain
objects.

Use one exact command payload schema and reject unknown/missing fields. Suggested
canonical option keys are:

```text
quality
jpegSubsampling
pngBitDepth
pngCompression
tiffSampleType
tiffCompression
tiffCompressionLevel
tiffGrayscaleIfNeutral
tiffResolutionDpi
```

Only the selected format's keys are accepted; original copy accepts none. If a
different strict shape is clearer, freeze it in the ADR and tests rather than
supporting both.

The selected format, not a translated name-filter string or filename suffix,
is authoritative. Normalize paths in C++ with these rules:

- no suffix: append `.jpg`, `.png`, or `.tif` for the selected rendered format;
- JPEG accepts `.jpg`/`.jpeg`; TIFF accepts `.tif`/`.tiff`; PNG accepts `.png`;
- any other or conflicting suffix for a rendered format fails before work is
  posted with a stable `studio_export_extension_mismatch` reason;
- original copy preserves the chosen filename and arbitrary suffix;
- an existing or racing destination still wins through ADR-0032; never add an
  overwrite confirmation that bypasses no-replace publication.

Remove the old locale-sensitive `export_format_from_ui(path, filter)` fallback
once all consumers use the explicit format. Do not retain a compatibility path
that silently chooses PNG for an empty suffix or parses translated filter text.

Dialog controls need keyboard focus order, accessible names, Escape/Cancel,
and a clear default Continue action. Disable Continue while the presentation
values are locally incomplete, but C++ must still reject direct invalid command
payloads. Busy state, window close, selection loss, and command revalidation
must retain the existing controller behavior.

### Localization

All visible QML text uses `qsTr`; visible C++ text uses
`QCoreApplication::translate` or `QT_TRANSLATE_NOOP`. Update translation assets
only through the repository workflow:

```text
python3 .codex/skills/i18n-translation-workflow/run_i18n_workflow.py --part 1
# Fill only newly added <unfinished> values in Ravo/desktop/i18n/zh_translate.ini.
python3 .codex/skills/i18n-translation-workflow/run_i18n_workflow.py --part 2
```

Do not hand-edit either `.ts` file. Preserve placeholders and canonical option
tokens. Require zero active unfinished entries and a successful compiled `.qm`
catalog; an English runtime fallback is not completion.

## Error, cancellation, and resource behavior

- CLI/presenter validation occurs before allocating/rendering product pixels.
  It returns the existing domain error unchanged where one exists; desktop may
  translate its user-facing message but must retain a testable stable reason in
  the private conversion result.
- No parsing exception crosses Qt or the CLI boundary. Unknown QVariant types,
  lossy numeric conversion, fractional integers, non-finite values, integer
  overflow, unknown fields, and missing fields fail closed.
- Once Studio posts the task, use the existing serial executor and shutdown
  token. Do not add a detached task, parallel export queue, nested event loop,
  or mutable dialog state capture.
- Closing the window/catalog or cancellation may return no destination; late UI
  completion must not resurrect dialog state or dispatch another export.
- Preserve input pixels, profiles, metadata snapshot, source file, adjacent
  sidecar, catalog rows, and existing destination on every failure.
- There is no fallback. In particular, invalid 16-bit/high-precision requests
  must not become 8-bit exports, and invalid format/extension pairs must not be
  inferred from another source.

## Tests and acceptance evidence

Add focused tests before relying on manual UI inspection.

### CLI

- Real `catalog export` tests cover defaults and every JPEG subsampling value.
  Independently parse JPEG SOF sampling factors and keep exact
  quality/quantization evidence.
- Real TIFF CLI tests cover 72, 300, and 9600 dpi plus below/above bounds and
  non-integer syntax. Independently parse X/YResolution and ResolutionUnit,
  while retaining all sample/compression/grayscale/high-precision contracts.
- Repeated value flags prove last-value-wins. Duplicate grayscale proves the
  one-shot error.
- Every JPEG/PNG/TIFF option on each wrong rendered format, original copy, and
  at least one non-export subcommand returns the exact structured scope reason
  and creates no output.
- Missing values, unknown canonical names, malformed integers, range errors,
  existing output, and cancellation preserve complete JSON context and source
  hashes.

### Studio/C++ presentation boundary

- Unit-test the pure presentation-map conversion for every default and explicit
  value, exact enum mapping, unknown/missing/non-applicable keys, QVariant type
  mismatch, fractional/overflow numbers, invalid bounds, and input-map
  immutability.
- Unit-test explicit format/path normalization for every accepted extension,
  extension insertion, case handling, original-copy arbitrary suffix, empty
  path, and mismatch failure. No test may depend on localized filter parsing.
- Command-controller tests prove the export action revalidates catalog,
  selection, busy state, and exact payload shape at dispatch time.
- A capturing service/raster boundary or an equivalently direct helper test
  proves the immutable typed values posted by Studio reach one `ExportRequest`.
  Avoid a broad production interface solely for a test double; extract the
  smallest private pure owner if current construction is hard to observe.
- QML contract tests cover every format/control, visible format isolation,
  default reset, one final intent, cancel paths, accessible labels, and no
  codec parsing/default logic in QML. The actual Ravo Studio QML smoke-load
  must still succeed.
- Localization tests require complete English and Simplified Chinese catalogs
  and verify representative new dialog strings in the compiled Chinese `.qm`.

### Regression

- Existing JPEG/PNG/TIFF encoder, Catalog, CLI, metadata, high-precision,
  atomic publication, desktop command, localization, and smoke tests remain
  enabled and unchanged in meaning.
- Default Studio/CLI output for each format is compared with a service request
  using exact domain defaults. Metadata packets remain mandatory and there is
  no new include/exclude switch.
- Original copy remains byte-exact and rejects every rendered option without
  creating metadata or invoking a raster encoder.

## Documentation and migration truth

- Add ADR-0039 (next after ADR-0038) for the explicit export-control intent,
  strict payload/path rule, C++/QML ownership, immutable async snapshot,
  defaults, non-persistence, and rejected localized-filter/fallback designs.
  It extends ADR-0030/0033/0034 rather than silently rewriting them. Update the
  ADR index.
- Update `Ravo/README.md`, `Ravo/ARCHITECTURE.md`, and `Ravo/TESTING.md` with
  current CLI/Studio behavior and tests. Update
  `DevDocs/phase0/capability-inventory.md` if its export row still describes
  implicit Studio defaults.
- Reconcile `Ravo/MIGRATION.md` and `TODO_LEGACY_MIGRATION.md`: remove only the
  completed JPEG-subsampling/TIFF-resolution CLI and explicit Studio-option
  gaps. Keep capture timezone/GPS, sidecars/history, batch presets/path
  templates, TIFF multipage masks, shared consumers, and legacy retirement
  unfinished. Do not claim I11/I12/I13, S9, or J6 complete.
- `DevDocs/ProductRoadmap.md` continues to own batch persistence/presets and
  undecided export workflow; this tranche must not move those capabilities into
  execution.
- Update CLI usage text and any checked command examples in the same change.
  Links and flag names must resolve exactly.

## Current state

- Repository: the current checkout root (the directory containing this prompt)
- Branch: `main`
- HEAD: `2d55cc4 [feat]: embed catalog export metadata`
- The parent branch is four local commits ahead of `origin/main`; it has not
  been pushed.
- At prompt creation, the implementation tree was clean after that commit and
  this hard-cut prompt rename/rewrite was the only intended working-tree diff.
  Preserve unrelated user changes if the state has moved.
- Active source-root mode is `pinned`; `show`, `resolve`, and `verify` pass.
  FreeCM and every dependency seed are clean. No source-root, seed, gitlink,
  lock-template, or dependency update belongs to this task.
- The C14 algorithm queue remains explicitly paused. This independent
  CLI/desktop tranche does not authorize C14 follow-ons, S2.2, C15, or another
  algorithm.
- `source_roots.lock.jsonc`, `CMakePresets.json`, `.freecm/`,
  `build/dependency_*`, build trees, and `userdoc/site/` are ignored local or
  generated state. Never stage or patch them.

## Relevant current owners

Read these directly before editing:

```text
Ravo/domain/include/ravo/domain/types.h
Ravo/domain/src/types.cpp
Ravo/services/src/catalog_service.cpp
Ravo/cli/src/application.cpp
Ravo/desktop/include/ravo/desktop/studio_presenter.h
Ravo/desktop/src/studio_presenter.cpp
Ravo/desktop/src/studio_command_controller.cpp
Ravo/desktop/src/studio_qt.h
Ravo/desktop/qml/Main.qml
Ravo/desktop/qml/chrome/StudioActions.qml
Ravo/desktop/CMakeLists.txt
Ravo/tests/jpeg_cli_test.cpp
Ravo/tests/png_cli_test.cpp
Ravo/tests/tiff_cli_test.cpp
Ravo/tests/studio_command_test.cpp
Ravo/docs/adr/0030-typed-jpeg-export-options.md
Ravo/docs/adr/0032-encoded-byte-publication-contract.md
Ravo/docs/adr/0033-typed-png-export-options.md
Ravo/docs/adr/0034-typed-tiff-export-options.md
Ravo/docs/adr/0037-high-precision-export-pixel-contract.md
Ravo/docs/adr/0038-embedded-export-metadata.md
```

The frozen JPEG/PNG/TIFF output owners may be read statically only to confirm
labels/defaults. Do not configure, build, or run `legacy/`:

```text
legacy/src/imageio/format/jpeg.c
legacy/src/imageio/format/png.c
legacy/src/imageio/format/tiff.c
```

GeoControls' materialized/seed `QmlFileDialogPage` may be read as API evidence
only. Do not modify either dependency checkout or copy its implementation.

## Validation baseline and final gate

Immediately before this prompt was written, the following passed on
macOS/Apple Silicon for `2d55cc4`:

```text
python3 configs/source_roots.py show --format json
python3 configs/source_roots.py resolve --format json
python3 configs/source_roots.py verify
cmake --preset mac_clang_debug -DBUILD_TESTING=ON
cmake --build --preset mac_clang_debug
ctest --test-dir build/mac_clang_debug --output-on-failure
python3 -m mkdocs build -f userdoc/mkdocs.yml --strict
git diff --check
```

The full Ravo suite passed 434/434. The strict documentation build passed with
only the external Material for MkDocs warning about its future MkDocs 2.0
project. Focused post-fix metadata tests passed 54/54, and JPEG/PNG/TIFF output
was independently read by Exiv2; TIFF was also read by `tiffinfo` and all XMP
packets by `xmllint`. Windows and Linux were not run and must not be reported as
passing. No fallback was added.

Minimum final validation for this public CLI/desktop tranche is:

```text
python3 configs/source_roots.py show --format json
python3 configs/source_roots.py resolve --format json
python3 configs/source_roots.py verify
python3 .codex/skills/i18n-translation-workflow/run_i18n_workflow.py --part 1
# Complete only new <unfinished> zh_translate.ini values.
python3 .codex/skills/i18n-translation-workflow/run_i18n_workflow.py --part 2
cmake --preset mac_clang_debug -DBUILD_TESTING=ON
cmake --build --preset mac_clang_debug
ctest --test-dir build/mac_clang_debug --output-on-failure --parallel 4
python3 -m mkdocs build -f userdoc/mkdocs.yml --strict
git diff --check
git diff --stat
```

Run clang-format dry-run on every changed C/C++ source/header. If QML formatting
is configured through the repository hook, use that owner; do not invent a
different formatter. Build every additional host toolchain actually available,
otherwise report Windows/Linux as untested. An optional manual Studio dialog
smoke is useful but does not replace the C++/QML/service contracts.

## Execution order

1. Inspect branch/worktree/source roots and read both `AGENTS.md` files plus the
   linked authorities and ADRs. Post a short pre-edit plan naming value
   ownership, dialog/task lifecycle, validation/error/cancellation paths, and
   the smallest test set.
2. Add pure CLI and desktop conversion/path tests first. Freeze flag names,
   strict payload keys, defaults, format isolation, extension policy, and error
   reasons before changing QML.
3. Implement CLI subsampling/DPI parsing and format-scope hardening using the
   existing domain parsers/validators. Add real JPEG/TIFF CLI evidence.
4. Implement the desktop-private strict conversion owner and change the
   command/presenter boundary to accept an explicit format plus complete
   options. Capture typed values before posting the existing task.
5. Add the app-owned options dialog and two-step save interaction. Keep QML as
   presentation/intent only, reset state on every open, and cover cancel,
   focus/accessibility, and busy/selection revalidation.
6. Run the i18n workflow, translate only new unfinished memory values, compile
   both catalogs, and test representative Chinese output.
7. Add ADR-0039 and reconcile stable docs/TODO without claiming blocked export
   or legacy-retirement work. Run the complete validation gate and inspect the
   full diff.

## Do not do

- Do not commit, amend, rebase, or push without an explicit user request.
- Do not modify, configure, build, or execute `legacy/`; it is static evidence
  only. Do not delete JPEG/PNG/TIFF output owners or registrations.
- Do not modify FreeCM, GeoControls, dependency seeds/source roots,
  `source_roots.lock.jsonc.in`, the active ignored lock, generated presets, or
  `build/dependency_*`.
- Do not start C14 follow-on work, S2.2, C15, masks, GPU, or another algorithm.
- Do not add codec logic, duplicate typed option values, infer format from a
  localized filter, silently clamp UI/CLI input, or retain a hidden fallback.
- Do not add overwrite, metadata include/exclude, sidecar, pHYs, multipage mask,
  resize/preset/batch/path-template, or settings-persistence behavior.
- Do not hand-edit generated `.ts` files or leave active unfinished
  translations.
- Do not weaken existing pixels, profile, metadata, precision, cancellation,
  publication, localization, or resource-destruction tests.
