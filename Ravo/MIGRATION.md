# Ravo Migration Policy

## Goal

Ravo will ultimately replace `legacy/src/`. Catalog/import/viewer and Basic
Develop are already implemented in Ravo. The next stage follows the user-value
queue in the root
[`TODO_LEGACY_MIGRATION.md`](../TODO_LEGACY_MIGRATION.md), bundling the legacy
owners required for one user-visible outcome instead of walking old source
order. Delete each corresponding old owner only after it is “Ravo accepted.”
New ownership lives only in `Ravo/`. Version 0.9 remains prohibited from
configuration, compilation, and execution.

Under [ADR-0015](docs/adr/0015-migrate-all-non-ui-algorithms.md), every
remaining non-UI image algorithm is in scope for C++20 migration. Defaults
remain unchanged, but substitute algorithms are not permanent leftovers. GTK,
Lua, dynamic ABI, and OpenCL are ultimately removed rather than ported.

## One-way boundary

Allowed:

- Ravo tests may read source images, XMP, and goldens in `legacy/tests/`.
- Ravo may statically read `legacy/src/` source to study algorithms, catalog,
  import, and UI call chains.
- Ravo may directly consume pinned third-party dependencies through FreeCM.
- Ravo may implement its own SQLite catalog, import services, preview pipeline,
  and C++-backed Qt Quick/QML desktop.

Forbidden:

- Ravo production targets must not include `legacy/src/` private headers, link
  `libdarktable`, load old IOPs, or read global `darktable` state.
- Do not configure, compile, or run the old CLI, old CTest, `legacy/tests/run`,
  or old packaging targets.
- Do not make the frozen application call Ravo or add adapters, entry points,
  or build dependencies to the old GTK application for migration.
- Do not generate a live oracle through the old CLI or wrap old catalog/GTK
  types as new APIs.
- Do not let QML/JavaScript issue SQL, decode images directly, own engine tasks,
  or duplicate services business logic.
- Do not use permanent shims, silent fallbacks, or copied implementations to
  hide undecided data compatibility.

Production dependencies remain completely independent; neither `src → Ravo`
nor `Ravo → src` may exist.

## First-version migration unit

The current migration unit is a vertical slice observable by both users and
automated tests:

1. **Evidence:** list the frozen owner, input formats, data/thread/error
   behavior, and read-only fixtures.
2. **Define the contract:** document catalog schema, Asset/Import/Preview value
   types, ports, lifecycle, cancellation, and failure semantics.
3. **Implement the Ravo owner:** domain/services/engine/adapters/desktop each
   own only their layer's responsibility.
4. **Validate without UI:** service integration completes create → import →
   preview → reopen.
5. **Desktop acceptance:** Ravo Studio creates/opens, imports, lists, selects,
   and views.
6. **Resources and recovery:** cover duplicates, corruption, missing files,
   cancellation, disk/database failure, close, and restart.
7. **Record status:** update roadmap, ADR, ledger, actual validation, and
   untested platforms.

A change need not complete an entire vertical slice, but neither “target was
created,” “database opens,” nor “window is visible” alone counts as a completed
first version.

## Subsequent algorithm migration unit

For an editing capability, shared algorithm, or operation, use this order:

1. Inventory old owner, registration, callers, parameters, threads, cache, GPU,
   resources, and fixtures.
2. Freeze real RAW/XMP/pixel/metadata evidence statically; do not run the old
   CPU path.
3. Define canonical schema, input/output, ownership, cancellation/failure, and
   incompatibilities.
4. Reproduce the frozen C default CPU mathematics and behavior: formulas,
   color space, filters, and default modes. Remove GUI, old lifecycle, global
   state, dynamic ABI, and OpenCL types. A simplified substitute algorithm
   (such as HSL for UCS, neighborhood averaging for opposed reconstruction, or
   a three-level Gaussian for a-trous Y0U0V0) is not acceptable as a completed
   migration or reason to delete old code.
5. Run unit, synthetic, old-mapping, real-RAW/golden, error/cancellation, and
   resource validation.
6. Make CLI and Studio supported consumers through the same services/engine.
7. Under the active root migration TODO, delete old owner after Ravo accepts
   the item and synchronize freeze/inventory checks.

## Definition of “absorbed by Ravo”

“Ravo accepted” and “old implementation removed” are distinct states. A
capability is final only when all of these are true:

- Ravo is the supported implementation and owns data, CPU/UI behavior, error,
  cancellation, and resource contracts.
- Promised fixtures, service/desktop tests, and platform gates meet their
  thresholds.
- Historical-data migration or an explicit rejection strategy is recorded and
  tested.
- Release transition is complete and production builds have no second reachable
  old implementation.
- The corresponding `src` source, build wiring, registration, configuration,
  resources, and entry points are removed under the active migration TODO.
- Documentation, search, and the link graph have no accidental consumers or
  reverse dependencies.

## Migration order

1. Keep the accepted catalog/review/develop/export baseline regressible.
2. Follow the root TODO's user-value queue strictly: finish the active tranche,
   then take the highest-priority Ready outcome. Do not select the next IOP or
   cleanup merely because its legacy row comes next.
3. Bundle the mask, RAW/color/geometry/output, service, data, and UI owners that
   one outcome genuinely requires. Pull a lower-priority foundation forward
   only as a named dependency; each owner keeps its own evidence and deletion
   gate.
4. Apply data-safety, cache/resource, accessibility, and three-platform gates
   continuously. Old styles/catalog/XMP compatibility still needs a separate
   dated product decision, and optional GPU remains behind CPU acceptance.
5. Delete an old owner only after its capability is Ravo accepted. After the
   user-value queue is empty, demonstrate release transition/rollback, then
   handle explicit leftover archiving and final cleanup.

## Explicit non-algorithm leftovers

The following old implementations are deleted rather than ported. Shared files
that still have algorithm consumers wait until their active TODO items are
accepted:

- GTK Lighttable/Darkroom, dtgtk, Bauhaus, and old module layout/UI ABI;
- Lua, dynamic IOP loading, and historic plugin ABI;
- 0.9 OpenCL; Ravo GPU does not reuse its API;
- old catalog/styles binaries and unproven complete XMP-history replay;
- map, tethering, print, slideshow, and remote publishing.

`filmicrgb`, `agx`, `colorzones`, other diagnostic calculations, creative/
repair modules, and all other remaining IOPs remain algorithm-migration
candidates; they must not become empty shells or be deleted en masse before
their active TODO acceptance. The retired final-display channel and mask
branches are explicitly unsupported presentation adapters. Sigmoid and
`colorequal` remain the default display transform and default HSL partition
respectively.

## Migration ledger

| Capability | Old owner | Ravo owner | Status | Current evidence / next gate |
| --- | --- | --- | --- | --- |
| Basic errors/cancellation | `src/common`, `src/control` | foundation | In progress | cancellation/deadline and SerialExecutor submit/wait_idle are tested |
| Recipe/schema | IOP params/XMP | recipe | In progress | versioned round-trip and strict mappings include S3.1 typed mask-schema-v2 graph with v1 `all` upgrade, deterministic serialization, bounded DAG validation, S3.2 strict Studio-owned leaf authoring fields/IDs, leftover flip v2 orientation→rotate/flip (ADR-0048), leftover crop v1–v3 box→x/y/width/height (ADR-0049), leftover ashift v4/v5 generic rotation/lens-shift/shear→canonical Perspective (ADR-0096; ADR-0050 remains the earlier rotation-only boundary), leftover rgblevels v1→`ravo.color.rgblevels` (ADR-0051), leftover rgbcurve v1 including middle-grey uncompensate→`ravo.color.rgbcurve` (ADR-0052/0053), and leftover Bayer/X-Trans rawdenoise v2→`ravo.raw.denoise` (ADR-0054/0096); legacy XMP remains evidence-bound |
| RAW inspect/decode | imageio/LibRaw + common/dng_opcode | engine + private LibRaw/DNG adapters | In progress | First-frame Bayer/X-Trans inspect/decode and `.dng` suffix routing include bounded owned OpcodeList2/3 parsing, tiled RCD/Markesteijn defaults and explicit PPG/Markesteijn modes. Inspect exposes CFA/default mode; GainMap, WarpRectilinear and FixVignetteRadial execute in DNG order. Malformed/unknown mandatory state, unsupported CFA and sensor/mode mismatch fail without fallback (ADR-0047/0096). Synthetic, real RAW/DNG/RAF, error/cancel/immutability, recipe/CLI/Catalog/Studio and cache contracts cover the path. Dual/green matching, three-platform evidence and shared leftover deletion remain open |
| CPU preview/pixelpipe | `src/develop` | engine | In progress | bounded PNG, cancellation, full exposure, Color Checker, affine D50 Lab Color Correction, bounded/unbounded D50 Lab Color Contrast, profile-aware dt-UCS/RYB Color Harmonizer including canonical-ROI recursive positive smoothing, leftover RGB levels LUT (ADR-0051), and canonical mask alpha/normal mix with Studio overlay are tested; complete pixelpipe/geometry ROI remains unfinished |
| SQLite catalog | common/database | domain + SQLite adapter | In progress | schema v6 create/reopen/migrate/newer-version rejection tested; v5→v6 adds transactional per-asset recovery generations while preview state stays rebuildable. Additive capture datetime/GPS columns stay NULL on old rows; catalogs that claimed v5 with the rejected signed `gps_altitude_mm` column are repaired on open to magnitude/ref. Tags, writable metadata, history, recovery restart, and verified preview-free database snapshots are tested; no old-catalog migration (ADR-0097) |
| Reference-only import | common/imageio/import | services + adapters | In progress | PNG/JPEG/TIFF and LibRaw RAW (including ARW/DNG suffix) plus recursive directory import. Catalog fully decodes JPEG/PNG/TIFF before insert and only TIFF RAW containers may fall through to LibRaw. A RAW without embedded JPEG unpacks before publication. JPEG/GIF/WebP/TIFF plugin targets remain required. X-Trans full decode and leftover wrappers remain later |
| Preview cache | mipmap/cache/imageio | services + adapters | Ravo contract accepted; shared cleanup remains | atomic PNG cache outside the library; 512 MiB hard byte budget; deterministic mtime/key startup order and in-process LRU promotion; pre-commit eviction; corrupt-signature cleanup; cancellation immediately before publication; close/reopen rebuild. Shared old cache/imagebuf owners remain until their develop/imageio/job/GTK consumers reach zero (ADR-0047/0067) |
| Gallery/viewer | lighttable/darkroom | desktop + services | In progress | Studio can create/open/import/fit/fill/100%; long-list resource gate remains |
| Photo navigation | `libs/navigation.c` | Studio presenter + QML Flickable/navigator | Old implementation removed | Fit/Fill/Actual/custom 0.1–8 zoom, wheel steps, inspect magnifier click-to-1:1 with restore of the last non-Actual view, GPU scale/pan animation on inspect click, bounded pan, normalized navigator seek, crop ownership, grid reveal, and active-asset/mode/zoom recenter lifecycle are explicit and tested. Review notifications for the same asset preserve pan. No viewport telemetry or old config compatibility is persisted (ADR-0060/0076) |
| Preview scopes | `libs/histogram.c`, `libs/scopes*` | engine statistics + Studio projection | Old implementation removed | exact RGB8 preview validation; 256-bin Histogram; 160-tone/360-bin HLG Waveform and Parade; fixed 384-square linear D50 CIE u*v* Vectorscope with 2×2 sampling; max-preserving Waveform/Vectorscope Split. All use displayed preview/thumbnail pixels and revisioned image URLs. AzBz/RYB/log/harmony/profile/picker/exposure-drag state is explicitly unsupported (ADR-0061) |
| Library query/filter | `libs/recentcollect.c`, `libs/filtering.c`, `libs/filters/*`, shared collection state | domain `LibraryQuery` + services + Studio | GTK filter owners removed; shared core remains | strict current-query validation covers review/folder/tag/text/media/edit/camera/capture/numeric predicates and stable import/capture/name/rating/size sorting. Invalid ranges/text/enums fail before listing. Studio exposes common predicates through commands. Persistent recent-query history and legacy-only bookkeeping/group/module fields are explicit unsupported product decisions (ADR-0059). Shared `common/collection*`, proxies/config/manual state and S8 deletion wait for remaining consumers |
| Catalog metadata/workflow | common/libs | domain + services | Ravo contract accepted; shared cleanup remains | Unicode tag filtering, catalog-only writable title/creator/copyright, bounded read-only capture Exif, explicit source refresh with asset+capture+revision transaction, persistent recipe history/snapshot, session-only same-control history/Undo coalescing, and typed full/no-location/none rendered-export privacy. Originals and adjacent sidecars stay unchanged; catalog-owned recovery mirrors and verified backups are derived from SQLite, and original-copy remains exact. Faces/map/GPS writeback and opaque packet copy are unsupported. Old `libs/tagging.c`, `metadata*.c`, `history.c`, `snapshots.c`, and `copy_history.c` are deleted; shared Exif/tag/image/crawler/storage writers wait for S7/S9/J2/I10–I14 zero consumers (ADR-0011/0064/0097) |
| Asset mutation lifecycle | `control/jobs/image_jobs*` + batch image jobs | repository + CatalogService + Studio commands | Ravo contract accepted; old wrapper blocked | duplicate import is revision-neutral; missing/corrupt/unsupported/cancelled input publishes no asset; catalog-only removal preserves the original and clears caches; asset cascade+revision is one SQLite transaction; disk deletion uses adjacent quarantine and rollback with recoverable final-unlink diagnostics. Trigger tests prove row/revision/original rollback. `common/mipmap_cache.c` still consumes the old speculative load wrapper, so J5 source deletion waits for S11/J4 zero-consumer work (ADR-0062) |
| Sidecar policy | `control/jobs/sidecar_jobs*` + direct old writers | explicit CLI legacy-XMP adapter + catalog-owned recovery store | Product policy accepted; old wrapper blocked | no automatic adjacent sidecar attach/read/write/watch or compatibility config; SQLite is the edit authority. Explicit `recipe import-xmp` writes a new recipe only, including fail-closed Lightroom CRS mapping (ADR-0086); rendered XMP is newly embedded; original-copy is exact bytes. Schema-v6 recovery generations derive bounded `.ravo.json` mirrors under the catalog support directory and never act as implicit input. Source/adjacent sidecar hashes remain unchanged. Old `common/darktable.c`/`common/image.c` and direct writers block wrapper deletion until S7/S9/J2/D0 cleanup (ADR-0063/0097) |
| Recipe styles/presets | `libs/styles.c`, shared styles/presets/history | recipe artifact + CLI + Studio | GTK style module and bundled examples removed; shared cleanup remains | `.rstyle.json` v1 is a bounded complete Recipe template with exact placeholder identity. Create/validate/apply preserves operations, masks, bypass and profiles; applied recipes use normal history/undo. Studio lists imported Lightroom CRS XMP and Ravo styles above History from a `Ravo Presets` folder beside the library (ADR-0086). Unknown/newer/malformed/oversized and legacy dtstyle reject; no partial module drop. All 24 `.dtstyle` examples and their exclusive translation generator are retired; shared common styles/presets/undo/history remain S10/D0 consumers (ADR-0065/0072) |
| Typed product settings | `control/conf*`, `control/settings.h`, crawler/preferences | desktop language manager plus typed owners | Ravo contract accepted; shared cleanup remains | `desktop/language` is the sole persistent Studio preference, normalized through the versioned nine-locale manifest; corrupt stored state repairs synchronously and persistence/package failure leaves active state unchanged. View state is transient and recipe/catalog/export/task values remain typed. No old keys or automatic crawler migrate. Old shared files wait for zero consumers; U5 GTK windows may retire (ADR-0066/0093) |
| Mask/blend/operations | develop/iop | recipe + engine | In progress | S3 owns typed versioned all/linear-gradient/circle/ellipse/parametric/path/brush/ordered-group DAGs, frozen group/parametric branch order, attached-frame pixel-centre ROI evaluation, normal mix, saturating masked memory accounting, and Color Harmonizer/Graduated ND Recipe/CLI/Catalog/Studio consumers including preview overlay and owned group-child authoring. Historic blend modes and leftover GTK mask-manager consumers remain; strict legacy mask/custom-blend/multi import still rejects |
| RAW highlight reconstruction | `iop/highlights.c` | `ravo.raw.highlights` | Old implementation removed | default Bayer opposed (`_process_opposed`); clip / reconstruct-color inpaint / LCh are explicit modes. Non-Bayer, raster, laplacian, and segmentation are structured unsupported states |
| RAW hot-pixel repair | `iop/hotpixels.c` | `ravo.raw.hotpixels` | Old implementation removed | Bayer four same-colour ±2 neighbours, `strength/2`, strict 4 / permissive 3, replacement with neighbor maximum; X-Trans/monochrome/raster are structured unsupported |
| RAW wavelet denoise | `iop/rawdenoise.c` | `ravo.raw.denoise` | Old implementation removed | schema v2 owns threshold plus four five-band curves. Bayer runs four 2×2 planes; X-Trans runs frozen nearest-neighbour RGB planes through the same square-root five-level hat transform and writes only matching sensels. Cancellation, memory, source ownership, recipe/CLI/Catalog/Studio and real Bayer/X-Trans references are covered (ADR-0054/0093) |
| RAW Bayer chromatic aberration | `iop/cacorrect.c` | `ravo.raw.cacorrect` | Old implementation removed | RawTherapee 128 tile/16 overlap, green/color-difference statistics, 3×3 median, full-image polynomial shift fit, ±3.99 interpolation, and avoid-color-shift; `cacorrectrgb` remains a separate leftover |
| Default denoising | `iop/denoiseprofile.c` | `ravo.detail.denoiseprofile` | Old implementation removed | schema v1 keeps Y0U0V0 variance stabilization, edge-aware à-trous decomposition, and BayesShrink. Generic Poisson `a=0.0001,b=0` is calibrated from bounded deterministic finest-band MAD; canonical scale selects visible bands, Radius owns dilation/coarse response, and Luminance/Chroma mix independent neutral/colour deltas. Finite/scale/cancellation/atomic output and four-RGB-plane-plus-bounded-sample resource gates are tested; preview v10 owns the correction. Camera profiles, NLMeans, bilateral, and RAW denoise remain separate (ADR-0094) |
| Camera noise calibration | vkdt `rawhist`/`nprof` research; no legacy runtime owner | foundation + engine + JSON adapter + services + CLI | Ravo contract accepted | strict v1 black-subtracted uint16 mean/variance/count input; bounded Theil–Sen/MAD plus weighted non-negative Gaussian/Poisson fit; canonical SHA-256 v1 profile; explicit atomic no-replace output, cancellation, malformed/outlier/tamper/reproducibility/source-safety tests. RAW measurement and denoiser profile lookup remain separate gates (ADR-0096) |
| Lens correction | `iop/lens.cc` | `ravo.geometry.lens` | Old implementation removed | explicit lensfun poly3/poly5 + linear TCA + manual vignette spline; lookup uses a versioned coefficient table and fails fast when unmatched. The lensfun source root remains the production database successor and was not pinned in this work |
| Color equalizer | `iop/colorequal.c` | `ravo.color.colorequal` | Old implementation removed | dt UCS 22 eight-node periodic RBF LUT; remains the default hue-partition operation. Color Zones is separately accepted by ADR-0073 |
| Color Zones | `iop/colorzones.c` | `ravo.color.colorzones` | Old implementation removed | schema v1 owns L/C/h selection, three 2–20-node D50 Lab curves, cubic/Catmull–Rom/monotone interpolation, 65,536-entry source quantization, strength, low-chroma blend, canonical mask, cancellation and memory contracts. Exact enabled 0022 v5 singleton/default blend maps; full FilmicRGB document remains negative. Recipe/CLI/Catalog/Studio/style/reopen/export pass; old kernel/config/icons are removed while shared spline/histogram/picker/order/manual owners remain (ADR-0073) |
| Monochrome | `iop/monochrome.c` | `ravo.color.monochrome` | Old implementation removed | schema v2 owns D50 Lab a*/b* filter, size, highlights, mix, source bit-fast-exp, canonical-scale bilateral base, envelope and neutral output. Ravo v1 amount upgrades to mix; masks, cancellation, finite/resource/source ownership, Recipe/CLI/Catalog/Studio/style/reopen/export pass. Exact enabled 0017 v2 singleton/default blend maps; full document remains negative. Old kernels/icons are removed; shared bilateral/picker/camera-mono/demosaic/order/manual owners remain (ADR-0074) |
| Split Toning | `iop/splittoning.c` | `ravo.color.splittoning` | Old implementation removed | schema v2 owns linear-Rec.709 shadow/highlight hue+saturation, pivot balance, compression and mix through shared source-order HSL conversion. Ravo v1 amount upgrades; masks, cancellation, finite/resource/source ownership, Recipe/CLI/Catalog/Studio/style/reopen/export pass. Exact enabled 0062 v1 singleton/default blend maps. Old kernel/icons are removed; shared HSL/picker/order/manual owners remain (ADR-0075) |
| Velvia | `iop/velvia.c` | `ravo.color.velvia` | Old implementation removed | schema v2 owns linear-Rec.709 strength and mid-tones bias with the frozen HSL-style low-saturation weight and per-channel clamp. Ravo v1 amount upgrades; canonical masks, cancellation, finite/source ownership, Recipe/CLI/Catalog/Studio/style/reopen/export pass. Exact enabled 0063 v2 singleton/default blend maps. Old kernel/icons are removed; shared order/module-group/manual owners and frozen evidence remain (ADR-0095) |
| 3D LUT | `iop/lut3d.c` | `ravo.color.lut3d` + private bounded `.cube` adapter | Old implementation removed | schema v1 owns explicit file, input/output colour spaces, tetrahedral/trilinear interpolation and strength after Velvia. Per-channel domains, full-content fingerprint, eight-entry immutable LRU, unbounded linear blend, strict parse/resource/cancel errors, Recipe/CLI/Catalog/Studio/reopen/export and source safety are tested. Legacy XMP external paths reject explicitly; pyramid/Hald/OCIO/CTL and masks are unsupported. The exclusive CPU/OpenCL owners are removed; shared order/module-group/manual names and frozen evidence remain (ADR-0096) |
| RAW white balance | `iop/temperature.c` | `ravo.color.temperature` | Old implementation removed | explicit `camera_cfa_or_linear_rgb` four-coefficient scaling; LibRaw as-shot/daylight metadata, manual, late-reference + explicit CAT. The old Kelvin/tint approximation, generic fallback, GTK picker/presets, and OpenCL are not ported |
| Exposure | `iop/exposure.c` | `ravo.core.exposure` + private RAW analysis/metadata adapter | Old implementation removed | v2 manual EV/black, optional bias/highlight-preservation compensation, and deflicker percentile-to-EV use one immutable original-RAW histogram/metadata snapshot; private Exiv2 never crosses the engine boundary. Missing tags, metadata-read failure, raster analysis, memory, cancellation, and finite-value states are explicit. All 158 XMPs freeze v5/v6/v7 final-revision and exact default-unmasked acceptance; masks/custom blend/multi reject structurally. GTK picker/GUI auto is not serialized math. Shared proxy/order names and `basic.cl` remain D0.4/S4/S14 cleanup, not runtime owners |
| Input colour profile | `iop/colorin.c` | `ravo.color.input` + private engine colour adapter | Old implementation removed | explicit decode profile state and working matrix; frozen matrix/shaper/unbounded/normalize/RAW blue paths plus private LittleCMS RGB/XYZ/Lab ICC transforms; raster ICC, external-profile cache invalidation, Studio reopen, and profile-labelled CLI/export; no missing-profile or generic-camera fallback |
| Unbreak input profile | `iop/profile_gamma.c` | `ravo.color.profilegamma` + private pre-input engine path | Old implementation removed | opt-in v1 log/gamma correction immediately before input colour; frozen float `fastlog2`, dual `2^-16` floors, 65,536-entry piecewise LUT and unbounded extrapolation; exact cache identity, tagged-raster/RAW references, CLI/Catalog/Studio reopen. No legacy payload is invented; JPEG media-dependent order, GTK picker/autotune, presets, blend UI, and OpenCL are not ported |
| Output colour profile | `iop/colorout.c` | `ravo.color.output` + private engine colour adapter | Old implementation removed | recipe schema v3; built-in/file ICC, matrix/shaper/unbounded and general RGB/XYZ/Lab transforms, four intents, BPC, soft proof, cyan gamut warning, deterministic encoded ICC state, preview v7 cache identity, Studio reopen, and CLI/Catalog profile embedding; no monitor/display/sRGB fallback |
| Final display packing | `iop/gamma.c` | private engine RGB8 packer + strict XMP importer | Old implementation removed | finite profiled float RGB clamps/rounds to owned RGB8 without another transfer curve and retains exact profile state; all 158 frozen histories prove one exact schema-v1 boundary across 12 versioned blend tuples, which imports without a recipe operation. Modified payload/version/enabled/blend/mask/multi/singleton state rejects; GTK channel/mask presentation is unsupported. Shared old ordering, registry, and pixelpipe names remain for D0.4/S4 cleanup |
| RGB primaries | `iop/primaries.c` | `ravo.color.primaries` + private engine matrix path | Old implementation removed | eight bounded hue/purity parameters; working-matrix xy/white derivation, frozen forward edge intersection and custom RGB→XYZ adjustment before the linear-Rec709 bridge; exact 0152 decode, synthetic/RAW references, cancellation, CLI/Catalog/Studio reopen; GTK display slider painting, blend UI, OpenCL, and fallback profile state are not ported |
| Color calibration | `iop/channelmixerrgb.c` | `ravo.color.channelmixerrgb` | Old implementation removed | explicit `linear_srgb_d50`, V3 matrix normalization + CAT16/Bradford/XYZ/RGB + gamut + saturation/lightness/grey; no hidden CAT by default; old chart/OpenCL/XMP ABI is not ported |
| Color checker calibration | `iop/colorchecker.c` | `ravo.color.colorchecker` | Old implementation removed | explicit presence plus 0–49 ordered source/target D50 Lab pairs; frozen N=0–4 polynomial and N>4 thin-plate RBF fits retain exact fast-log, Gaussian, addition-order, and singular-fallback behavior through a private linear-Rec709↔D50 Lab bridge. Eight source-exact presets, recipe/Develop/CLI/Catalog/Studio persistence, cache/resource/cancellation, and owned-output contracts are tested. Strict import accepts the one evidenced enabled-v2 default-unmasked record and a synthetic v1 upgrade; full 0098, disabled, duplicate, mask/custom-blend/multi, and malformed state reject structurally. GTK chart/picker and OpenCL execution are not ported; shared colour tables/Gaussian helpers/`extended.cl`/registry text remain D0.3/D0.4/S1/S4 cleanup |
| Scene-referred color grading | `iop/colorbalancergb.c` | `ravo.color.colorbalancergb` | Old implementation removed | explicit `linear_srgb_d50` + Filmlight Yrg three-zone mask/grading RGB; DT UCS 2022 is default and JzAzBz 2021 explicit optional; the removed three-parameter approximation is not the independent legacy Color Balance contract |
| Legacy Color Balance | `iop/colorbalance.c` | `ravo.color.colorbalance` | Old implementation removed | explicit presence plus all 17 v3/v4 legacy fields; Lab D50/XYZ/ProPhoto round trip with corrected RGBL lift/gamma/gain or slope/offset/power, two saturation stages, and grey-fulcrum contrast. Default-unmasked singleton state maps; masks/custom blend/multi reject structurally. Real 0033/0034 histories are negative evidence, while synthetic v3/v4 payloads establish the accepted boundary. Shared order/proxy names and `extended.cl` remain D0.4/S4/S14 cleanup |
| Color Correction | `iop/colorcorrection.c` | `ravo.color.colorcorrection` | Old implementation removed | explicit presence plus highlight/shadow a*/b* and saturation under schema v1; private linear-Rec709↔D50 Lab bridge and source-order float affine math, with no explicit-default shortcut. Strict import accepts only the enabled-v1 singleton/priority-zero/unnamed/default-unmasked blend-v9/v11 envelope represented by 0029/0092; all other presentation state rejects structurally. CLI/Catalog/Studio, cancellation, owned output, resource, and source-immutability contracts are tested. GTK plane/picker, three presets, and OpenCL are not product contracts; shared `basic.cl`, order/modulegroup/manual names, style, and pixmap remain D0.3/D0.4 or later cleanup |
| Color Contrast | `iop/colorcontrast.c` | `ravo.color.colorcontrast` | Old implementation removed | explicit-presence schema v2 owns D50 Lab per-axis steepness/offset and bounded/unbounded branches with exact float narrowing and source evaluation order. Frozen legacy v1 adds `unbound=0`; prior Ravo `amount` v1 maps deterministically and retains its zero skip. Strict import accepts only the verbatim enabled-v2 singleton/priority-zero/unnamed/default-unmasked blend-v10 record from 0038 plus a synthetic legacy-v1 upgrade under the same presentation envelope; the full masked document and all other presentation state reject structurally. Develop/CLI/Catalog/Studio, cache, cancellation, finite/error, owned-output, and source-immutability contracts are tested. GTK sliders and OpenCL are not product contracts; shared `extended.cl`, order/modulegroup/manual names and fixtures remain D0.3/D0.4/S14/E1 owners |
| Color Harmonizer | `iop/colorharmonizer.c` | `ravo.color.colorharmonizer` | Old implementation removed | exact schema v1 retains the frozen CPU/positive-smoothing contract. Canonical all/spatial/parametric/path/brush/group attachments use the private normal-mix dispatcher; unmasked bits remain unchanged and strict legacy XMP masks/custom blend/multi still reject. Studio authors owned leaves and groups and can show a preview-only overlay. Exclusive OpenCL and leftover order/module-group/manual names remain D0.3/D0.4 |
| Highlight colour reconstruction | `iop/colorreconstruction.c` | `ravo.color.colorreconstruct` | Old implementation removed | schema v1 owns the full-frame D50 Lab bilateral grid, none/chroma/hue precedence, canonical original-pixel spatial scale, deterministic x/y/lightness blur and trilinear slice immediately before Output Color. The sole 0052 enabled-v3 default-unmasked singleton imports with exact built-in RAW tuples; other versions, masks/custom blend/multi reject. Recipe/CLI/Catalog/Studio persistence, cancellation, resource accounting, real RAW reference, and source immutability are tested. GTK preview-grid caching, tile-local approximation, and OpenCL are not ported; shared order/manual names and frozen evidence remain D0.4/E1 |
| Texture | ART Texture Boost research; no legacy runtime owner | `ravo.detail.texture` + private scalar guided filter | Ravo contract accepted | schema v1 owns signed strength, canonical detail scale and one-to-five iterations before Sharpen. Two-band linear-Rec.709 luminance adjustment preserves RGB ratios and unclipped HDR; identity/default output, finite/cancel/resource/source ownership, real RAW, Recipe/CLI/Catalog/Studio/reopen/export and the Release 30 ms operation gate pass. Local Laplacian is rejected; Filmulator remains test-only after its measured interaction-budget failure (ADR-0096) |
| Sharpen | `iop/sharpen.c` | `ravo.detail.sharpen` | Old implementation removed | schema v2 owns the frozen scale-aware separable D50 Lab L* USM with radius 0–99, amount 0–2, threshold 0–100, source-order Gaussian, unchanged borders/chroma, and radius-12 support cap without clamping sigma. Current Ravo v1 values upgrade to the exact meaning. Three enabled-v1 default-unmasked singleton records map; other versions, masks/custom blend/multi reject. CLI/Catalog/Studio persistence, cancellation, resource accounting, independent oracle, real RAW reference, and original safety are tested. Demosaic capture sharpening remains R2/S2; GTK presets/OpenCL and the former Ravo approximation are removed |
| Retouch | `iop/retouch.c` | `ravo.repair.retouch` + canonical mask graph | Old implementation removed | schema v1 owns ordered clone/heal/Gaussian-or-bilateral-blur/erase-or-color-fill regions, normalized source geometry, group opacity, 0–15 reflected à-trous detail scales, merge and residual reconstruction. Strict import decodes only the five evidenced v1 revisions and v6 circle/ellipse/path/brush/group payloads from four fixture families; unsupported surrounding history remains negative. Develop/CLI/Catalog/Studio persistence, mask graph validation, preview/export/reopen equality, cancellation and memory bounds are tested. Studio authors circle regions; canonical imported path/brush shapes reproduce. Shared DWT/heal/bilateral/mask/order/style/manual/GTK helpers remain S2/S3/S10/D0 consumers; old IOP and exclusive OpenCL kernel are removed |
| Haze removal | `iop/hazeremoval.c` | `ravo.effect.dehaze` | Old implementation removed | schema v2 runs after demosaic and before Input Color on declared source-linear camera RGB, with strength/distance/adaptive state, 95% dark/bright ambient and depth estimation, scaled max/min transition, bounded tiled RGB covariance guided filter, and frozen atmospheric output. Current Ravo v1 amount upgrades and its constant-airlight approximation is removed. Exact v1/v2 default-unmasked singleton records map; raster, masks/custom blend/multi/other versions reject. CLI/Catalog/Studio cache/reopen/export, independent oracle, cancellation, resource accounting, real RAW and original safety are tested. Shared old guided-filter/box owners remain for blend/rasterfile/cacorrectrgb consumers; GTK preview caching/OpenCL are not ported |
| Graduated filter | `iop/graduatednd.c` | `ravo.effect.graduatednd` | Old implementation removed | `_compute_density` + hue/saturation RGB; positive density darkens along the positive rotated axis (sky at the top by default) |
| Tone equalizer | `iop/toneequal.c` | `ravo.core.toneequal` | Old implementation removed | before Sigmoid; five photographic controls expand into all nine [-8,0] EV one-stop targets and a normalized Gaussian RBF LUT. RGB L2 energy feeds a canonical-scale log-EV self-guided mask that retains dark texture without broad edge halos. Finite/scale/cancellation/resource failures are explicit; five-plane peak memory and preview v9 are owned (ADR-0092) |
| Local export | imageio / `libs/export.c` | services + raster encoder + CLI/Studio | Old implementation removed | JPEG owns typed quality/subsampling; PNG owns typed 8/16-bit depth, compression, private bounded libpng RGB8/RGB16/ICC/cICP, eXIf, and XMP iTXt; TIFF owns typed uint8/uint16/float16/float32, compression, conditional grayscale, resolution, and bounded private LibTIFF ICC/EXIFIFD/GPSIFD/XMP/IPTC. Product high-precision requests consume engine-owned sources and fail closed on RGB8. Catalog snapshots metadata/tags once and applies full/no-location/none privacy; no source or sidecar is rewritten or generated. Single and batch CLI/Studio export use typed options and ADR-0032 atomic no-replace publication. Remaining: PNG pHYs, TIFF multipage masks, shared imageio/storage/job consumers, and zero-consumer format-plugin retirement under I10–I13 |
| Batch local storage | `imageio/storage/disk.c` + shared dynamic storage ABI | CatalogService + CLI + Studio | GTK disk module removed; shared ABI blocked | 1–10,000 unique ordered assets; existing output directory; bounded flat portable template with only stem/asset/sequence/extension tokens; complete source/name/conflict preflight; typed options; per-item atomic no-replace; cancellation and runtime failure expose stable partial-delivery context without deleting completed outputs. CLI and multi-selection Studio share the service. Overwrite/changed/skip/unique guessing and old variable/config compatibility are rejected. Shared storage loader/header and U10/J2 export jobs remain until zero consumers (ADR-0068) |
| Output dither/posterize | `iop/dither.c` | `ravo.output.dither` + final engine output stage | Old implementation removed | explicit schema-v1 presence; encoded-output RGB after Output Color; random TEA/triangular noise with fixed serial stream; source-order Floyd–Steinberg gray/RGB/auto levels and tiny rule; 2–8 posterization; preview/RGB8/RGB16/float target policy; strict three-record XMP mapping; Recipe/Develop/CLI/Catalog/Studio/style/reopen/export and cancellation/source ownership tests. Masks and disabled/multi/custom blend reject. Shared TEA/extended kernel/order/manual names remain M8/D0 consumers (ADR-0069) |
| Canvas | `iop/enlargecanvas.c` + `develop/borders_helper.*` | `ravo.geometry.canvas` + attached photo-content frame | Old implementation removed | schema v1 owns frozen percentage truncation, asymmetric placement, 5-pixel/three-times bounds, five opaque linear-Rec.709 colours, and mask/Retouch evaluation in the original content rectangle with zero-alpha padding outside. Exact 0157 import, finite/resource/cancellation, Recipe/CLI/Catalog/Studio/reopen/export are tested. Perspective/straighten and crop now transform pixels and preview alpha together; nested Canvas, masks on Canvas, attached sub-ROI, post-Canvas rotate/flip/lens, and later masked consumers reject (ADR-0070/0096) |
| Perspective correction | `iop/ashift.c` + `iop/ashift_lsd.c` + `iop/ashift_nmsimplex.c` | `ravo.geometry.perspective` + bounded Engine analysis | Old implementation removed | schema v1 owns angle, vertical/horizontal shift, shear, deterministic maximal safe crop and bilinear/Lanczos2/Lanczos3 sampling in scene-linear RGB. A bounded Sobel/NMS/oriented-Hough detector and robust fitter serve CLI and Studio; no-line/degenerate/cancel/resource failures do not publish. Generic v4/v5 leftover state maps strictly; Recipe/CLI/Catalog/Studio/reopen/export, real RAW analysis, Canvas/mask alpha and Release latency evidence are covered. The exclusive old IOP, LSD/Nelder–Mead sources and icons are retired; shared Overlay/order/module-group/manual strings remain other-owner cleanup (ADR-0096) |
| Output frame | `iop/borders.c` + `develop/borders_helper.*` | `ravo.output.frame` + final engine output stage | Old implementation removed | schema v1 owns frozen constant/image/custom aspect, orientation, basis, size, position, border/line colours, integer endpoints, and final encoded-output placement after optional Dither. Exact 0030 v3 and 0154/0155 v4 imports, finite/resource/cancellation, overlay padding, Recipe/CLI/Catalog/Studio and JPEG/PNG/TIFF dimensions are tested. Exclusive helper/kernel/icons are removed; Overlay/order/module-group/manual strings remain M3/D0 owners (ADR-0070) |
| Text watermark | `iop/watermark.c` + `host/data/watermarks/*` | `ravo.output.watermark` final engine stage | Old implementation/resources removed | schema v1 owns bounded portable ASCII text, fixed 5×7 glyphs, source stem/asset token expansion, RGB/opacity, short-side scale, rotation, normalized offsets, nine-way alignment, four-sample coverage, and alpha composition after Frame. Recipe/CLI/Catalog/Studio/style/reopen/export, finite/cancellation/source ownership, and exact glyph tests pass. Arbitrary SVG/PNG, system fonts, broad metadata variables, and the missing `promo.svg` silent no-op are explicit unsupported decisions. Shared crop/develop/imageio/order/module-group/manual strings remain other-owner cleanup (ADR-0071) |
| Original-copy publication | `imageio/format/copy.c` + dynamic imageio/storage/job consumers | services + CLI | In progress | explicit source→destination exact bytes stream through 64 KiB; unique exclusive adjacent temp, sync, atomic no-replace, conflict/cancellation/source-change/disk-full taxonomy, cleanup, source immutability, new destination metadata, and complete CLI context are tested. No XMP is generated. I1/I14/U10/J2 must reach zero consumers before the legacy plugin or registration can retire |
| Tone curve | `iop/tonecurve.c` | `ravo.core.tonecurve` + Curves section | Old implementation removed | frozen C default `RGB, linked`: Lab D50 → ProPhoto, `preserve_colors=average`. Studio authors RGB-linked, Lab, XYZ, and Lab-independent L/a/b with monotone Hermite, centripetal Catmull-Rom, or cubic spline. Histogram overlay uses engine-owned display RGB8 + luma bins (ADR-0084) |
| RGB curve | `iop/rgbcurve.c` | `ravo.color.rgbcurve` + Curves section | Ravo accepted | Linked/independent working-RGB, preserve-colors, middle-grey uncompensate, 2–20 nodes, leftover interpolators, and Studio parametric regions. Dedicated editor is accepted (ADR-0052/0053/0084). Leftover IOP stays until freeze census is zero |
| Default display transform | `iop/sigmoid.c` | `ravo.display.sigmoid` + RAW baseline + Develop Inspector | Old implementation removed | default per-channel generalized log-logistic + hue preservation; `rgb_ratio` is the C second mode. Linear sRGB, Standard SDR target. `filmicrgb`/`agx` remain leftovers |
| CLI | `src/cli` | cli + control | In progress | engine/recipe/catalog/develop/export JSON use supported services/engine; `ravo-studio-control/v1` adds owner-only discovery, revisioned selection/current+saved recipe inspection, strict command-controller Develop mutation, and exact no-replace preview artifacts without UI automation (ADR-0090) |
| GPU | OpenCL/pixelpipe | engine adapter | Deferred | Start only after active TODO / GPU baseline CPU goldens and end-to-end benefit proof |

Use only these statuses: “Not started / Baseline frozen / In progress / Ravo
accepted / Old implementation removed / Deferred / Unsupported.” Physically
delete an old owner under the active migration TODO acceptance for that item;
do not wait for the entire package to retire.
