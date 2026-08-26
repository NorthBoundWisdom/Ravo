# Ravo Architecture

## Core conclusion

Ravo's current first product is a usable local photo browser: create or open an
SQLite catalog, import JPEG/PNG/TIFF/RAW by reference, and view images in Ravo
Studio. The `ravo` CLI remains a supported headless client. CLI and desktop
must call the same application services and engine; they must not own separate
catalog, import, preview, or recipe implementations.

```text
ravo CLI ───────────────┐
                        ▼
                 Application Services ◀────────── Ravo Studio
                 │ create/open                    │ Gallery
                 │ import/list                    │ viewer
                 │ request/cancel preview         │ visible errors
                 ├───────────────┐
                 ▼               ▼
           Catalog Domain     Ravo Engine Facade
                 ▲               ▲
                 │ implements    │ implements
          SQLite/FS Adapter   RAW/Raster/Cache Adapters

Frozen 0.9 legacy/src/ ──read-only source and fixture evidence──▶ Ravo tests
Frozen 0.9 legacy/src/ ╳────────────────────────────────────────▶ Ravo production
```

[ADR-0007](docs/adr/0007-first-usable-catalog-viewer.md) accepts this order. It
validates catalog, import, preview, task, and window lifecycles early without
restoring the old GTK, dynamic IOP ABI, or global state.

## Targets and dependency direction

| Target | Ownership | Allowed dependencies | Forbidden dependencies |
| --- | --- | --- | --- |
| `ravo_foundation` | errors, IDs, cancellation, basic resource contracts | standard library, QtCore where needed | recipe, engine, catalog, UI |
| `ravo_recipe` | recipes, operation schema, version upgrades | foundation, QtCore where needed | codec, database, UI |
| `ravo_engine` | inspect, operation registry, CPU render/preview | foundation, recipe, engine ports, QtCore where needed | catalog, services, CLI, UI, old `src` |
| `ravo_domain` | Asset/Catalog, Import/Preview state, repository ports | foundation | SQLite, codec, engine-private types, UI |
| `ravo_services` | create/open/import/list/preview use cases and task orchestration | domain, engine facade | SQL, QML/presentation types, third-party codec types |
| `ravo_adapters` | SQLite, filesystem, RAW/raster codecs, preview cache | matching ports, Qt Core/Gui/Sql, pinned third-party dependencies | QML/UI state, old core |
| `ravo_cli` | arguments, JSON, exit codes, CLI composition | services, engine facade, adapters | algorithms, SQL, UI |
| `ravo_desktop` | C++ composition/presenters, Qt Quick/QML window, Gallery, viewer, file selection; presenters split by catalog/preview/develop and QML by `theme/`, `gallery/`, `inspect/`, and `chrome/` | services, read-only preview resources, Qt Core/Gui/Qml/Quick, GeoControls | Qt Widgets, SQL, codecs, algorithm-private state |

Private Qt Sql/QSQLITE and `QImageReader` adapters contain SQLite and the
first raster path. LibRaw, LittleCMS, and platform APIs likewise do not cross a
port. LittleCMS is linked only by `ravo_engine`; public colour state owns ICC
bytes or matrices instead of third-party handles. Qt value types may be used
inside a target with a clear benefit, but recipes, CLI JSON, catalog schema,
and public persisted contracts must not serialize Qt/C++ object memory layout.

Ravo Studio has one presentation architecture. Its C++ composition root owns
services, tasks, and `QQmlApplicationEngine`; desktop-owned QObject
presenters/models map immutable service snapshots and commands to QML.
QML/JavaScript owns transient view state, layout, bindings, and input only; it
does not implement catalog/import/preview business rules. A desktop-owned C++
registry/controller owns commands. Each command has a stable ID, strict
parameters, runtime state, and a handler, while action contributions project it
to menus, shortcuts, context menus, controls, and the command palette. Every
entry rechecks context at execution, disabled state has a visible reason, and
the controller issues dialog presentation requests that QML displays and returns.
Commands that depend on current control values are not in the command palette.
QML retains only transient focus/popup state and thin action binding, not a
second table of IDs, titles, shortcuts, or enablement.

Desktop localization is likewise presentation-only. The desktop-owned language
manager selects only en_US or zh_CN, persists that UI preference locally, and
owns the QTranslator lifetime. It installs a verified candidate before
discarding the prior translator, then asks the QML engine and command controller
to retranslate. Source TS catalogs and the Chinese translation memory are
repository assets; CMake validates and compiles them to build-local QM files,
which are the only files deployed with Studio. Missing Chinese artifacts are
reported and leave the active language unchanged; no catalog, service, task, or
engine state is translated or altered.

Gallery grid schedules only `kThumbnailMaxEdge` browse thumbnails, never a
1600px processed preview merely for a selected grid item; histogram and parade
are calculated from that thumbnail. Loupe/develop requests full decode. On
open/import, presenters seed ready cache paths from the preview table so grid
delegates do not saturate a single-thread queue. The Library panel separately
shows import and preview-build progress. After system file/folder selection,
scanning and import run on workers and each successful photo immediately
appears in the grid with a browse thumbnail.

Grid cells fit available width in the 120–320 range and have a vertical scroll
bar. `positionViewAtIndex` runs only when the selected item leaves the
viewport. C++ passes `--catalog <library.sqlite>` to the presenter; QML opens
it at session start rather than a default library.

Develop preview is bounded and coalesced: at most one render is in flight, plus
one recipe waiting to save and one preview request waiting. A new revision
cancels the old token; stale results are dropped by revision and asset, while
failure retains the prior verified preview. During a drag, the presenter only
forwards in-memory parameters. Services apply effects to cached scene-linear
working images at `kInteractivePreviewMaxEdge`, return memory pixels, and do
not write PNG/cache. CatalogService caches RAW unpack and demosaic, invalidating
them when highlight reconstruction changes; it must not fall back to embedded
JPEG. Save and request a complete preview after release.

Develop crop is interactive: crop-tool preview removes crop and straighten,
while Qt Quick rotates the working image. Photo and overlay share the GPU
transform; the crop frame remains screen-axis aligned and inscribed in the
rotated image. Selection and Angle dragging change in-memory parameters only;
release writes the recipe. Export continues to use CPU straighten, not an
engine GPU adapter. Deleting a photo normally removes only the catalog record
and preview cache. The explicit “Delete from Disk” command deletes the source
after confirmation, then removes the catalog record. QML resources are built
and deployed with `qt_add_qml_module`; the first version links no Qt Widgets
and has no hybrid fallback.

## Core data contracts

### Catalog

A catalog is one user-selected SQLite file. Schema v1 stores at least:

- schema version and migration metadata;
- stable asset ID, normalized local URI, media type, source size/mtime/optional
  fingerprint;
- dimensions, orientation, basic capture metadata, import state, and structured
  error summary;
- preview contract version, cache key, dimensions, state, and latest success.

Normalized URIs are unique within a catalog. The database stores neither source
files nor full preview blobs, nor presentation state, recipe object layout,
codec handles, or database-row addresses. Schema v2 stores rating/color/reject
on `asset`; v3 stores at most one canonical recipe JSON per image in
`asset_recipe`; v4 adds `asset_tag`, `asset_metadata` (read-only capture
EXIF plus catalog-only writable fields), and `asset_recipe_history`
(history/snapshot). The SQLite adapter enables WAL, `synchronous=NORMAL`, and
`busy_timeout`. New catalogs and migrations use transactions, and an unknown
higher schema version fails fast.

The repository adapter atomically publishes the current recipe row (or baseline
clearing), automatic history, and catalog revision. Any failed step rolls back
everything; services neither compensate writes nor expose a partially committed
recipe. The first version does not read or migrate a frozen 0.9 catalog. Future
compatibility requires an independent product decision, backup/rollback, and
fixtures.

### Import

`ImportRequest` carries catalog ID, file/directory input, recursion/format
policy, resource budget, cancellation token, and correlation ID.
`ImportItemResult` returns imported, duplicate, unsupported, or failed for
each input; partial success must not lose failure detail.

First-version import only registers sources: it never copies, moves, renames,
rewrites metadata, or deletes them. Codec probing confirms a format; extensions
are only candidate filters. One LibRaw open reads RAW capture metadata and
embedded JPEG, then persists a `kThumbnailMaxEdge` browse thumbnail; import
does not perform a 1600px full decode. It validates trusted metadata before
transactionally publishing an asset. Cancellation stops undispatched work;
committed results remain valid.

### Preview

`PreviewRequest` explicitly carries asset ID, target pixel dimensions,
orientation/colour policy, backend, memory/thread budget, cancellation token,
and request revision. `PreviewResult` returns a trusted read-only preview
resource or structured failure.

RAW uses the Ravo CPU engine, while JPEG/PNG/TIFF use the raster adapter. Both
share orientation, colour, alpha, scaling, finite-value, and error contracts.
Preview cache is atomically written outside the database, keyed by source
fingerprint, target dimensions, and contract version. Corrupt or missing cache
rebuilds from the read-only source.

Import and Gallery use browse cache. One LibRaw open reads RAW metadata and
embedded JPEG, then writes a PNG at `kThumbnailMaxEdge` under the
`embedded-jpeg` key digest. It is not editable scene-linear data. Loupe,
Develop, scopes, export, and `request_preview` with
`prefer_embedded_preview=false` use preview contract v6: full CPU
decode/render followed by the `ravo.display.sigmoid` baseline at the end of
the scene-linear buffer. The cache types must not share a digest. Without
embedded JPEG, browse fails open to full decode and never writes an empty image.
Cached PNGs contain one standard `sRGB` chunk; v6 rebuilds prior cache output.
The baseline creates no `asset_recipe` row and does not set `has_edits`;
persistence begins only after a user override. Existing JPEG/PNG/TIFF are
display-referred input and do not receive Sigmoid twice. Import persists
thumbnail dimensions only; loupe creates 1600px on demand.

The Qt raster adapter accepts PNG/JPEG/BMP/GIF/WebP/TIFF and exports
PNG/JPEG/TIFF. JPEG/GIF/WebP/TIFF plugin targets and QSQLITE are configure-time
requirements. TGA/WBMP/ICO and other SQL drivers have no product consumers.

### Recipe and operation

Canonical recipes, operation descriptors, `RenderRequest`/`RenderResult`,
and the explicit colour contract remain valid. The first viewer needs only the
minimal CPU chain that yields a trusted preview; later editing UI maps the
versioned schema only and owns neither a second algorithm nor history format.

`ravo.core.tonecurve` implements the frozen C RGB-linked default:
Lab D50 → ProPhoto, `preserve_colors=average`, a 0–1 point list, and
`interpolation=monotone_hermite`. `working_space=lab|xyz|lab_independent`
is an explicit C mode; Inspector forwards points and recipe/engine evaluates.

`ravo.display.sigmoid` v1 is the sole default display transform:
`working_space=linear_srgb`, `color_processing=per_channel`, middle-grey
contrast, skew, Standard SDR black/white target, and hue preservation. It is
the RAW baseline and final scene-referred operation before sRGB encoding. RAW
Studio Contrast belongs to Sigmoid; `ravo.core.contrast` serves
display-referred raster input and old recipes. Highlights/shadows/whites/blacks
are scene controls before the transform.

RAW may execute `ravo.raw.highlights` before demosaic; default denoise, lens
correction, dt UCS `colorequal`, graduated filter, and nine-band toneequal
use the same recipe/engine. `ravo.raw.hotpixels` processes an owned
`DecodedRaw` copy before highlights. Both complete before demosaic and are
disabled before RGB recipe. The scene-linear preprocess cache key includes all
CFA-operation parameters and order; recipe mutation never contaminates raw
decoded cache. The current hot-pixel sensor contract accepts complete Bayer 2×2
CFA only. `ravo.raw.cacorrect` runs Bayer tile statistics, full-image
polynomial shift fit, and optional avoid-color-shift on the same copy before
demosaic. The RAW memory estimate includes owned CFA, float scratch, fitting,
and avoid-shift factor; low budget fails before allocation or pixel publication.

`ravo.color.temperature` v1 fixes `camera_cfa_or_linear_rgb` and
`channel_scale_v4`. It normally parses four-channel coefficients from LibRaw
as-shot metadata in `DecodedRaw`, and may use camera-reference, explicit
manual, or as-shot-to-reference. The engine resolves one temperature intent at
RAW preprocess; cacorrect and demosaic consume the same coefficients. Demosaic
scales each CFA sample by its own channel before the camera-to-working matrix.
Temperature is in the scene-linear preprocess cache key; source `DecodedRaw`
is immutable. Late reference shares no global chroma state and must be followed
by explicit non-RGB `channelmixerrgb` adaptation; raster accepts explicit
coefficients only.

Canonical recipe schema v2 upgrades schema-v1 recipes by inserting one
explicit source → linear Rec709 `ravo.color.input` operation. That operation
follows RAW preprocessing and owns input-profile to working-profile conversion.
Decode publishes immutable `ColorProfileState`:
RAW supplies an enhanced camera-to-XYZ D50 matrix and raster supplies a valid
embedded ICC/built-in profile or an explicit missing state. The engine-private
adapter retains the frozen matrix-only, 65,536-sample shaper plus unbounded
extrapolation, target-gamut clipping, RAW blue mapping, and general LittleCMS
RGB/XYZ/Lab paths. File ICC content and canonical profile parameters enter the
scene-linear/preview cache key. Missing/corrupt profiles, unavailable matrix
kinds, singular/non-finite transforms, and untagged raster inputs fail before
publication; there is no sRGB or generic-camera fallback. `LinearWorkingBuffer`
carries its working-profile matrix. Existing operations are explicitly bridged
to their declared linear Rec709/sRGB workspaces. Output remains declared sRGB
until `ravo.color.output` adds selectable export profiles.

`ravo.color.channelmixerrgb` v1 fixes the `linear_srgb_d50` workspace and
V3 algorithm, persisting three mixing rows, saturation/lightness/grey,
normalization, adaptation, illuminant xy, gamut, and clip. Studio normally
edits only the `adaptation=rgb` 3×3 matrix; CAT16/Bradford/XYZ are selected
only through explicit canonical parameters and never hidden camera/ICC state.

`ravo.color.colorbalancergb` v1 also explicitly declares
`linear_srgb_d50`. It stores four Y/C/H zones, three falloff/fulcrum values,
chroma/saturation/brilliance, hue/vibrance/contrast, and formula. The engine
derives a render-lifetime gamut LUT and executes CAT16 D65, CIE 2006 LMS,
Filmlight Yrg/Ych, and DT UCS by default or explicit JzAzBz. It writes pixels
to an owned output buffer and publishes only after every row succeeds. Studio
projects canonical values into a read-only parameter map; QML owns no mask,
LUT, or colour mathematics. The former three-parameter
`ravo.color.colorbalance` approximation is hard-deleted; frozen
`colorbalance.c` remains a separate later capability.

Every decode/preview/render boundary carries explicit pixel format, alpha,
source/target colour description, and profile state. UI, file name, or unmarked
buffer must not implicitly select colour strategy. See
[ADR-0006](docs/adr/0006-explicit-colour-contract.md).

## Services

First-version services provide:

- `CreateCatalog` / `OpenCatalog`: create, validate, migrate, and return an
  immutable catalog snapshot;
- `ImportAssets`: enumerate inputs, call codec/engine, transactionally publish
  assets, and schedule preview;
- `ListAssets` / `ObserveCatalog`: return stable order and revision without
  SQL cursors;
- `RequestPreview` / `CancelPreview`: request bounded previews by viewport
  and discard stale results;
- `CloseCatalog`: stop associated tasks and release connection/cache handles
  before completion.

CLI and desktop composition roots may use different presenters, but must
assemble the same services, ports, and adapters.

## Ownership, lifecycle, and threads

- Composition creates catalog repository, codecs, engine, cache, executor,
  services, and clients, then destroys them in reverse order after tasks stop.
- The UI main thread owns `QQmlApplicationEngine`, windows, and desktop
  presentation state. Scanning, metadata, decode, preview, cache, and database
  I/O run only in C++ owner-managed tasks; detached threads are prohibited.
- Catalog, asset, recipe, descriptor, and cross-thread results are immutable
  snapshots; writes make a new revision.
- Database connections are never bare-shared across threads. Adapters use an
  explicit serial owner or a controlled connection per worker and finish or
  roll back transactions before close.
- Pixel buffers and preview temporary files have one write owner; read-only
  sharing binds a resource handle and clear validity period.
- Every asynchronous completion carries catalog/asset/request revision; results
  after cancellation, catalog close, or selection change are discarded.
- Codec, database, cache, or memory failure retries only from trusted input or
  returns failure; it must not display partial output.
- Originals stay read-only. Database and previews become visible only after
  transaction or atomic publication succeeds.

## Desktop boundary

The Ravo Studio first version owns:

- creating/opening catalogs and choosing files/directories;
- Gallery list states: loading, ready, missing, unsupported, and failed;
- selection, Gallery grid/loupe and Edit panes, fit, 100%, and pan. Grid and
  filmstrip use whole-image containment with letterbox number, rating,
  format/dimension, and flag overlays;
- shared scopes above the right Gallery/Edit panel: frozen-C 256-bin RGB
  histogram (linear Y) and RGB-parade component plot;
- progress, cancellation, and recoverable-error presentation;
- window, focus, keyboard, HiDPI, and basic accessibility.

QML sends only intents to desktop-owned C++ presenters and observes immutable,
revisioned view state. Visible controls use GeoControls (buttons, labels, list
items, segmented switches, status bar, and file dialog). Gallery/`Image`
consumes controlled preview resources only and never opens an original directly.
QML contains no SQL, file enumeration, codec probing, task scheduling, or
services-duplicated business state machine. GeoControls has no folder picker, so
`FolderDialogPage.qml` wraps `FolderDialog` under the same dialog contract.

Ravo Studio does not own SQL, file enumeration, codec probing, RAW processing,
preview cache, recipe evaluation, bare engine/SQLite/LibRaw objects, shell
launch of `ravo`, old GTK widgets, a Qt Widgets fallback, old config keys, or
dynamic IOP lifecycle.

## CLI boundary

CLI owns arguments, stdin/stdout, versioned JSON, stable exit codes, and
headless composition. Existing catalog create/import/list/preview/develop/export
commands validate the same services; human logs must not pollute JSON stdout.

## Current non-goals

- CatalogService owns local JPEG/PNG/TIFF/original-copy export; encoded pixel
  exports declare sRGB. Selectable output profiles, complete metadata, batch
  jobs, and old export presets remain out of scope.
- The first version does not implement full history/styles, mask/blend, every
  operation, or old-catalog migration.
- Do not implement GPU before CPU correctness and viewer resource gates.
- Do not freeze APIs for networks, cloud sync, public plugin ABI, or a complex
  query language without consumers.
- Do not modify frozen 0.9 to call Ravo or let Ravo production call the frozen
  application.
