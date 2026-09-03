# Ravo Architecture

## Core conclusion

Ravo is one local photo-management and non-destructive editing product: create
or open an SQLite catalog, import JPEG/PNG/TIFF/RAW by reference, browse and
review, develop, recover/backup, and export. The `ravo` CLI and Ravo Studio are
supported clients of the same application services and Engine; neither owns a
separate catalog, import, preview, recipe, or render implementation.

```text
ravo CLI ───────────────┐
                        ▼
                 Application Services ◀────────── Ravo Studio
                 │ create/open                    │ Gallery
                 │ import/page/relink             │ viewer
                 │ recovery/backup/restore        │ visible errors
                 │ develop/preview/export          │ Develop
                 ├───────────────┐
                 ▼               ▼
           Catalog Domain     Ravo Engine Facade
                 ▲               ▲
                 │ implements    │ implements
       SQLite/Recovery/FS     RAW/Raster/Cache Adapters

Frozen fixtures in Ravo/tests/fixtures/frozen ──static evidence──▶ Ravo tests
Leftover darktable source ╳──────────────────────────────────────▶ Ravo production
```

[ADR-0007](adr/0007-first-usable-catalog-viewer.md) established the initial
vertical-slice order; later ADRs extend the same owners through Develop,
recovery, paging, and export without restoring the old GTK, dynamic IOP ABI, or
global state.

## Targets and dependency direction

| Target | Ownership | Allowed dependencies | Forbidden dependencies |
| --- | --- | --- | --- |
| `ravo_foundation` | errors, IDs, cancellation, basic resource contracts | standard library, QtCore where needed | recipe, engine, catalog, UI |
| `ravo_recipe` | recipes, operation schema, version upgrades | foundation, QtCore where needed | codec, database, UI |
| `ravo_engine` | inspect, operation registry, CPU render/preview, Engine-owned GPU adapter, offline numeric fitting | foundation resource types, recipe, engine ports, QtCore, Qt Gui/GuiPrivate for the private QRhi GPU adapter | catalog, services, CLI, UI, old `src`, OpenCL |
| `ravo_domain` | Asset/Catalog, Import/Preview state, repository ports | foundation | SQLite, codec, engine-private types, UI |
| `ravo_services` | create/open/import/list/preview use cases and task orchestration | domain, engine facade; adapters for filesystem text/hash and CRS interchange helpers (implementation .cpp only — public headers must not include `ravo/adapters`) | SQL, QML/presentation types, third-party codec types; adapters types in public headers |
| `ravo_adapters` | SQLite, filesystem, RAW/raster codecs, preview cache | matching ports, Qt Core/Gui/Sql, pinned third-party dependencies | QML/UI state, old core |
| `ravo_control` | transport-neutral live-session protocol plus bounded same-user local discovery/transport | foundation, Qt Core/Network | catalog, recipe, engine, services, UI state |
| `ravo_cli` | arguments, versioned JSON, exit codes, catalog composition, local-control client | services, engine facade, adapters, control | algorithms, SQL, QML/UI state |
| `ravo_desktop` | C++ composition/presenters, live-session snapshot/dispatch, Qt Quick/QML window, Gallery, viewer, file selection; presenters split by catalog/preview/develop and QML by `theme/`, `gallery/`, `inspect/`, and `chrome/` | services, control, read-only preview resources, Qt Core/Gui/Network/Qml/Quick, GeoControls | Qt Widgets, SQL, codecs, algorithm-private state |

Private Qt Sql/QSQLITE and `QImageReader` adapters contain SQLite and the
first raster path. LibRaw, LittleCMS, Exiv2, and platform APIs likewise do not
cross a port. LittleCMS and Exiv2 are linked only by `ravo_engine`; public
colour and exposure-analysis state owns ICC bytes, matrices, or metadata values
instead of third-party handles. Qt value types may be used inside a target with
a clear benefit, but recipes, CLI JSON, catalog schema, and public persisted
contracts must not serialize Qt/C++ object memory layout.

Exiv2, LensFun, LibJpegTurbo, LibTIFF, and RawSpeed are pinned migration
source roots. Configure validates the exact materialized sources. The accepted
engine-private RAW metadata adapter is the only Exiv2 consumer; other roots do
not link a product target until their corresponding lens-database or codec
adapter is accepted. RawSpeed remains an unused I3 input and must not silently
replace or fall back to LibRaw. Ravo does not expose Exiv2's upstream CMake options.
`ravo_add_private_exiv2()` compiles one frozen private profile:

| Surface | Frozen value |
| --- | --- |
| Linkage | static `exiv2lib`, `EXCLUDE_FROM_ALL` |
| Read path | filesystem `ImageFactory::open`, plus in-memory TIFF from PNG `eXIf` |
| Containers | BMFF on (CR3); Exiv2 XMP SDK, PNG codec, video, webready/curl, Brotli, NLS, inih, and Nikon lens data off |
| Extra targets | CLI, samples, unit/fuzz tests, and docs off |
| Apple host | Force-include `<ctime>` on `exiv2lib` so pinned `value.cpp` can use `std::mktime` |

Public configure still has only Ravo `BUILD_TESTING`. That flag never enables
Exiv2 tests or docs.

Future public contracts continue to carry owned metadata,
calibration, or encoded bytes rather than third-party handles. Existing
QSQLITE, zlib/libpng PNG input, and Qt JPEG/TIFF input runtime ownership is
unchanged. The adapter-private libpng/ZLIB encoder owns PNG output bit depth
and compression, opaque non-interlaced RGB8/RGB16, ICC/recognized built-in
cICP, bounds, and cancellation. Product PNG16 requests consume engine-owned
host-endian RGB16; an RGB8 source still rejects a 16-bit request rather than
expanding packed samples. The adapter-private
pinned libjpeg-turbo encoder owns JPEG output quality, explicit chroma
subsampling, ICC APP2,
bounds, and cancellation. The adapter-private pinned LibTIFF/ZLIB encoder owns
TIFF output sample/compression/level, 72–9600 inch resolution, conditional
grayscale, exact ICC, bounded baseline main-directory metadata, and
cancellation. The shared prepared-metadata path supplies bounded Exif/XMP/IPTC
plus capture time/offset/GPS; multipage output and sidecar/history/privacy
policy remain separate later contracts. Domain `ExportMetadataSnapshot`
carries only owned destination and metadata values, never third-party handles.

Ravo Studio has one presentation architecture. Its C++ composition root owns
services, tasks, the live-session endpoint, and `QQmlApplicationEngine`; desktop-owned QObject
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

Desktop localization is likewise presentation-only. One versioned locale
manifest owns supported locale codes, native display names, aliases, catalogs,
and translation memories. The desktop-owned language manager parses that
embedded manifest on the main thread, persists the normalized UI preference
locally, and owns the QTranslator lifetime. It installs a verified candidate before
discarding the prior translator, synchronously persists a normalized value,
then asks the QML engine and command controller to retranslate. A malformed
stored value is removed and falls back to English; a package or settings-write
failure is reported and leaves active state unchanged. Source TS catalogs and
locale-specific translation memories are repository assets; CMake derives its
complete catalog set from the same manifest, validates it, and compiles it to
build-local QM files, which are the only files deployed with
Studio. Typed desktop settings are the UI language, assistant
endpoint/model/key, and window size/position/maximized state. Corrupt stored
values are removed synchronously; a settings-write failure is reported and
leaves prior durable state unchanged except that a live resized window stays
where the user put it. View controls such as zoom, pan, browse mode, and
library filters remain session state, and catalog, service, recipe, export,
task, and engine values stay in typed owning contracts. No old configuration
key is read (ADR-0066/0081/0115).

Gallery grid schedules only `kThumbnailMaxEdge` browse thumbnails, never a
1600px processed preview merely for a selected grid item; all scopes are
calculated from that thumbnail. Loupe/develop requests full decode. On
open/import, presenters seed ready cache paths from the preview table. A cold
cache starts only from active GridView/filmstrip delegates plus one GridView
row of look-ahead: desktop C++ deduplicates the bounded demand set, holds at
most one background thumbnail request in flight, and lets newer viewport
demand lead older queued demand. It does not prefill every asset in a loaded
page. Foreground Develop cancels the active browse token; the same owner
retries still-resident demand after foreground work and rejects results after
page eviction, catalog replacement, or close.

While the first exact loupe or Develop result is pending, QML may display the
selected asset's verified browse-thumbnail URL as an explicit loading layer.
The presenter seeds a stable expected viewport extent from the selected asset,
and the accepted exact result replaces it. The loading layer never changes
`previewUrl`, owned preview pixels, scopes, live-session hashes, crop/white-
balance interaction, recipe state, cache identity, or export input. Missing or
failed originals do not use the layer. The Library panel separately shows
import and demanded-preview progress. After system file/folder selection,
Studio enumerates the source on a worker, publishes named placeholder cells
immediately, then inspects and fills bounded 320-pixel workspace thumbnails as
the grid demands them. Catalog import still runs on workers. Clicking Import publishes named Gallery
placeholders immediately; each cataloged photo then fills that cell, and
viewport demand loads a browse thumbnail so the user can inspect while later
items and selected previews continue.

Gallery and import-workspace grid cells fit available width in the 120–320
range and have a vertical scroll bar. `positionViewAtIndex` runs only when the
selected Gallery item leaves the viewport. Import cells use a separate highlight
from the import checkbox: Command/Control extends the highlight, Shift selects a
range, Command/Control+A highlights all, and the checkbox applies its new state
to every highlighted eligible cell. C++ passes `--catalog <library.sqlite>` to the presenter; QML opens
it at session start rather than a default library.

Library filtering is a value boundary, not UI-built SQL. `LibraryQuery` owns
validated review, folder/tag, text/media/edit, camera/capture/numeric ranges
and stable sort state; CatalogService rejects an invalid query before loading
and delegates matching/sorting to domain. Studio keeps only the active query,
reloads the visible model through the service, and forwards changes through
the command registry. It intentionally records no recent-query history. QML
never sees catalog columns or legacy rule strings.

Library listing is additionally a bounded page boundary. `LibraryPageRequest`
owns validated query, optional stack collapse (default on), offset, a limit no
greater than 512, optional keyset cursor, and an optional known total. `LibraryPage` returns only that page plus
total/next-cursor, materialized-row count, and database elapsed time. Ordinary
sequential traversal uses the stable sort key and asset ID cursor; an explicit
viewport jump may use offset. The adapter attaches tags, metadata, and preview
records only for the current page. Studio exposes the full logical row count
through a sparse model with at most three resident 200-row pages, while QML
delegates request unloaded rows and one row of thumbnail look-ahead. The C++
demand queue is bounded by those resident pages rather than total catalog size;
only one request enters the serial executor at a time. Selection is asset-ID
based and protects its page from eviction (ADR-0100).

Reusable style/preset state reuses Recipe rather than introducing another
parameter model. Schema-v1 `RecipeStyle` replaces the asset with a fixed
template identity; complete application restores only the target identity.
Schema v2 additionally owns a sorted unique list of stable logical Develop
fields. Selective application decodes the template and current target through
the same `DevelopParams` owner, copies only those fields, merges required mask
nodes, rebuilds canonical operation order, and then enters the same Engine
validation, Develop, Catalog history, undo, preview, and export lifecycle.
Studio C++ derives candidates from the current product baseline, revalidates a
submitted subset, and atomically writes it into the library-adjacent
`Ravo Presets` folder; QML starts every checkbox clear and forwards only the
name and selected field IDs. Filesystem adapters own bounded reads and
atomic complete writes with explicit pre-existing-path conflict
(ADR-0065/0098).

The Studio session clipboard reuses that same Recipe-owned selection and merge
contract without serializing a file. Desktop C++ snapshots `DevelopParams` plus
the explicit selected field IDs; paste merges them into the current target and
then uses the normal Develop commit lifecycle. QML uses the same selection
component as preset saving, starts every checkbox clear, and owns neither the
clipboard nor merge policy. The former complete clipboard and fixed Light/Color
paste paths have no product consumer (ADR-0078/0098).

Develop preview is bounded and coalesced: at most one render is in flight, plus
one recipe waiting to save and one latest preview request waiting. Repeated
pure-interactive intents do not cancel the frame already in its pixel stage:
that monotonically newer result may publish while the latest parameters remain
queued, and `displayed_develop_` keeps live control from claiming it matches the
current recipe. The latest request starts immediately afterwards. Save,
selection, catalog-close, comparison, and non-interactive supersession still
cancel the active token and reject late results by revision and asset. During a
drag, the presenter only forwards in-memory parameters. Services apply the
complete effect stack to a cached 960px scene-linear working image, return
memory pixels, and do not write PNG/cache. That 960px buffer is a box-filtered
copy of the 1600px settled linear working, so entering Develop demosaics once.
Actual Size 1:1 is a Bayer CFA window of the visible crop (ADR-0132) demosaiced
on GPU RCD when a compute backend exists. Live Develop parameter changes
re-request that window; they do not wait for a pan. The CFA-window linear
working is retained across RGB-only edits so sliders do not remosaic. Lens,
perspective, and full-frame ROIs reject and keep the 1600 preview. GPU is an
Engine-owned QRhi adapter (ADR-0133/0134): one process-wide device. Unmasked
Exposure, light controls, Lab USM Sharpen, and Sigmoid run on the GPU during
preview in one SSBO session, interleaved with remaining CPU RGB ops. Masked
ops, other RGB kernels, and export stay on CPU. Failures are
fail-closed. Recipe, Catalog, CLI, and QML do not hold device objects.
`catalog probe --json` reports `gpu_backend`.
For an ordinary commit whose parameters are not
already displayed, Studio first saves
atomically, publishes that 960px memory preview, then queues the same
revision for an exact 1600px persisted preview. Foreground Develop and
background Gallery work own separate bounded decode/working lanes; within the
foreground lane, one linear-working slot is retained for each size class. A
new decoded source, incompatible preprocess key, close, or destruction
invalidates only the applicable lane. The 960px foreground slot additionally
owns one exact serialized pre-light operation-prefix buffer and one reusable
row team. Prefix identity includes operation order, parameters, enabled state,
masks, and asset recipe identity; the containing working slot binds source,
dimensions, profile/preprocess state, and lifetime. A changed prefix replaces
the prior value only after successful completion. The 1600px and Gallery lanes
do not retain this state. Active background preview work is
cancelled for Develop, and queued foreground work leads normal work while
preserving FIFO order within each priority. It must not fall back to embedded
JPEG. CPU pixel rows use deterministic static partitions with at most 16
workers and caller participation; cancellation is checked per row and
worker-start failure is structured rather than falling back silently
(ADR-0087/0089). Studio posts the pixel job before broadcasting live edit
property changes, so QML binding reevaluation can overlap the foreground work;
revision acceptance still exclusively controls publication.
The UI thread publishes an owned `QImage` and its revision before diagnostic
work. A second presenter-owned serial worker keeps only the latest immutable
image, computes its exact pixel/profile SHA-256 and current scope mode, and
posts those value results back with analysis, preview, and asset revisions. New
frames cancel that analysis between bounded image rows and diagnostic stages.
Live control reports the preview identity as loading until the matching hash
arrives; stale analysis cannot change the resource ID or scopes.
Close/destruction cancels both owners, clears the bounded pending image, and
waits for both workers. QML still receives only the image URL and engine-owned
diagnostics; this adds neither another renderer nor a pixel algorithm in QML.
The presenter also publishes a stable viewport extent: an accepted 960px
interactive result retains the prior settled maximum edge while adopting a
changed aspect ratio, and an accepted settled result replaces the extent.
Failed, cancelled, superseded, or loader-transient results cannot shrink the
Flickable geometry or clamp the user's same-photo pan.

Develop comparison is desktop-owned transient presentation state. The toolbar
Left/Right view retains one immutable, non-persistent baseline image for the
current asset while the ordinary preview resource remains the live edited
image. Both panes share the existing Flickable transform, so Fit/Fill/Actual,
zoom, and pan stay synchronized without another renderer. Baseline work uses
the same cancellable CatalogService preview path with `ignore_edits=true`, does
not publish a preview record, loses stale results by request/asset revision,
and is released on selection change, Develop exit, crop/pick/mask entry, or
window destruction. The existing single-photo Before/After command remains a
separate view toggle.

Develop crop is interactive: crop-tool preview removes only the final crop and
renders the canonical CPU Perspective operation, including angle. Photo and
mask overlay therefore arrive in the same post-homography coordinates; the
crop frame remains screen-axis aligned. Selection and crop-handle dragging
change in-memory parameters only; release writes the recipe through the same
Develop commit path as the right-panel controls. History restore, Original,
and snapshot restore use that path with a session undo step and without
appending a new history row. Deleting a photo
normally removes only the catalog record
and preview cache. The explicit “Delete from Disk” command deletes the source
after confirmation, then removes the catalog record. QML resources are built
and deployed with `qt_add_qml_module`; the product links no Qt Widgets
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
(history/snapshot); v5 adds seven nullable capture-time/GPS columns
(`captured_local_exif`, `captured_subsecond_digits`,
`captured_utc_offset_minutes`, `gps_latitude_e6`, `gps_longitude_e6`,
`gps_altitude_magnitude_mm`, `gps_altitude_ref`). The SQLite adapter enables
WAL, `synchronous=NORMAL`, and `busy_timeout`. Schema v6 adds one
`asset_recovery_state` row per asset. Database triggers advance its generation
with every durable asset, recipe, tag, metadata, or history write but not with
preview/cache state. Schema v7 adds adapter-private display/folder projections
and the accepted page/filter/sort indexes; changing only those derived fields
does not advance recovery. Schema v8 owns the catalog backup policy and its
last-success/next-run/bytes/failure observations. Schema v9 owns stable direct-
containing-folder IDs and binds each asset to one of them. Schema v10 owns named
manual membership collections and smart `LibraryQuery` documents, revision-
checked mutations, and paged listing that never materializes a whole set
(ADR-0103). Schema v11 replaces global URI uniqueness with
`(normalized_uri, version_ordinal)`, copies recipe/history into a new asset ID
for a virtual copy, and adds `library_stack` / `library_stack_member` with one
pick. Listing collapse is session state on `LibraryPageRequest`, not a smart
collection field. Studio Survey is a browse mode that requests exact
`PreviewPurpose::kBrowse` images serially and does not start N Develop
pipelines (ADR-0105). New catalogs and
migrations use transactions, and an unknown higher schema version fails fast
(ADR-0100/0101/0103/0105).

The repository adapter atomically publishes the current recipe row (or baseline
clearing), optional deletion of history rows newer than a cursor, automatic
history, and catalog revision. A history preview writes the current recipe with
`RecipeHistoryWrite::kUnchanged` so the stack stays intact; a later parameter
edit discards `seq` greater than the cursor in the same transaction before
appending. Adjacent Studio commits from one control carry the exact preceding
ordinary-history ID and update that row in place only while it remains newest;
snapshots, another control, selection/view changes, undo/redo, and intervening
client rows end the session-only group. Any failed step rolls back everything;
services neither compensate writes nor expose a partially committed recipe.
Ravo does not read or migrate a frozen 0.9 catalog. Future
compatibility requires an independent product decision, backup/rollback, and
fixtures.

### Catalog recovery and backup

SQLite is the only live edit authority. After a durable mutation commits,
CatalogService serializes its exact recovery generation to
`<catalog>.ravo/sidecars/<asset-id>.<generation>.ravo.json`. The bounded
canonical document records catalog/source identity, source fingerprint,
review and capture state, tags, writable metadata, canonical recipe, and
ordered history under a SHA-256 envelope. Originals and existing adjacent XMP
are never read or written by this owner. Filesystem success acknowledges only
the serialized generation; an intervening generation stays pending, while an
I/O failure reports the committed catalog fact and leaves a restart-safe retry.
Older generation files are deleted only after exact acknowledgement.

Open, explicit sync, clean close, and backup drain pending generations through
the serial CatalogService owner. Studio's coalesced Develop save advances the
database generation, queues the new preview result for the UI, then drains the
generation on the same serial worker. Sidecar work therefore stays out of the
adjustment-to-preview path without remaining deferred for the lifetime of the
window. Other catalog mutations publish before return. The global catalog
revision in a sidecar is an observation; unrelated asset commits may change it
between publication and acknowledgement, so the asset ID, generation, and
payload excluding that observation define immutable generation content.

A v1 backup is an absent directory containing only `catalog.sqlite`,
`manifest.json`, and `sidecars/`. Creation drains recovery, integrity-checks the
source, checkpoints WAL, holds a bounded writer lock while cancellably copying
the database, removes preview rows from the copy, switches it to a self-
contained journal, copies the exact generation set, rejects concurrent source
changes, verifies hashes/layout/SQLite integrity, and publishes the staged
directory atomically without replacement. The manifest explicitly excludes
originals and previews. Verification never opens the artifact as the live
catalog. There is no non-interruptible `VACUUM INTO` fallback.

Restore accepts only a caller-selected absent catalog path. It verifies the
complete source before creating operation-owned staging, verifies the staged
database and sidecars again, publishes the support root first and the catalog
file last, then opens it through the ordinary repository path. Pre-commit
failure removes only exact staging. Once the catalog file is visible, every
failure reports that durable fact and deletes nothing implicitly. Preview rows
and artifacts remain excluded and are rebuilt explicitly (ADR-0099).

The backup scheduler is catalog state, not a desktop preference. The normal-
priority CatalogService owner checks its persisted due time and creates an
ordinary verified backup. Retention considers only a canonical scheduled name
that re-verifies as the current catalog. It moves an expired candidate to a
unique quarantine, verifies identity again, and only then removes it. Unknown,
changed, malformed, symlink, active, and user-created paths remain. Studio and
CLI display the persisted last verified success, next run, bytes, and failure
without owning retention policy (ADR-0101).

### Folder identity and relink

`catalog_folder.id` is stable while its URI is mutable. Assets bind to the ID
of their direct containing folder; hierarchy-only ancestors remain synthetic
presentation rows. CatalogService derives explicit missing state from a live
read-only directory check. A relink names the stable ID and an existing
replacement directory, requires the old root to be missing, maps every asset
by its existing basename, and validates stored size, modification time, and
content fingerprint. It rejects path and catalog conflicts before mutation.
The SQLite adapter then rechecks the old folder URI and exact asset set and
updates the folder URI, asset URIs, recovery generations, and catalog revision
in one cancellable transaction. Rollback preserves the prior catalog and no
original is ever written (ADR-0101).

### Import

`ImportRequest` carries catalog ID, file/directory input, recursion/format
policy, resource budget, cancellation token, and correlation ID.
`ImportItemResult` returns imported, duplicate, unsupported, or failed for
each input; partial success must not lose failure detail.

First-version import only registers sources: it never copies, moves, renames,
rewrites metadata, or deletes them. Codec probing confirms a format; extensions
are only candidate filters. Enumeration drops a JPEG that shares a parent
directory and case-insensitive stem with a RAW in the same input set; the RAW
is the catalog original and the JPEG is a non-cataloged companion. Ambiguous
`.jpg`/`.jpeg` pairs for one stem fail closed. One LibRaw open reads RAW
capture metadata and embedded JPEG, then persists a `kThumbnailMaxEdge` browse
thumbnail, preferring the companion JPEG when present. A RAW suffix or TIFF RAW
container with no browse JPEG must first-frame-decode before
`commit_imported_asset`. Import does not perform a 1600px full decode when a
browse JPEG exists. It validates trusted metadata before transactionally
publishing an asset. Cancellation stops undispatched work; committed results
remain valid. Missing, directory, unrecognized, unpack-failed, oversized,
malformed or mandatory-unsupported DNG opcode, and unsupported CFA
full-decode inputs fail with stable `reason` context.

ADR-0102 extends that baseline with one planned local-source workspace for Add,
Copy, and Move, including `YYYY/MM` month organization and Studio folder trees
for source and destination. ADR-0104 further gives Copy/Move an optional bounded filename
template (`date`, source stem, stable sequence, and extension) and one distinct
second-copy root. Services derive one relative organization/name, preflight the
complete primary/second media, XMP, and JPEG companion path set with
conservative ASCII-case collision keys, and catalog only the primary URI. Every
requested output uses the existing atomic no-replace stream; when a second copy
is present, source, primary, second copy, and companions are independently
reopened and compared byte for byte under cancellation before catalog
publication. A pre-catalog failure
removes only files published by that item. Move cleanup begins only after that
verification and the ordinary catalog commit. No import option creates schema,
preference, detached task, or second catalog authority.

Studio first enumerates a deterministic bounded input list, then dispatches one
normal-priority `import_one` task at a time. It queues the next item only after
observing the current result. Foreground Develop uses the existing priority
lane and can run between items; cancellation leaves committed assets valid and
stops all undispatched paths. Catalog commits remain serialized (ADR-0100).

### Preview

`PreviewRequest` explicitly carries asset ID, target pixel dimensions,
orientation/colour policy, backend, memory/thread budget, cancellation token,
and request revision. `PreviewResult` returns either trusted profile-labelled
RGB memory pixels or a read-only profiled cache resource, otherwise a
structured failure.

RAW uses the Ravo CPU engine, while JPEG/PNG/TIFF use the raster adapter. Both
share orientation, colour, alpha, scaling, finite-value, and error contracts.
Catalog import fully decodes JPEG/PNG/TIFF before publication and reuses that
RGB8 thumbnail. Deferred Standard/1:1 import still persists a
`kThumbnailMaxEdge` browse thumbnail immediately so Gallery can show photos
while later processed previews drain. Untagged raster browse encodes
file-native 8-bit RGB as sRGB presentation pixels; Develop and export still
fail closed when the source has no declared colour profile. A recognized raster error never becomes a RAW inspect except
`unsupported_tiff_raw_container`. First-frame RAW decode accepts validated
16-bit RGB Bayer 2×2 and X-Trans 6×6 CFA state.
`.dng` uses the same LibRaw path, but copies OpcodeList2/3 bytes into bounded
immutable Ravo values before the LibRaw owner is destroyed. Inspect exposes
supported correction state and optional skips plus CFA family/size and the
sensor-default demosaic mode without running a render.
Preview cache is atomically written outside the database, keyed by source
fingerprint, target dimensions, and contract version. A cached PNG without the
8-byte PNG signature is a miss and is deleted. Corrupt or missing cache
rebuilds from the read-only source. The filesystem adapter serializes access
and owns a 512 MiB hard byte budget. Valid hits refresh persistent access time;
startup orders that time and key deterministically, and commits evict the
least-recently-used PNGs before atomic publication. Database preview rows are
hints and may outlive eviction. Single entries above budget and real directory,
measurement, timestamp, or removal errors fail structurally. Catalog checks
cancellation after encode and before commit. Rebuildable preview PNG uses one
bounded libpng write with its latency-first flag; normal output/export PNG
keeps the ordinary encoder. `CatalogService::close` drops the
repository, raster, cache index, and decoded RAW/working buffers while bounded
disk entries remain available to reopen (ADR-0067, ADR-0087).

Import and Gallery use browse cache. One LibRaw open reads RAW metadata and
embedded JPEG, then writes a PNG at `kThumbnailMaxEdge` under the
`companion-jpeg` digest when a same-stem JPEG exists, otherwise
`embedded-jpeg`. It is not editable scene-linear data. Gallery grid cells keep
that browse thumbnail; Standard/1:1 import drain does not replace it with
processed RAW. Loupe, Develop, scopes, export, and `request_preview` with
`prefer_embedded_preview=false` use preview contract v10: full CPU
decode/render of the RAW. Import writes a colour-calibration baseline for RAW:
as-shot white balance from LibRaw `cam_mul`, the camera input matrix
(`enhanced_matrix` via input profile `source`), `ravo.display.sigmoid`, and
`ravo.detail.sharpen` at the accepted Lab USM defaults (amount 0.5, radius 2,
threshold 0.5). Later Develop edits stack on that baseline. This is Ravo's analogue of a
Lightroom camera profile; Adobe DCP / Adobe Color / Adobe Standard are not
shipped (ADR-0085/0088). As-shot white balance prefers `cam_mul` even when
`as_shot_wb_applied` is set. The cache types
must not share a digest. Without
embedded JPEG, browse fails open to full decode and never writes an empty image.
Cached built-in sRGB PNGs contain one standard `sRGB` chunk; other RGB outputs
contain one `iCCP` and no conflicting `sRGB`. Preview v10 rebuilds prior cache
output.
The baseline creates no `asset_recipe` row and does not set `has_edits`;
persistence begins only after a user override. Existing JPEG/PNG/TIFF are
display-referred input and do not receive Sigmoid twice. Import persists
thumbnail dimensions only; loupe creates 1600px on demand.

The raster adapter accepts PNG/JPEG/BMP/GIF/WebP/TIFF and exports
PNG/JPEG/TIFF. PNG delegates to the adapter-private libpng/ZLIB encoder, JPEG
delegates to the adapter-private pinned libjpeg-turbo encoder, and TIFF
delegates to an adapter-private static LibTIFF built from its pinned source
root with only ZLIB Deflate enabled. The TIFF encoder writes bounded classic
little-endian opaque RGB8 or frozen conditional-grayscale strips and embeds
the exact resolved RGB ICC. Catalog snapshots writable metadata, capture
values, and sorted unique tags once after asset lookup for every rendered
JPEG/PNG/TIFF export. TIFF also
keeps the normalized destination as `DocumentName`. JPEG writes Exif APP1, XMP
APP1, ICC APP2, and optional IPTC APP13; PNG writes one `eXIf` and one
uncompressed XMP `iTXt` and still writes no `pHYs`; TIFF keeps the 72–9600 dpi
baseline plus EXIFIFD, XMP 700, and optional IPTC 33723. Title is not an Exif
field. The encoder does not reopen the destination or mutate a sidecar.
JPEG/GIF/WebP/TIFF plugin targets and
QSQLITE remain configure-time requirements for their current input/runtime
consumers; TIFF output no longer uses the Qt plugin.
TGA/WBMP/ICO and other SQL drivers have no product consumers.

Radiance RGBE tranche 1 remains outside that RGB8 path. A dedicated synchronous
`HdrDecoder` port returns owned linear RGB float32 pixels plus handle-free
Radiance value metadata; borrowed path and encoded-byte inputs live only for
the decode call. The adapter owns header/RLE parsing, bounds, and file I/O.
Until a later HDR engine/preview tranche is accepted, the Qt raster adapter
recognizes both RGBE magic tokens as structured unsupported input and Catalog
publishes no asset or preview. See
[ADR-0027](adr/0027-radiance-rgbe-decoder-contract.md).

### Recipe and operation

Canonical recipes, operation descriptors, `RenderRequest`/`RenderResult`, and
the explicit colour contract are shared by preview, Develop, CLI render, and
export. Editing UI maps the versioned schema only and owns neither a second
algorithm nor history format.

External 3D LUT state is a recipe-owned path plus declared input/output colour
spaces, interpolation, and strength; QML only selects and displays those
values. The Engine owns bounded `.cube` parsing, colour conversion,
interpolation, cancellation, and a thread-safe process LRU of immutable parsed
snapshots. Every snapshot has a complete-content fingerprint. CatalogService
validates that resource before committing a recipe, and the same fingerprint
participates in persistent preview identity; a missing, changed-to-invalid, or
unsupported file is a structured failure and cannot reuse a stale snapshot.
Neither Catalog nor QML parses LUT bytes, and no external colour subprocess or
second graph is introduced (ADR-0096).

Offline camera-noise calibration is not recipe or catalog state. Foundation
owns its handle-free camera identity, black-subtracted uint16
mean/variance/count samples and fitted Gaussian/Poisson resource values. Engine
performs a bounded deterministic robust fit; the JSON adapter owns the strict
sample/profile schemas and canonical
SHA-256 payload; Services owns cancellation-aware, race-safe atomic no-replace
publication. CLI only composes those owners. The command never discovers or
writes an implicit profile directory, and the current denoisers do not load the
artifact automatically. Measurement/extraction and later profile selection are
separate gates (ADR-0096).

S3.1 adds a recipe-owned canonical mask graph under
[ADR-0043](adr/0043-canonical-mask-graph-foundation.md). Each immutable
node has its own schema version and typed all/linear-gradient/circle/rotated-
ellipse/parametric/group payload; v1 identity `all` upgrades on read.
[ADR-0045](adr/0045-studio-mask-overlay-group-path.md) extends that graph
with path/brush kinds, preview overlay, and owned group authoring. Recipe
parsing, upgrade, deterministic serialization, and DAG validation are the only
owners of IDs, references, opacity/inversion, bounds, topology, and version
rules. S3.2's [ADR-0044](adr/0044-studio-canonical-mask-authoring.md)
keeps that ownership in recipe: its pure Develop helper owns Studio's stable
numeric field intents, reserved collision-safe leaf IDs, strict edits, and
detach rules. Catalog/services preserve the canonical graph without rewriting
its mathematics; QML receives presentation values but owns no canonical graph
state or mask mathematics.

The engine-private evaluator accepts full attached-input dimensions plus an
explicit ROI and borrowed input/optional operation-output RGB planes. Pixel
centres are normalized in that full frame, so independently evaluated tiles
join exactly. Canvas may additionally attach an immutable photo-content
subframe: evaluation runs against that original content rectangle and pads the
added area with alpha zero. Attached-subframe evaluation is currently
full-frame only; a sub-ROI rejects rather than translating coordinates
implicitly. Preview overlay alpha is evaluated on that attached frame before
post-Canvas Perspective/straighten/crop, then travels through the same ordered
geometry with bounded bilinear resampling. A later masked consumer is rejected
once composed geometry has discarded the attached-frame coordinates. It owns
only an alpha result and depth-first group
accumulator/child scratch through RAII; stored and expanded graph-work limits
bound shared-DAG recomputation. Invalid ROI/stride/sample count,
non-finite values, missing parametric operation output, cancellation, overflow,
or allocation returns a structured failure before publication. Its only blend
is normal `input + alpha * (operation_output - input)`, with exact alpha 0/1
source selection. Raw budgeting uses saturating arithmetic for the masked
snapshot/output/alpha/evaluator peak.

Color Harmonizer, Graduated ND, Color Balance RGB, Exposure, RGB Curve, Tone
Curve, Highlights, Shadows, Whites, Blacks, and the other named `supports_mask`
operations dispatch a canonical mask. Their unmasked path
remains bit-identical; a masked operation retains a local pre-operation image,
produces local output, evaluates alpha, then mixes before the result becomes
the next recipe input. `DevelopParams` keeps the typed graph and attachments,
including disabled/default instances, so live preview, save/reopen and ordinary
Develop edits cannot baseline-elide a loaded mask. Studio projects read-only
mask-editor maps and forwards numeric intents to the recipe helper through the
existing Develop preview/commit path. Click placement of circle, ellipse, and
linear-gradient geometry uses the same fields; Canvas, Perspective, straighten,
rotate, and flip reject rather than approximating an inverse (ADR-0114). Unshared
Studio-owned leaves and groups are editable; external IDs and shared
attachments are visibly read-only and may only be explicitly detached. Studio can
show a preview-only yellow overlay of the named attachment, author owned group
children, and author path/brush leaves. The Graduated ND density gradient
is still its independent operation formula, not an alias for generic mask
geometry. Historic blend modes and leftover GTK mask-manager consumers remain
outside this tranche.

Explicit Lightroom CRS XMP import lives in the adapters target, not the leftover
darktable history importer. The leftover path rejects `crs:` rather than treating
it as empty history. Mapping writes `DevelopParams` and `recipe_from_develop`;
Studio lists imported presets above History on the Edit left rail. Import copies
a CRS XMP or `.rstyle.json` into `Ravo Presets` next to the library and applies
it to the selected photo. The owned copy's filename is the Studio display name:
rename preserves its dialect suffix and content, while delete is limited to a
validated direct child of that folder and follows an explicit UI confirmation.
`catalog develop --from-xmp` is the same overlay
without replacing crop, masks, or profiles. CRS exposure is exact EV; RAW
contrast targets sigmoid, while display-referred raster contrast retains the
core owner. Highlights/shadows use calibrated scene-EV envelopes;
Whites/Blacks use narrower monotonic scene-EV envelopes with one positive RGB
scale instead of subtracting a global endpoint. Adobe
profiles and looks remain reported omissions rather than hidden colour-engine
substitutes (ADR-0086/0088/0091).

Studio's Curves section authors two operations without folding them.
`ravo.color.rgbcurve` is the default RGB/R/G/B working-space curve, with
optional preserve-colors, middle-grey uncompensate, a parametric
shadows/darks/lights/highlights map composed as `point_curve(parametric(x))`,
and one owned canonical mask (ADR-0110).
Its optional `application_space` defaults to `scene_linear`. A CRS master plus
independent channel curves is instead composed into a restricted
`display_srgb` curve after sigmoid; the engine performs an explicit sRGB
encode/evaluate/decode round trip and rejects preserve-color, compensation, or
parametric combinations in that mode (ADR-0088).
`ravo.core.tonecurve` remains the Lab D50 → ProPhoto RGB-linked default with
`preserve_colors=average`, explicit `lab` / `xyz` / `lab_independent`
working spaces, and one owned canonical mask (ADR-0111). Both share recipe-owned `monotone_hermite`, `catmull_rom`,
and `cubic_spline` evaluators (2–20 nodes). Recipe also owns dense LUT
construction so an evaluator prepares interpolation coefficients once per
curve instead of once per sample; Engine consumes that API without changing
sample positions or per-pixel lookup math. Exact scalar/LUT sample equality is
covered for every interpolation mode. QML draws the plot and histogram; C++
owns points and commits. Histogram bins come from the engine-owned display
RGB8 histogram plus Rec.709 luma.

The lightweight P1 global controls do not stand in for the later full-module
migration queue. The raster/old-recipe core Contrast path plus Saturation and
Vibrance retain the darktable basic-adjustments CPU response. RAW Contrast is
owned by Sigmoid, Highlights/Shadows use the calibrated scene-EV response from
ADR-0088, and Whites/Blacks use the positivity- and tone-order-preserving
response from ADR-0091. A contiguous canonical Highlights → Shadows → Whites →
Blacks sequence composes the same ordered EV maps in one cancellable row pass.
Each of those four operations may carry one owned canonical mask (ADR-0112);
an enabled masked control leaves the fused pass and mixes as a single-amount
envelope, while remaining unmasked neighbours still fuse.
Lab-backed controls share the engine's D50 working conversion; Grain, Bloom,
and Soften defaults use the corresponding source parameters, while Sharpen and
post-crop vignette geometry (signed amount, midpoint, falloff, shape, centre)
are independently accepted below (ADR-0085). Studio presents
darktable-equivalent soft ranges while recipe validation retains the explicit
hard bounds. Full leftover `shadhi`, `gamma`, `grain`, `vignette`, `bloom`, and
`soften` IOP acceptance remains governed by the root migration queue rather than
inferred from these global controls. Dehaze is independently accepted below.

`ravo.repair.retouch` is the first operation with an ordered set of internal
mask attachments rather than one operation-level blend mask. Recipe owns the
region array and canonical leaf references; engine evaluates each leaf and
publishes clone/heal/blur/fill results sequentially on an owned working image.
Normalized source points are converted against the attached frame, while
à-trous original/detail/residual layers and merge state remain engine-private.
Services persist and schedule the immutable recipe, and Studio forwards only
bounded add/remove intents through the command controller. No QML pixel math,
legacy form pointer, preview-pipe cache, or global repair state crosses this
boundary.

`ravo.core.exposure` v2 owns the frozen legacy exposure CPU contract. Manual
mode resolves optional camera metadata into
`effective_ev = exposure_ev - clamp(exposure_bias, -5, 5) +
clamp(highlight_preservation, -1, 4)` and applies
`(sample - black) / (exp2(-effective_ev) - black)`. Deflicker replaces that
effective EV with `target_ev - raw_ev`, where `raw_ev` comes from the first
65,536-bin cumulative histogram sample at the requested percentile. The
histogram is captured from the original `DecodedRaw` before hot-pixel repair,
highlight reconstruction, chromatic-aberration correction, resize, demosaic,
or input-colour conversion.

The RAW decode path publishes a `shared_ptr<const ExposureAnalysisContext>`
containing an owned histogram and value-only metadata snapshot. Every operation
that rebuilds a `LinearWorkingBuffer` propagates the same snapshot, so live
preview and cache reuse cannot silently recompute it from processed pixels.
Pinned Exiv2 remains private to the engine adapter; missing tags are the frozen
zero-EV state, while a file-read failure is retained separately and fails only
when the recipe requests metadata compensation. Histogram/context bytes and
owned error-string capacity participate in the RAW memory estimate, and
allocation, finite-value, and row-cancellation failures publish no output.

Raster input retains manual EV plus black but rejects deflicker and metadata
compensation because it has no original-RAW analysis context. The old GTK area
picker is presentation-time analysis rather than serialized exposure math and
has no implicit Studio substitute. Legacy import accepts only the exact
default-unmasked singleton boundary and returns stable incompatibility for
mask, custom blend, or multi-instance state. Shared old exposure proxy/order
names and `basic.cl` kernel text are cleanup owners for D0.4/S4/S14, not runtime
exposure owners. [ADR-0024](adr/0024-exposure-analysis-and-metadata-contract.md)
freezes these boundaries.

`ravo.display.sigmoid` v1 is the sole default display transform:
`working_space=linear_srgb`, `color_processing=per_channel`, middle-grey
contrast, skew, Standard SDR black/white target, and hue preservation. It is
the RAW baseline and final scene-referred operation. A restricted imported
display-sRGB point curve may follow it before output-profile encoding. RAW
Studio and CRS Contrast belong to Sigmoid; CRS maps the signed slider
logarithmically around the 1.5 default to a +100 endpoint of 3.25.
`ravo.core.contrast` serves
display-referred raster input and old recipes. Highlights/shadows/whites/blacks
are scene controls before the transform; the narrower Whites/Blacks envelopes
meet at middle grey and never create a negative sample from a positive one
(ADR-0088/0091).

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

`ravo.detail.denoiseprofile` v1 remains the pre-light global Y0U0V0
variance-stabilized edge-aware à-trous BayesShrink owner. The generic
Poisson baseline is `a=0.0001`, `b=0`; the finest wavelet detail calibrates
per-channel stabilized noise with a deterministic 2^18-sample MAD bound, then
repeats edge-aware decomposition with the calibrated joint scale. The calibrated
noise propagates through the frozen wavelet variance factor. Canonical ROI scale
selects visible bands. Radius scales à-trous dilation and progressively weights
coarse thresholds; Luminance mixes the reconstructed neutral delta and
`Luminance * Chroma` mixes the colour delta into a separately owned output.
Invalid parameters, scale, dimensions, buffers, finite math, allocation, or
cancellation never swap that output into the working image. RAW preflight owns
four float-RGB scratch planes plus the greater bounded MAD/coordinate scratch.
Preview contract v10 invalidates fixed-profile output (ADR-0094).

`ravo.core.toneequal` v1 remains a global hue-preserving scene operation before
Sigmoid. Five authored controls at -8/-6/-4/-2/0 EV expand through adjacent-EV
interpolation into nine one-stop correction targets; a normalized Gaussian RBF
builds the bounded LUT. Pixel assignment uses the existing linear-RGB L2 energy,
but its mask is filtered in log2-EV with a 240-original-pixel radius scaled by
the immutable canonical ROI scale and 0.04 EV² regularization. The self-guided
filter smooths low-contrast mask texture while retaining strong boundaries, so
one common RGB correction preserves local texture and does not introduce broad
edge halos. Invalid scale, dimensions, non-finite input/output, allocation, and
cancellation fail without publication. The RAW memory estimate includes the
five-plane peak and LUT; preview contract v9 first invalidated the former sparse
pseudo-inverse/linear-mask pixels (ADR-0092).

The private Bayer demosaic owner accepts exactly one RGB 2×2 CFA and one
explicit `rcd|ppg` mode. RCD is the Bayer absent-operation/default choice and uses
task-local 194-pixel tiles with a 176-pixel output step and nine-pixel border;
PPG is a scalar compatibility choice. Unsupported CFA/mode and duplicate
enabled selection fail structurally—there is no IGV, 3×3, or other hidden
fallback. Full resolution consumes exact normalized sensor samples. Preview
reduction averages only source samples with the same CFA colour and phase, so
resize does not blend mosaic channels before interpolation. Both paths retain
positive floating-point headroom, check cancellation by row/tile, publish only
a complete owned RGB buffer, and include prepared CFA/RGB plus RCD tile scratch
in the RAW memory estimate. Recipe absence/default RCD and explicit PPG share
CLI, Catalog, Studio, style, undo and cache identity.

The private X-Trans owner accepts exactly the standard RGB 6×6 CFA with
8 red, 20 green and 8 blue sites. Recipe absence selects Markesteijn 3-pass;
explicit `markesteijn1|markesteijn3` select four/eight directional candidates.
Task-local 122-pixel tiles use mirrored input extension and 12/17-pixel
borders, while preview reduction preserves CFA colour and phase. RCD/PPG on
X-Trans and Markesteijn on Bayer fail with `demosaic_sensor_mismatch`; no mode
fallback is allowed. The memory estimate includes the prepared CFA, owned RGB
output and every concurrent tile scratch owner.

`ravo.raw.denoise` runs on the owned decoded copy before either demosaic path.
Its Bayer path keeps four dense 2×2 planes. Its X-Trans path follows the frozen
nearest-neighbour RGB reconstruction, square-root variance stabilization,
five-level hat-wavelet threshold and square write-back only to matching CFA
sites. A separate output plane makes cancellation atomic; full-plane wavelet
scratch participates in the preflight memory budget.

DNG OpcodeList2 GainMap runs after black/white linear-reference normalization
and before CFA interpolation. Supported OpcodeList3 GainMap and
FixVignetteRadial run in declared order after demosaic while the buffer is
still camera RGB, then white balance and Input Color run. WarpRectilinear is
parsed and inspect-visible so the file's lens geometry is not hidden, but the
default colour decode does not apply it. darktable keeps DNG warp for the lens
module (off by default); RapidRAW uses optional lensfun. Ravo import uses
as-shot white balance, the camera matrix, Sigmoid, and default Lab USM. The private adapter
owns all parsed values, applies DNG's `[0, 1]` clip after each executed
List2/List3 opcode, and preserves repeated operations. Known malformed or
unknown mandatory operations fail before publication; unknown optional
operations are retained as inspect-visible skip records. Opcode maps
participate in RAW memory estimation.

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

`ravo.color.profilegamma` v1 is an opt-in source-RGB correction immediately
before input colour. RAW applies it after demosaic; raster applies it after
encoded RGB normalization. Logarithmic mode uses the frozen float `fastlog2`
approximation and `2^-16` input/output floors. Gamma mode owns a render-local
65,536-entry piecewise LUT and four-sample exponential extrapolation. Both
paths produce an owned buffer, retain the exact source `ColorProfileState`,
check cancellation by row, and enter the scene-linear preprocess cache key.
Absence or disabled state is the identity; enabled default log parameters are
not. `security_factor` remains canonical but has no pixel effect. The old
picker/autotune is unsupported until an engine/service analysis contract owns
pre-operation pixels, ROI, statistics, cancellation, and recipe revision.

Canonical recipe schema v3 upgrades schema-v1/v2 recipes by inserting explicit
source → linear Rec709 `ravo.color.input` and working → output
`ravo.color.output` operations. Input colour follows RAW preprocessing and owns
input-profile to working-profile conversion.
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
to their declared linear Rec709/sRGB workspaces.

`ravo.color.output` consumes that immutable working state after RGB operations.
Built-in and file profiles, four rendering intents, soft proof, gamut warning,
proof intent, and black-point compensation are canonical recipe state. Matrix/
shaper RGB output uses the frozen inverse LUT and unbounded extrapolation;
matrix-free ICC plus XYZ/Lab/proof branches use render-local LittleCMS objects.
Only `ColorProfileState` crosses the engine boundary: it owns deterministic
ICC bytes and the declared model but no transform handle. Preview and file ICC
content enter the final cache identity, while the reusable scene-linear cache
depends only on input/working state. The engine PNG writer and Qt raster adapter
embed the same state or fail before publication. Studio attaches it to QImage
and only presents engine-computed soft-proof/gamut pixels.

`ravo.geometry.canvas` v1 grows the linear Rec.709 working buffer before later
colour/effect operations. Its frozen integer placement owns four percentage
extents, five opaque fill colours, dimension caps, and the attached photo
content frame used by masks and Retouch. Nested Canvas, Canvas masks, attached
sub-ROI evaluation, post-Canvas rotate/flip/lens, and another mask consumer
after composed geometry reject explicitly. Canonical Perspective/straighten
and crop consume the attached frame and transform preview alpha beside pixels.

`ravo.output.dither` v1 consumes profiled encoded RGB immediately after Output
Color. Random TEA, source-order Floyd–Steinberg, posterize, and target-aware auto
remain engine-private and run before packing. `ravo.output.frame` v1 then owns
the final encoded-output dimensions, border/aspect/orientation/basis/position,
and optional line. It pads preview overlays to the same final layout and
retains the exact output profile. Encoders receive one already framed pixel
product and do not repeat layout mathematics. Both stages validate finite
input, memory bounds, and cancellation before publication.

`ravo.output.watermark` v1 is the final encoded-output operation after Frame.
Recipe owns bounded text and placement state; the engine expands only source
stem/asset ID, evaluates the compiled 5×7 glyph table, samples deterministic
four-point coverage, rotates about the watermark centre, and alpha-composites
onto its owned profiled float buffer. No filesystem resource, host font,
metadata service, Qt/Pango glyph object, or mutable configuration state crosses
the boundary. Unsupported characters/tokens and the legacy missing-resource
record fail instead of producing an invisible edit.

Final RGB8 packing is a private engine boundary after output colour, optional
dither, optional frame, and optional watermark. It accepts
only finite profiled float RGB, clamps negative values, multiplies by 255,
rounds, clamps super-white, and publishes an owned RGB-order `RenderedImage`
with the exact `ColorProfileState`; it performs no additional transfer curve.
The old BGR order was a GTK/pixelpipe destination layout. Legacy XMP `gamma`
is strictly absorbed as this mandatory singleton boundary and creates no recipe
operation. Its channel/mask display branches are unsupported presentation
adapters because they depend on mutable, non-self-describing old display state.
Remaining frozen order, registry, and pixelpipe references are cleanup owned by
D0.4/S4 and are not Ravo production dependencies.

`ravo.color.primaries` v1 is the first working-profile-relative RGB grading
operation. Recipe hue values are radians and purity values scale the ray from
the working white point to its primary triangle. The engine derives xy from
`LinearWorkingBuffer::color_profile.matrix_to_xyz_d50`, builds
`inverse(working_to_xyz) * custom_to_xyz`, and keeps the original profile state
on the adjusted pixels. It runs immediately after input colour and before the
linear-Rec709 bridge used by existing RGB operations. Studio converts hue to
degrees only for presentation and intent forwarding; it owns no chromaticity
or display-profile calculation. Its drag ranges mirror the source soft ranges:
primary hue ±20°, primary purity 0.5–1.5, and achromatic purity 0–0.2; the
canonical recipe retains the full validated hard bounds.

`ravo.color.channelmixerrgb` v1 fixes the `linear_srgb_d50` workspace and
V3 algorithm, persisting three mixing rows, saturation/lightness/grey,
normalization, adaptation, illuminant xy, gamut, and clip. Studio normally
edits only the `adaptation=rgb` 3×3 matrix; CAT16/Bradford/XYZ are selected
only through explicit canonical parameters and never hidden camera/ICC state.

`ravo.color.colorchecker` v1 is an independent, explicitly present calibration
operation. Its canonical state is `working_space=lab_d50`,
`algorithm=thin_plate_rbf_v2`, and 0–49 ordered source/target Lab patch pairs;
it shares neither schema nor cache identity with Color Calibration, Primaries,
or Color Balance RGB. Canonical Develop order places it after the accepted
tone-equalizer and graduated-filter stages and before subsequent colour grading.
Absence skips it, while an explicitly present default 24-patch operation is
serialized, rendered, cached, and reopened rather than treated as identity.

The CPU engine requires the explicit linear-Rec709 working state and privately
bridges RGB through XYZ D50 to Lab and back. Zero through four patches use the
frozen special polynomial fits; larger sets use the `(N+4)` thin-plate RBF
system. Coefficient layout, float addition order, bit-level fast-log kernel,
Gaussian pivoting, and the sequential N=2/N=3 versus shared N=4/RBF singular
fallbacks remain source-exact. Fit matrices, pivots, coefficients, patch copies,
and output bytes enter render memory estimates. Invalid profiles, dimensions,
buffers, components, denominators, kernels, coefficients, results, allocation,
or cancellation fail before an owned output is published; input pixels, profile,
and immutable exposure-analysis state are retained unchanged.

The strict decoder accepts only the evidenced enabled-v2, default-unmasked
singleton presentation and a synthetic v1 parameter-history upgrade. It ignores
inactive v2 tail planes because frozen patch removal leaves stale bytes. The one
real record is preserved verbatim in a minimal positive document; the complete
0098 history remains structured unsupported because unrelated preceding
operations have no canonical mapping. Disabled legacy state, duplicates, masks,
custom blend, multi-instance, malformed, non-finite active, and unsupported
version state reject explicitly. Studio owns preset selection and direct Lab
patch intent, not fitting mathematics. All eight frozen presets are exact data;
the GTK live chart, picker, add/remove interaction, and OpenCL execution are not
new product contracts. Shared `common/colorchecker.h`, Gaussian helpers,
`extended.cl`, order, and registry text remain D0.3/D0.4/S1/S4 cleanup owners,
not runtime dependencies of the canonical operation.
[ADR-0026](adr/0026-colorchecker-calibration-contract.md)
freezes these boundaries.

The RGB↔XYZ D50↔Lab conversion is one engine-private, value-only owner shared
by Color Checker, Legacy Color Balance, Tone Curve, and the Lab basic controls.
It freezes the transposed matrix expression order, D50 reciprocal,
epsilon/kappa branches, Lab scale/add order, negative zero, and non-finite
propagation with source-derived goldens. Algebraic steps are bit-exact;
`cbrtf`-dependent round trips retain exact host-local scalar-oracle agreement
and a recorded 1e-5 component tolerance for supported platform libm variation.
It adds no clamp, finite repair, profile selection, or publication policy. This
bounded S1.1 owner is complete, while the wider S1 colour-science workspace/LUT
migration remains unfinished.

`ravo.color.colorbalancergb` v1 also explicitly declares
`linear_srgb_d50`. It stores four Y/C/H zones, three falloff/fulcrum values,
chroma/saturation/brilliance, hue/vibrance/contrast, and formula. The engine
derives a render-lifetime gamut LUT and executes CAT16 D65, CIE 2006 LMS,
Filmlight Yrg/Ych, and DT UCS by default or explicit JzAzBz. It writes pixels
to an owned output buffer and publishes only after every row succeeds. Studio
projects canonical values into a read-only parameter map; QML owns no mask,
LUT, or colour mathematics.

The independent `ravo.color.colorbalance` v1 contract owns the complete frozen
legacy Color Balance CPU path rather than reviving the removed three-parameter
approximation. It stores explicit presence plus mode and all 16 numeric legacy
values. Linear sRGB D50 enters a Lab D50/XYZ/ProPhoto boundary, where corrected
RGBL lift/gamma/gain or slope/offset/power, input/output saturation, and
grey-fulcrum contrast execute before the inverse conversion. An explicitly
present default operation still performs this round trip; absence alone skips
it. The engine owns finite/domain/cancellation checks and atomic output, while
Studio exposes numeric intent only. Picker, HSL colour derivation, auto
optimisation, masks, custom blend state, and multiple instances do not acquire
presentation or graph semantics; unsupported serialized state rejects
structurally. Shared ordering/proxy/name strings and the `extended.cl` kernels
remain retirement work for D0.4/S4/S14, not runtime owners.

`ravo.color.colorcorrection` v1 is an independent, explicitly present affine
D50 Lab operation. Its exact seven-field schema declares `working_space=lab_d50`,
`algorithm=affine_lab_v1`, four highlight/shadow a*/b* endpoints in [-40, 40],
and saturation in [-3, 3]. Numeric edits establish presence; field reset retains
presence, while whole-operation or Color-section reset removes it. Canonical
Develop order is Color Balance RGB, Color Correction, then Color Contrast.
Absence alone skips the operation; an explicit default remains serialized and
executes because its RGB/Lab round trip is observable.

The CPU path accepts only declared linear-Rec709 RGB working pixels and reuses
the private S1.1 D50 bridge. It derives each float scale as
`(highlight - shadow) / 100.0F`, retains the shadow base, copies L*, and
evaluates `saturation * (chromatic + L * scale + base)` in frozen order. It
adds no clamp, repair, transfer curve, or profile fallback. Dimensions, buffer,
profile, parameters, samples, allocation, and pre/row/final cancellation fail
before publication. Success returns separately owned pixels while preserving
the exact profile and immutable exposure-analysis snapshot; generic working
buffers remain part of the engine memory budget.

Across all 158 frozen XMPs, only 0029 and 0092 contain actual operation records.
The strict decoder accepts their enabled-v1 singleton, exact priority-zero,
unnamed, default-unmasked envelope and exact blend-v9/v11 default payloads;
history position only names the instance. Every unsupported version, disabled,
duplicate, mask, custom blend, multi/name/priority, unknown, malformed, or
non-finite state rejects structurally. Studio exposes the five numeric intents,
not the old GTK 2D plane/picker, three presets, blend UI, or OpenCL path. Shared
`basic.cl`, order/modulegroup/usermanual names, and pixmap remain D0.3/D0.4 or
later cleanup owners. Bundled `.dtstyle` examples are retired by ADR-0072.
[ADR-0029](adr/0029-colorcorrection-contract.md)
freezes this boundary.

`ravo.color.colorcontrast` v2 is the independent frozen Color Contrast owner.
Its exact seven-field schema declares `working_space=lab_d50`,
`algorithm=axis_affine_v2`, separate finite float-representable a*/b*
steepness and offset values, and boolean `unbound`; steepness is bounded to
[0, 5], while offsets retain the complete finite float surface. Develop owns
explicit presence because even canonical defaults execute an observable D50
round trip. Numeric edits establish presence, individual resets retain it, and
whole-operation or Color-section reset removes it. Canonical order is Color
Correction, Color Contrast, then Velvia.

The former Ravo schema-v1 `amount` contract remains compatibility input only.
A nonzero value narrows in its original float order and maps to both slopes,
zero offsets, and `unbound=true`; zero preserves its prior skip. Frozen legacy
module v1 separately copies its four floats and adds `unbound=false`. Recipe
validation, Develop load, and engine dispatch share those deterministic
normalizations rather than maintaining parallel runtime implementations.

The CPU path accepts only declared linear-Rec709 RGB and privately uses the
S1.1 D50 Lab bridge. It narrows canonical values once, then evaluates each
chromatic channel as `input * steepness + offset` in frozen float order.
Unbounded mode adds no clamp; bounded mode preserves the frozen per-axis
ternary clamp to [-128, 128]. L* and the surrounding profile contract are
retained. Dimensions, buffer length, profile, parameters, input/output samples,
allocation, mask state, and pre/row/final cancellation fail before publication.
Success owns separate pixels and profile storage, preserves the immutable
exposure-analysis snapshot, and leaves its borrowed input unchanged.

The 158-XMP census has one actual record, in 0038. Strict import accepts its
enabled-v2 singleton, priority-zero, unnamed, exact default-unmasked blend-v10
envelope and the corresponding synthetic v1 upgrade; history position remains
only the generic instance identifier. The complete 0038 document is a stable
negative because it contains the unsupported mask graph. Disabled, duplicate,
custom-blend, multi/name/priority, mask, unknown, malformed, non-finite, and
unsupported-version state rejects structurally. CLI, Catalog, and Studio use
the same recipe/engine/cache path; QML forwards only the six Develop fields and
contains no Lab mathematics. GTK presentation and OpenCL are not product
contracts. Shared `extended.cl`, order/modulegroup/usermanual names, the sepia
style, and frozen fixtures remain D0.3/D0.4/S14/E1 cleanup or evidence owners.
[ADR-0031](adr/0031-colorcontrast-contract.md) freezes this boundary.

`ravo.color.velvia` v2 follows Color Contrast in linear Rec.709. Its canonical
state is explicit presence/enabled, strength `0..100`, mid-tones bias `0..1`,
and an optional mask. The engine retains the frozen float order: min/max RGB,
HSL-style luminance and saturation, low-saturation plus midtone-bias weight,
strength multiplication, per-channel distance from half the other-channel
sum, and `[0,1]` clamp. Strength zero is bit-preserving identity. Schema-v1
Ravo amount remains an upgrade input only and maps to strength times 100.

The operation accepts only declared linear-Rec709 finite working pixels.
Dimensions, buffers, profile, parameters, allocation, row/pre-publication
cancellation, and source ownership fail before publication. Canonical masks
run through the shared evaluator and normal mix. Strict import accepts only
the exact enabled 0063 version-2 singleton, eight-byte parameter payload, and
blend-v10 default-unmasked envelope. Recipe, CLI, Catalog, Studio, history,
styles, preview, and export consume the same typed state; QML contains no
Velvia math. The old IOP, exclusive kernel, and icons are retired while shared
order/module-group/manual names and frozen fixtures remain with their owners.
[ADR-0095](adr/0095-velvia-weighted-saturation-contract.md) freezes this
boundary.

`ravo.color.colorharmonizer` v1 is a bounded profile-aware colour operation,
not an alias for Color Equalizer, Color Balance RGB, or an HSL nearest-hue
control. Its exact 17-field flat schema fixes
`working_space=profile_linear_rgb_d50`,
`algorithm=dt_ucs_harmony_v1`, one of nine predefined rules or `custom`, the
anchor/pull/neutral/width values, four custom hues, custom node count, four node
saturations, and smoothing. The two real version-1 records at history positions
12 and 13 in frozen fixture 0176 provide default and edited parameter evidence
and the accepted strict-import singleton envelope; they do not make the
complete 0176 document compatible.

The engine boundary accepts enabled unmasked operations plus an S3.1 canonical
attachment resolved by the private normal-mix dispatcher. It clips each finite
working RGB channel with
source-order `fmaxf(value, 0)`, applies the declared profile matrix to XYZ D50,
and reuses the private S1.2 D50/CAT16-D65/dt-UCS bridge. S2.1 supplies immutable
720-step UCS↔RYB lookup, all nine predefined node geometries, custom-node
lookup, circular attraction, and strict winner behavior. Neutral protection is
cubed before the `0.03F` cutoff; denominator, pull, wrap, saturation, inverse
dt-UCS, and inverse profile-matrix expressions retain their frozen float order.
The source compiles with float contraction disabled and adds no transfer curve,
FMA, reassociation, fallback profile, clamp beyond the frozen negative input
clip, or non-finite repair. Production and the independent scalar oracle match
bit-for-bit on each host; libm-dependent Color Harmonizer output references
also retain a recorded 1e-5 cross-platform tolerance.

For positive smoothing, `LinearWorkingBuffer` supplies one immutable canonical
ROI scale: current pixel density over original input density. RAW/raster
creation validates oriented proportional geometry and every derived working
buffer/cache preserves the value; unknown geometry is explicit and positive
smoothing fails as `invalid_colorharmonizer_roi_scale`, while smoothing zero
does not consume it. Pass 1 owns JCH 3c plus the sole correction 2c; private
S2.2 moves that correction into an exact `DT_IOP_GAUSSIAN_ZERO` two-channel
vertical/horizontal recurrence with its ±1e9 per-read clamp, then Pass 2 owns
the RGB output. Validation covers schema bounds/float narrowing, dimensions,
buffers/overflow, profile/matrix state, scale/sigma, finite signals and every
input/output sample. Cancellation covers preflight, mapping, recursive stages,
apply, and pre-publication. Success deep-copies RGB/profile, shares immutable
exposure analysis and preserves scale; any failure leaves borrowed input
unchanged and publishes nothing. Canonical S3.1 attachments are evaluated only
by the outer engine dispatcher; direct operation entry points remain fail-closed
when given a mask.

The private legacy-XMP adapter maps the evidenced v1 singleton (enabled 1,
priority 0, empty multi_name, missing-or-zero multi_name_hand_edited, blend v14
default-unmasked payload, no mask/custom blend/extra attrs) onto one canonical
operation. Ordered revisions are validated and the greatest numeric history
position wins. Develop owns explicit presence plus the existing 17-field
parameter object; CLI `--set`, Catalog preview/save/reopen/export, and one
Studio section consume that same recipe, including smoothing `0..2`. Cache
identity follows the canonical recipe/engine path. Synthetic positive legacy
payloads remain rejected because no frozen record evidences them. S3.1/S3.2's
canonical recipe graph and bounded Studio authoring do not relax that importer.
Studio overlay, owned group editing, and path/brush are accepted under
[ADR-0045](adr/0045-studio-mask-overlay-group-path.md). Remaining blend
modes and leftover GTK mask-manager consumers stay later work and do not
authorize C15. The frozen `iop/colorharmonizer.c` owner is retired.
[ADR-0035](adr/0035-colorharmonizer-core-contract.md) freezes the core;
[ADR-0041](adr/0041-colorharmonizer-smoothing-zero-vertical-slice.md)
extends it with the first product surface; [ADR-0042](adr/0042-colorharmonizer-canonical-roi-recursive-smoothing.md)
accepts canonical positive smoothing.

`ravo.color.colorzones` v1 is an optional D50 Lab/LCh curve owner, distinct
from the default dt-UCS Color Equalizer. Recipe owns one selection axis plus
three independent ordered curves and interpolation types. Engine owns their
source-quantized 65,536-entry LUTs, periodic hue geometry, low-chroma blend,
Lab formula, normal-mask dispatch, cancellation, and 768 KiB LUT resource
term. Studio's eight-band view is a bounded editor projection; custom canonical
nodes and attached masks remain immutable unless an owner capable of editing
their full graph is used.

`ravo.color.monochrome` v2 owns the creative Lab colour-filter path. It creates
a source-fast-exp filter plane from a*/b*, runs the engine-private shared
bilateral lightness grid at canonical original-pixel scale, applies the frozen
lightness envelope/highlight blend, clears chroma, and optionally mixes in Lab.
Retouch and Monochrome call the same bilateral primitive; mask dispatch,
cancellation, finite checks, and resource estimates remain outer-engine
contracts. Camera monochrome flags and demosaic passthrough are unrelated
source-workflow state.

`ravo.color.splittoning` v2 consumes linear Rec.709 through the engine-private
shared HSL value helpers. Its shadow/highlight hue+saturation, balance pivot,
compression band, and mix remain canonical state; row processing owns the
frozen HSL lightness preservation and clamp order. Masks and publication use
the same outer dispatcher as other accepted creative colour operations.

`ravo.color.colorreconstruct` v1 is the post-demosaic highlight-colour owner.
Its exact schema declares `working_space=lab_d50`,
`algorithm=bilateral_grid_v3`, threshold, spatial/range extent, normalized hue,
and none/chroma/hue precedence. Develop places it immediately before Output
Color. The engine therefore receives the explicit linear-Rec709 compatibility
working state, converts privately through the accepted D50 Lab bridge, and
retains the input profile on the separately owned result.

The CPU path sees the complete attached frame. It splats only samples at or
below threshold into the bounded 3D grid, applies the frozen in-place five-tap
blur in x/y/lightness order, and trilinearly slices replacement a*/b* through
the frozen 95%-threshold blend ramp while preserving L*. Spatial extent is in
original-input pixels and consumes `CanonicalRoiScale`; unknown or
non-proportional scale rejects instead of guessing or evaluating a local tile.
The RAW preflight accounts for the one owned grid next to the ordinary working
buffers. Splat-row, blur-line, slice-row, and pre-publication cancellation,
invalid dimensions/buffer/profile/scale, non-finite samples, overflow, and
allocation return a structured failure without changing the borrowed input.

The strict XMP adapter accepts only the one evidenced 0052 enabled-v3,
priority-zero, unnamed, default-unmasked singleton and exact built-in RAW blend
tuples in that document. Studio exposes the same threshold, spatial/range,
hue, and precedence values through the existing revisioned Develop task path.
No GTK preview-grid cache, historic blend graph, tile-local approximation, or
OpenCL owner enters Ravo. [ADR-0055](adr/0055-colorreconstruction-bilateral-grid-contract.md)
freezes these boundaries.

`ravo.detail.texture` v1 is the optional local-texture owner before Sharpen.
Recipe stores signed strength, an original-input-pixel detail threshold and a
bounded integer iteration count; the identity default emits no operation.
Develop, CLI, Catalog and Studio share those fields, with the common Texture
control above Sharpen and scale/iterations kept in a collapsed advanced group.

The Engine measures linear-Rec.709 luminance and uses the private scalar
self-guided filter shared with Tone Equalizer. A fine radius of
`3.5 * detail_threshold * canonical_scale` and a four-times-coarser radius
produce two bands. Only the filter guide is bounded to `[1e-5,32]`; the result
uses a positive luminance ratio on the original RGB, so channel ratios,
negative gamut components and unbounded positive highlights are not
independently clipped. Each later iteration halves its blend. The caller's
buffer remains borrowed, result/profile metadata are owned, and no partial
result crosses publication.

The RAW preflight includes middle/base plus the shared filter's four live float
planes. Invalid dimensions/profile/scale/parameters, non-finite samples,
allocation and cancellation at input, filter, output-row or publication
checkpoints return structured failures. The opt-in Release contract caps the
production operation at 30 ms for each committed 960×640 RAW buffer. ART's
mask/resampling/application/OpenMP owners and the rejected Local Laplacian
pyramids remain outside production. Filmulator's physical-development model is
also test-only research after its 155–157 ms CPU result failed the interaction
budget. [ADR-0096](adr/0096-reference-algorithm-assimilation-boundary.md)
records the selection and rejection evidence.

`ravo.detail.sharpen` schema v2 is the accepted scale-aware D50 Lab L* USM.
The RAW import baseline enables it at amount 0.5, radius 2, and threshold 0.5
without creating an `asset_recipe` row. Raster files stay at amount 0.
The explicit schema fixes `working_space=lab_d50`,
`algorithm=separable_gaussian_usm_v1`, radius, amount, and threshold. Current
Ravo schema-v1 values upgrade to v2 in one recipe owner; the former approximate
whole-plane RGB implementation has no compatibility runtime. Canonical Develop
places the operation before effects/geometry/display packing and both CLI and
Studio forward the same three numeric intents.

The engine-private owner narrows parameters once, multiplies radius by 2.5,
scales it by current/original pixel density, and caps only the convolution
support at 12. The requested radius continues to control the truncated Gaussian
sigma. A complete Lab plane and row scratch preserve source-order vertical then
horizontal convolution; only L* receives the signed, threshold-subtracted
detail, while a*/b* and kernel-width borders remain unchanged. Invalid scale is
explicit for a positive radius. Input/output conversion rows, both convolution
stages, and pre-publication are cancellable; all failure paths retain the
borrowed input and publish no partial output. RAW memory preflight includes the
Lab plane, row, and bounded kernel. Strict import accepts only the three
evidenced enabled-v1 singleton records with their exact v9/v11 default-unmasked
blends. Demosaic capture sharpening in fixture 0171 is a separate R2/S2 owner.
[ADR-0056](adr/0056-source-exact-lab-sharpen.md) freezes these boundaries.

`ravo.effect.dehaze` schema v2 is a RAW source-preprocess operation with
`working_space=source_linear_rgb`, `algorithm=dark_channel_guided_v4`,
strength, distance, and adaptive window state. It executes after demosaic and
before profile-gamma/Input Color on the declared camera-matrix RGB buffer.
`linear_working_from_raw` applies it once, RAW preprocess/cache identity owns
every parameter, and both engine and services disable it before ordinary RGB
recipe dispatch. Encoded raster and a caller that tries to run it on an
already-converted working buffer fail structurally.

The CPU owner computes full-frame dark-channel min windows, deterministic 95%
hazy/bright quantiles, ambient RGB and characteristic haze distance. It then
uses max/min transition windows and a private S2 RGB guided filter: 512-pixel
tiles, rounded `3*w` overlap, source-order Kahan box means, all RGB covariance
terms, Cramer's-rule coefficients, and the frozen singular fallback. Adaptive
window radii consume canonical current/original scale; non-adaptive keeps
full-scale radii. Complete transition planes and bounded tile statistics enter
RAW memory preflight. Dark-channel, selection, transition, guided statistics/
solve, output and prepublication checkpoints share the render cancellation
token; every invalid ambient/profile/scale/buffer/non-finite/allocation path
publishes no output. The generic old guided-filter owner remains frozen for
other unaccepted consumers. [ADR-0057](adr/0057-source-linear-dark-channel-dehaze.md)
freezes these boundaries.

Every decode/preview/render boundary carries explicit pixel format, alpha,
source/target colour description, and profile state. UI, file name, or unmarked
buffer must not implicitly select colour strategy. See
[ADR-0006](adr/0006-explicit-colour-contract.md).

## Services

First-version services provide:

- `CreateCatalog` / `OpenCatalog`: create, validate, migrate, and return an
  immutable catalog snapshot;
- `ImportAssets`: enumerate inputs, call codec/engine, transactionally publish
  assets, and schedule preview;
- `ListAssets` / `ObserveCatalog`: return stable order and live
  `CatalogSnapshot.revision` without SQL cursors. Studio's presenter polls
  that revision on a 1 s timer and reloads listing/recipe/history when another
  client commits. A completed Studio import advances the presenter's observed
  revision from the same returned listing before publishing it, so the timer
  cannot misclassify that local write and discard a later in-memory edit; QML
  does not poll or read the database;
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
  snapshots; writes make a new revision. `snapshot().revision` is read from
  SQLite on each call so a second connection's commit is visible.
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
- Explicit capture refresh reads one source generation and publishes asset
  identity, capture row, and revision in one repository transaction. Export
  privacy is a typed request: location removal happens before the prepared
  metadata boundary, while disabled metadata reaches encoders only as an empty
  snapshot; ICC remains part of pixel color state.

## Desktop boundary

Ravo Studio owns:

- creating/opening catalogs and one session-owned import workspace for local
  source browsing, selection, Add/Copy/Move planning, destination organization,
  bounded rename templates, optional verified second-copy root, and initial
  preview policy. Enumeration publishes named placeholders before codec work;
  C++ fills workspace thumbnails asynchronously from viewport demand. Services
  own enumeration, inspection, complete two-tree preflight, atomic transfer,
  byte verification, catalog publication, XMP and JPEG companions, and source
  cleanup;
  QML only presents state and forwards intents (ADR-0102/0104);
- a session-only Last Imported Photos source group. The desktop presenter owns
  its successful-batch time bounds and selection lifecycle. Clicking Import
  closes the workspace immediately and publishes named Gallery placeholders
  for the selected files; catalog polling stays suppressed while items fill
  those cells. After the batch, Studio performs one bounded page query for the
  successful Last Imported Photos range. Cancellation publishes only completed
  items, folder selection leaves the group, and catalog replacement destroys
  it. No schema row or QML-owned asset list is introduced;
- Gallery list states: loading, ready, missing, unsupported, and failed;
- selection, Gallery grid/loupe and Edit panes, fit, 100%, and pan. Grid and
  filmstrip use whole-image containment with letterbox number, rating,
  format/dimension, and flag overlays. Gallery's left rail is the folder tree,
  tag filter, and Import/Export; Edit's left rail is the selected photo's
  recipe history and snapshots. Clicking a step previews that recipe and dims
  newer rows; a subsequent parameter edit discards the dimmed rows. The default
  Edit grading stack is Light, Curves, Color, Color Mixer, Color · Advanced,
  and Camera Calibration. Color groups the independently resettable White
  Balance owner, Presence, global Hue, and three-way/global Color Balance RGB
  wheels without moving parameter math into QML. Color Mixer projects the
  Color Equalizer's eight named bands as swatches with Hue, Saturation, and
  Luminance tracks. Overlapping Lab and legacy color tools stay under Color ·
  Advanced (ADR-0082/0084/0085). Bayer white-balance pick writes manual
  temperature coefficients (ADR-0083);
- Presets place **Save…** immediately to the right of **Import…**. Save shows
  only baseline-relative modifications, selects none initially, and publishes
  a managed selective `.rstyle.json`; imported complete styles and Lightroom
  CRS presets remain supported alongside it;
- Edit history presents exactly **Copy Parameters** and **Paste Parameters**.
  Copy requires explicit baseline-relative field selection; paste changes only
  that immutable session selection and preserves other target edits. **Paste
  Parameters to Selection** is a separate command: CatalogService overlays the
  same clipboard onto every ID in an explicit multi-selection, with catalog
  revision preflight, per-item partial failure, and cancellation that keeps
  completed photos. Session undo remains one-photo (ADR-0078/0098/0107);
- shared scopes above the right Gallery/Edit panel, defaulting a new Studio
  session to RGB Parade and remaining visible while the Edit list scrolls:
  frozen 256-bin RGB Histogram, overlaid Waveform, RGB
  Parade, fixed linear D50 CIE u*v* Vectorscope, and Waveform/Vectorscope
  Split. Each preview refreshes the histogram used by Curves and only the
  currently selected diagnostic; switching modes recomputes that mode from the
  current owned preview. Engine owns pixels; QML owns only grids and selection;
- progress, cancellation, and recoverable-error presentation;
- versioned English `ravo.debug.photo` and `ravo.debug.parameters` clipboard
  blocks for the selected asset. Desktop C++ serializes the current canonical
  recipe and saved/pending state; QML only invokes the registered commands;
- window, focus, keyboard, HiDPI, and basic accessibility;
- a floating Assistant popup whose URL, model, and API key are typed desktop
  settings. Assistant HTTP and chat JSON stay in desktop C++; the separate
  same-user local control transport contains no assistant state; QML only presents.

QML sends only intents to desktop-owned C++ presenters and observes immutable,
revisioned view state. Visible controls use GeoControls (buttons, labels, list
items, segmented switches, status bar, edit-panel lamps, and file dialog).
Develop section lamps bind identity/active/bypass to presenter state; a bypass
keeps stored parameters and writes `operation.enabled = false`. Gallery/`Image`
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
Recipe-style CLI keeps the schema-v1 asset/input application form and accepts
an explicit target Recipe for schema-v1 or schema-v2 application; selective
styles without a target Recipe fail structurally rather than resetting omitted
values.
The read-only catalog Develop probe applies strict numeric field overrides to a
current or synthesized baseline recipe, calls the non-persistent interactive
preview contract, reports display-referred pixel statistics, and verifies that
the stored recipe and preview records are unchanged before returning. Optional
`--output` encodes those in-memory RGB8 pixels to a throwaway PNG through the
atomic no-replace byte writer; it must not create a preview record. Recipe owns
the closed `--set` inventory (`list_develop_set_fields`) so CLI
`develop-fields` / `catalog fields` cannot drift from
`apply_develop_field_strict`. Canonical-mask `--set` names remain prefix-based.

CLI is the required machine-automation and acceptance client. Explicit
catalog/asset commands remain authoritative for stored state. Selection-relative
automation uses `ravo-studio-control/v1`: `ravo_control` discovers live
same-user sessions, desktop C++ publishes an immutable revisioned snapshot, and
mutations carry the observed session/selection revisions plus asset ID before
they enter `StudioCommandController`. The snapshot contains current/saved
canonical recipes and baseline-relative modified operations, never QObjects,
SQL handles, engine buffers, or assistant settings.

`ravo studio preview` and `studio develop --output` keep large bytes outside
the socket. CLI opens the named catalog through the ordinary composition,
renders the snapshot's exact recipe through the non-persistent preview path,
rechecks selection and recipe revision, then atomically publishes a no-replace
PNG with MIME, dimensions, profile, SHA-256, and caller-owned lifecycle. Local
descriptors and sockets disappear with their Studio owner; unreachable crash
residue is ignored. MCP may later project the same snapshots, commands, and
immutable image results, but cannot become a second state, renderer,
permission, or business-policy owner (ADR-0090). Process lists, logs,
accessibility state, and screenshots remain non-authoritative.
Each connected CLI request completes a bounded local-socket disconnect before
returning; an unsettled handle is aborted. Windows may report a still-live named
pipe as server-not-found or connection-refused while the prior instance is
being recycled, so only those two connect errors retry the same server name
within the caller's existing timeout. Other errors fail immediately, and no
alternate descriptor or process is guessed. An explicit session ID reads its
validated descriptor directly and lets the requested method prove liveness,
avoiding a redundant ping/request pipe pair.

Original-copy export is separate from pixel encoding. CatalogService passes one
explicit local source and destination to a bounded 64 KiB streaming owner,
which creates an exclusive adjacent temporary, synchronizes it, and atomically
publishes with no replacement. Existing or racing targets win. The service
copies exact media bytes only: it neither mutates the source nor creates XMP or
inherits source mode, timestamps, or xattrs. Stable cancellation and I/O errors
identify both paths.

Encoded pixel output uses the same private destination primitives without
entering the original-copy source stream. A complete immutable byte vector is
written in 64 KiB chunks to an exclusively created adjacent temporary,
synchronized, closed, and atomically moved with no replacement; an empty vector
publishes an exact empty file. Existing, symlink, non-regular, or racing targets
win, and stable cancellation or I/O failures identify the output and stage.
The legacy-compatible `path` context remains alongside the explicit `output`.
This primitive does not synchronize the parent directory or own codec metadata,
path templates, batch scheduling, or sidecars. CatalogService owns the higher
I14 batch policy: strict flat `{stem}`/`{asset_id}`/`{sequence}`/`{ext}`
expansion, complete known-conflict preflight, ordered calls to the same
no-replace item owner, and explicit non-rollback partial-delivery context.
CLI and Studio project that contract without expanding paths themselves. See
[ADR-0032](adr/0032-encoded-byte-publication-contract.md) and
[ADR-0068](adr/0068-typed-batch-export-storage.md).

## Current non-goals

- CatalogService owns local JPEG/PNG/TIFF/original-copy export. Pixel exports
  embed the recipe-declared RGB profile. JPEG requests own typed quality
  5–100/default 95 plus automatic or explicit 4:4:4/4:4:0/4:2:2/4:2:0 sampling;
  PNG requests own typed 8/16-bit depth and compression 0–9/default 5, while
  TIFF requests own typed uint8/uint16/float16/float32 sample state plus
  none/Deflate/Deflate-predictor, level 1–9/default 6, and conditional grayscale.
  `catalog export` projects those values through JPEG-only quality/subsampling
  flags, PNG-qualified flags, and TIFF-qualified flags including resolution. Catalog maps PNG16 and matching TIFF sample
  requests to engine-owned RGB16 or finite RGB float; JPEG/PNG8/TIFF uint8
  stay on RGB8. Rendered JPEG/PNG/TIFF embed the ADR-0038 Catalog-owned public
  metadata snapshot before publication, including validated capture time,
  source UTC offset, and GPS from Catalog schema v5. Explicit source metadata
  refresh, privacy stripping, no-automatic-sidecar policy, recipe styles, and
  typed batch/path-template storage are accepted. TIFF multipage masks and
  shared old export consumers remain out of scope. Studio collects one explicit
  format plus matching typed options, including rendered long-edge `max_edge`
  (0 keeps the current size; a positive value never enlarges, ADR-0113), before
  the native file/folder dialog; it does not infer format from a localized filter.
- Ravo does not implement every historic blend/operation or
  old-catalog migration.
- GPU is an Engine QRhi adapter (ADR-0133/0134). CPU remains the correctness
  reference. Preview keeps unmasked Exposure, light controls, Lab USM, and
  Sigmoid on one GPU SSBO session; other RGB ops stay CPU. Bayer window RCD
  for ROI 1:1 is GPU when a compute backend exists; PPG and export stay CPU.
  Do not reuse 0.9 OpenCL or add a silent CPU fallback.
- Do not freeze APIs for networks, cloud sync, public plugin ABI, or a complex
  query language without consumers.
- Do not modify frozen 0.9 to call Ravo or let Ravo production call the frozen
  application.
