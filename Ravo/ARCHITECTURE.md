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
first raster path. LibRaw, LittleCMS, Exiv2, and platform APIs likewise do not
cross a port. LittleCMS and Exiv2 are linked only by `ravo_engine`; public
colour and exposure-analysis state owns ICC bytes, matrices, or metadata values
instead of third-party handles. Qt value types may be used inside a target with
a clear benefit, but recipes, CLI JSON, catalog schema, and public persisted
contracts must not serialize Qt/C++ object memory layout.

Exiv2, LensFun, LibJpegTurbo, and LibTIFF are pinned migration source roots.
Configure validates the exact materialized sources. The accepted engine-private
RAW metadata adapter is the only Exiv2 consumer; other roots do not link a
product target until their corresponding lens-database or codec adapter is
accepted. Ravo does not expose Exiv2's upstream CMake options.
`ravo_add_private_exiv2()` compiles one frozen private profile:

| Surface | Frozen value |
| --- | --- |
| Linkage | static `exiv2lib`, `EXCLUDE_FROM_ALL` |
| Read path | filesystem `ImageFactory::open`, plus in-memory TIFF from PNG `eXIf` |
| Containers | BMFF on (CR3); Exiv2 XMP SDK, PNG codec, video, webready/curl, Brotli, NLS, inih, and Nikon lens data off |
| Extra targets | CLI, samples, unit/fuzz tests, and docs off |

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
discarding the prior translator, synchronously persists a normalized value,
then asks the QML engine and command controller to retranslate. A malformed
stored value is removed and falls back to English; a package or settings-write
failure is reported and leaves active state unchanged. Source TS catalogs and
the Chinese translation memory are repository assets; CMake validates and
compiles them to build-local QM files, which are the only files deployed with
Studio. This is the only persistent Studio preference: view controls remain
session state, and catalog, service, recipe, export, task, and engine values
stay in typed owning contracts. No old configuration key is read (ADR-0066).

Gallery grid schedules only `kThumbnailMaxEdge` browse thumbnails, never a
1600px processed preview merely for a selected grid item; all scopes are
calculated from that thumbnail. Loupe/develop requests full decode. On
open/import, presenters seed ready cache paths from the preview table so grid
delegates do not saturate a single-thread queue. The Library panel separately
shows import and preview-build progress. After system file/folder selection,
scanning and import run on workers and each successful photo immediately
appears in the grid with a browse thumbnail.

Grid cells fit available width in the 120–320 range and have a vertical scroll
bar. `positionViewAtIndex` runs only when the selected item leaves the
viewport. C++ passes `--catalog <library.sqlite>` to the presenter; QML opens
it at session start rather than a default library.

Library filtering is a value boundary, not UI-built SQL. `LibraryQuery` owns
validated review, folder/tag, text/media/edit, camera/capture/numeric ranges
and stable sort state; CatalogService rejects an invalid query before loading
and delegates matching/sorting to domain. Studio keeps only the active query,
reloads the visible model through the service, and forwards changes through
the command registry. It intentionally records no recent-query history. QML
never sees catalog columns or legacy rule strings.

Reusable style/preset state reuses Recipe rather than introducing another
parameter model. A RecipeStyle replaces the asset with a fixed template
identity; application restores only the target identity and then enters the
same Engine validation, Develop, Catalog history, undo, preview, and export
lifecycle. Filesystem adapters own bounded reads and complete writes; QML only
opens save/open dialogs and forwards paths.

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
rotated image. Selection and crop-handle dragging change in-memory parameters
only; release writes the recipe through the same Develop commit path as the
right-panel controls. History restore, Original, and snapshot restore use that
path with a session undo step and without appending a new history row. Export
continues to use CPU straighten, not an engine GPU adapter. Deleting a photo
normally removes only the catalog record
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
(history/snapshot); v5 adds seven nullable capture-time/GPS columns
(`captured_local_exif`, `captured_subsecond_digits`,
`captured_utc_offset_minutes`, `gps_latitude_e6`, `gps_longitude_e6`,
`gps_altitude_magnitude_mm`, `gps_altitude_ref`). The SQLite adapter enables
WAL, `synchronous=NORMAL`, and `busy_timeout`. New catalogs and migrations use
transactions, and an unknown higher schema version fails fast.

The repository adapter atomically publishes the current recipe row (or baseline
clearing), optional deletion of history rows newer than a cursor, automatic
history, and catalog revision. A history preview writes the current recipe with
`RecipeHistoryWrite::kUnchanged` so the stack stays intact; a later parameter
edit discards `seq` greater than the cursor in the same transaction before
appending. Any failed step rolls back everything; services neither compensate
writes nor expose a partially committed recipe. The first version does not
read or migrate a frozen 0.9 catalog. Future compatibility requires an
independent product decision, backup/rollback, and fixtures.

### Import

`ImportRequest` carries catalog ID, file/directory input, recursion/format
policy, resource budget, cancellation token, and correlation ID.
`ImportItemResult` returns imported, duplicate, unsupported, or failed for
each input; partial success must not lose failure detail.

First-version import only registers sources: it never copies, moves, renames,
rewrites metadata, or deletes them. Codec probing confirms a format; extensions
are only candidate filters. One LibRaw open reads RAW capture metadata and
embedded JPEG, then persists a `kThumbnailMaxEdge` browse thumbnail. A RAW
suffix or TIFF RAW container with no embedded JPEG must first-frame-decode
before `commit_imported_asset`. Import does not perform a 1600px full decode
when a browse JPEG exists. It validates trusted metadata before transactionally
publishing an asset. Cancellation stops undispatched work; committed results
remain valid. Missing, directory, unrecognized, unpack-failed, oversized, and
X-Trans/non-Bayer full-decode inputs fail with stable `reason` context.

### Preview

`PreviewRequest` explicitly carries asset ID, target pixel dimensions,
orientation/colour policy, backend, memory/thread budget, cancellation token,
and request revision. `PreviewResult` returns either trusted profile-labelled
RGB memory pixels or a read-only profiled cache resource, otherwise a
structured failure.

RAW uses the Ravo CPU engine, while JPEG/PNG/TIFF use the raster adapter. Both
share orientation, colour, alpha, scaling, finite-value, and error contracts.
Catalog import fully decodes JPEG/PNG/TIFF before publication and reuses that
RGB8 thumbnail. A recognized raster error never becomes a RAW inspect except
`unsupported_tiff_raw_container`. First-frame RAW decode is 16-bit Bayer only;
`.dng` uses the same LibRaw path. X-Trans may browse from embedded JPEG.
Preview cache is atomically written outside the database, keyed by source
fingerprint, target dimensions, and contract version. A cached PNG without the
8-byte PNG signature is a miss and is deleted. Corrupt or missing cache
rebuilds from the read-only source. The filesystem adapter serializes access
and owns a 512 MiB hard byte budget. Valid hits refresh persistent access time;
startup orders that time and key deterministically, and commits evict the
least-recently-used PNGs before atomic publication. Database preview rows are
hints and may outlive eviction. Single entries above budget and real directory,
measurement, timestamp, or removal errors fail structurally. Catalog checks
cancellation after encode and before commit. `CatalogService::close` drops the
repository, raster, cache index, and decoded RAW/working buffers while bounded
disk entries remain available to reopen (ADR-0067).

Import and Gallery use browse cache. One LibRaw open reads RAW metadata and
embedded JPEG, then writes a PNG at `kThumbnailMaxEdge` under the
`embedded-jpeg` key digest. It is not editable scene-linear data. Loupe,
Develop, scopes, export, and `request_preview` with
`prefer_embedded_preview=false` use preview contract v7: full CPU
decode/render followed by the `ravo.display.sigmoid` baseline at the end of
the scene-linear buffer and the recipe-owned output profile. The cache types
must not share a digest. Without
embedded JPEG, browse fails open to full decode and never writes an empty image.
Cached built-in sRGB PNGs contain one standard `sRGB` chunk; other RGB outputs
contain one `iCCP` and no conflicting `sRGB`. Preview v7 rebuilds prior cache
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
[ADR-0027](docs/adr/0027-radiance-rgbe-decoder-contract.md).

### Recipe and operation

Canonical recipes, operation descriptors, `RenderRequest`/`RenderResult`,
and the explicit colour contract remain valid. The first viewer needs only the
minimal CPU chain that yields a trusted preview; later editing UI maps the
versioned schema only and owns neither a second algorithm nor history format.

S3.1 adds a recipe-owned canonical mask graph under
[ADR-0043](docs/adr/0043-canonical-mask-graph-foundation.md). Each immutable
node has its own schema version and typed all/linear-gradient/circle/rotated-
ellipse/parametric/group payload; v1 identity `all` upgrades on read.
[ADR-0045](docs/adr/0045-studio-mask-overlay-group-path.md) extends that graph
with path/brush kinds, preview overlay, and owned group authoring. Recipe
parsing, upgrade, deterministic serialization, and DAG validation are the only
owners of IDs, references, opacity/inversion, bounds, topology, and version
rules. S3.2's [ADR-0044](docs/adr/0044-studio-canonical-mask-authoring.md)
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
implicitly. It owns only an alpha result and depth-first group
accumulator/child scratch through RAII; stored and expanded graph-work limits
bound shared-DAG recomputation. Invalid ROI/stride/sample count,
non-finite values, missing parametric operation output, cancellation, overflow,
or allocation returns a structured failure before publication. Its only blend
is normal `input + alpha * (operation_output - input)`, with exact alpha 0/1
source selection. Raw budgeting uses saturating arithmetic for the masked
snapshot/output/alpha/evaluator peak.

Only Color Harmonizer and Graduated ND currently dispatch a canonical mask.
Their unmasked path remains bit-identical; a masked operation retains a local
pre-operation image, produces local output, evaluates alpha, then mixes before
the result becomes the next recipe input. `DevelopParams` keeps the typed graph
and both attachments, including disabled/default instances, so live preview,
save/reopen and ordinary Develop edits cannot baseline-elide a loaded mask.
Studio projects two read-only mask-editor maps and forwards numeric intents to
the recipe helper through the existing Develop preview/commit path. Unshared
Studio-owned leaves and groups are editable; external IDs and shared
attachments are visibly read-only and may only be explicitly detached. Studio can
show a preview-only yellow overlay of the named attachment, author owned group
children, and author path/brush leaves. The Graduated ND density gradient
is still its independent operation formula, not an alias for generic mask
geometry. Historic blend modes and leftover GTK mask-manager consumers remain
outside this tranche.

`ravo.core.tonecurve` implements the frozen C RGB-linked default:
Lab D50 → ProPhoto, `preserve_colors=average`, a 0–1 point list, and
`interpolation=monotone_hermite`. `working_space=lab|xyz|lab_independent`
is an explicit C mode; Inspector forwards points and recipe/engine evaluates.

The lightweight P1 global controls do not stand in for the later full-module
migration queue. Contrast, Saturation, and Vibrance use the darktable basic-
adjustments CPU response;
Lab-backed controls share the engine's D50 working conversion; hidden Grain,
Vignette, Bloom, and Soften defaults use the corresponding source parameters,
while Sharpen is now independently accepted below. Studio presents
darktable-equivalent soft ranges while recipe validation retains the explicit
hard bounds. Full `shadhi`, `gamma`, `grain`, `vignette`, `bloom`, and `soften`
capability acceptance remains governed by the root migration queue rather than
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
exposure owners. [ADR-0024](docs/adr/0024-exposure-analysis-and-metadata-contract.md)
freezes these boundaries.

`ravo.display.sigmoid` v1 is the sole default display transform:
`working_space=linear_srgb`, `color_processing=per_channel`, middle-grey
contrast, skew, Standard SDR black/white target, and hue preservation. It is
the RAW baseline and final scene-referred operation before output-profile
encoding. RAW
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
sub-ROI evaluation, and enabled geometry after Canvas reject explicitly; G1/G4/
G6 must supply a composed transform before that ordering is accepted.

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
[ADR-0026](docs/adr/0026-colorchecker-calibration-contract.md)
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
[ADR-0029](docs/adr/0029-colorcorrection-contract.md)
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
[ADR-0031](docs/adr/0031-colorcontrast-contract.md) freezes this boundary.

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
[ADR-0045](docs/adr/0045-studio-mask-overlay-group-path.md). Remaining blend
modes and leftover GTK mask-manager consumers stay later work and do not
authorize C15. The frozen `iop/colorharmonizer.c` owner is retired.
[ADR-0035](docs/adr/0035-colorharmonizer-core-contract.md) freezes the core;
[ADR-0041](docs/adr/0041-colorharmonizer-smoothing-zero-vertical-slice.md)
extends it with the first product surface; [ADR-0042](docs/adr/0042-colorharmonizer-canonical-roi-recursive-smoothing.md)
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
OpenCL owner enters Ravo. [ADR-0055](docs/adr/0055-colorreconstruction-bilateral-grid-contract.md)
freezes these boundaries.

`ravo.detail.sharpen` schema v2 is the accepted scale-aware D50 Lab L* USM.
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
[ADR-0056](docs/adr/0056-source-exact-lab-sharpen.md) freezes these boundaries.

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
other unaccepted consumers. [ADR-0057](docs/adr/0057-source-linear-dark-channel-dehaze.md)
freezes these boundaries.

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
- `ListAssets` / `ObserveCatalog`: return stable order and live
  `CatalogSnapshot.revision` without SQL cursors. Studio's presenter polls
  that revision on a 1 s timer and reloads listing/recipe/history when another
  client commits; QML does not poll or read the database;
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

The Ravo Studio first version owns:

- creating/opening catalogs and choosing files/directories;
- Gallery list states: loading, ready, missing, unsupported, and failed;
- selection, Gallery grid/loupe and Edit panes, fit, 100%, and pan. Grid and
  filmstrip use whole-image containment with letterbox number, rating,
  format/dimension, and flag overlays. Gallery's left rail is the folder tree,
  tag filter, and Import/Export; Edit's left rail is the selected photo's
  recipe history and snapshots. Clicking a step previews that recipe and dims
  newer rows; a subsequent parameter edit discards the dimmed rows. The default
  Edit grading stack is White Balance, Light, Color Equalizer, and Color
  Balance RGB wheels; overlapping Lab color tools stay under Color · Advanced
  (ADR-0082). Color Equalizer exposes eight named bands; Bayer white-balance
  pick writes manual temperature coefficients (ADR-0083);
- shared scopes above the right Gallery/Edit panel, remaining visible while
  the Edit list scrolls: frozen 256-bin RGB Histogram, overlaid Waveform, RGB
  Parade, fixed linear D50 CIE u*v* Vectorscope, and Waveform/Vectorscope
  Split. Engine owns pixels; QML owns only grids and selection;
- progress, cancellation, and recoverable-error presentation;
- window, focus, keyboard, HiDPI, and basic accessibility;
- a floating Assistant popup whose URL, model, and API key are typed desktop
  settings. Qt Network and chat JSON stay in C++; QML only presents.

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
The read-only catalog Develop probe applies strict numeric field overrides to a
current or synthesized baseline recipe, calls the non-persistent interactive
preview contract, reports display-referred pixel statistics, and verifies that
the stored recipe and preview records are unchanged before returning. Optional
`--output` encodes those in-memory RGB8 pixels to a throwaway PNG through the
atomic no-replace byte writer; it must not create a preview record. Recipe owns
the closed `--set` inventory (`list_develop_set_fields`) so CLI
`develop-fields` / `catalog fields` cannot drift from
`apply_develop_field_strict`. Canonical-mask `--set` names remain prefix-based.

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
[ADR-0032](docs/adr/0032-encoded-byte-publication-contract.md) and
[ADR-0068](docs/adr/0068-typed-batch-export-storage.md).

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
  format plus matching typed options before the native file/folder dialog; it
  does not infer format from a localized filter.
- The first version does not implement every historic blend/operation or
  old-catalog migration.
- Do not implement GPU before CPU correctness and viewer resource gates.
- Do not freeze APIs for networks, cloud sync, public plugin ABI, or a complex
  query language without consumers.
- Do not modify frozen 0.9 to call Ravo or let Ravo production call the frozen
  application.
