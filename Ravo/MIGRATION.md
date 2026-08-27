# Ravo Migration Policy

## Goal

Ravo will ultimately replace `legacy/src/`. Catalog/import/viewer and Basic
Develop are already implemented in Ravo. The next stage rewrites items one by
one according to the root [`TODO_LEGACY_MIGRATION.md`](../TODO_LEGACY_MIGRATION.md);
delete the corresponding old owner only after an item is “Ravo accepted.” New
ownership lives only in `Ravo/`. Version 0.9 remains prohibited from
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
2. Follow the root active migration TODO strictly: first finish cache,
   save/reopen, and interaction gates for migrated-but-unclosed capabilities,
   then start the next IOP. Delete an old owner only after its item is “Ravo
   accepted.”
3. Raise masks, additional RAW/color/geometry/output algorithms according to
   dependencies after the queue; old styles/catalog/XMP data compatibility still
   needs a separate dated product decision.
4. Accept cross-cutting reliability, three-platform installation, and optional
   GPU under the TODO gates.
5. After the queue is empty, demonstrate release transition/rollback, then
   handle explicit leftover archiving or final cleanup.

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
| Recipe/schema | IOP params/XMP | recipe | In progress | versioned round-trip, explicit upgrades, and bounded strict mappings including exposure v5/v6/v7, Color Checker v1/v2, Color Correction v1, Color Contrast schema v2 plus both independent v1 upgrades, and the exact 17-field Color Harmonizer schema v1 are tested |
| RAW inspect/decode | imageio/LibRaw | engine + codec adapter | In progress | `mire1.cr2` inspect/render tested; format and sensor coverage remain limited |
| CPU preview/pixelpipe | `src/develop` | engine | In progress | bounded PNG, cancellation, full exposure, Color Checker, affine D50 Lab Color Correction, bounded/unbounded D50 Lab Color Contrast, and profile-aware dt-UCS/RYB Color Harmonizer smoothing-zero CPU behavior, plus explicit colour operations are tested; complete ROI remains unfinished |
| SQLite catalog | common/database | domain + SQLite adapter | In progress | schema v4 create/reopen/migrate/newer-version rejection tested; tags, writable metadata, history tested; no old-catalog migration |
| Reference-only import | common/imageio/import | services + adapters | In progress | PNG/JPEG and LibRaw RAW (including ARW) plus recursive directory import; JPEG/GIF/WebP/TIFF plugin targets are required |
| Preview cache | mipmap/cache/imageio | services + adapters | In progress | atomic PNG cache outside library and rebuild on reopen tested |
| Gallery/viewer | lighttable/darkroom | desktop + services | In progress | Studio can create/open/import/fit/fill/100%; long-list resource gate remains |
| Catalog metadata/workflow | common/libs | domain + services | Old implementation removed | Unicode tag filtering, catalog-only writable title/creator/copyright, read-only capture EXIF, persistent history/snapshot; faces/map/GPS writeback not implemented. Old `libs/tagging.c`, `metadata*.c`, `history.c`, `snapshots.c`, and `copy_history.c` deleted |
| Mask/blend/operations | develop/iop | recipe + engine | In progress | `ravo.effect.graduatednd` is the first local adjustment, where the gradient is the mask. Exposure, Color Checker, Color Correction, and Color Contrast accept exact default-unmasked import boundaries; the Color Harmonizer core accepts only an unmasked canonical operation. Unsupported mask/custom-blend/multi state rejects structurally and the general mask graph remains |
| RAW highlight reconstruction | `iop/highlights.c` | `ravo.raw.highlights` | Old implementation removed | default Bayer opposed (`_process_opposed`); clip / reconstruct-color inpaint / LCh are explicit modes. Non-Bayer, raster, laplacian, and segmentation are structured unsupported states |
| RAW hot-pixel repair | `iop/hotpixels.c` | `ravo.raw.hotpixels` | Old implementation removed | Bayer four same-colour ±2 neighbours, `strength/2`, strict 4 / permissive 3, replacement with neighbor maximum; X-Trans/monochrome/raster are structured unsupported |
| RAW Bayer chromatic aberration | `iop/cacorrect.c` | `ravo.raw.cacorrect` | Old implementation removed | RawTherapee 128 tile/16 overlap, green/color-difference statistics, 3×3 median, full-image polynomial shift fit, ±3.99 interpolation, and avoid-color-shift; `cacorrectrgb` remains a separate leftover |
| Default denoising | `iop/denoiseprofile.c` | `ravo.detail.denoiseprofile` | Old implementation removed | default wavelets + Y0U0V0 + a-trous BayesShrink; use recorded generic a/b without a camera profile. `nlmeans`/`atrous`/`bilateral`/`rawdenoise` are outside this item |
| Lens correction | `iop/lens.cc` | `ravo.geometry.lens` | Old implementation removed | explicit lensfun poly3/poly5 + linear TCA + manual vignette spline; lookup uses a versioned coefficient table and fails fast when unmatched. The lensfun source root remains the production database successor and was not pinned in this work |
| Color equalizer | `iop/colorequal.c` | `ravo.color.colorequal` | Old implementation removed | dt UCS 22 eight-node periodic RBF LUT; `colorzones` remains a leftover |
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
| Color Contrast | `iop/colorcontrast.c` | `ravo.color.colorcontrast` | Old implementation removed | explicit-presence schema v2 owns D50 Lab per-axis steepness/offset and bounded/unbounded branches with exact float narrowing and source evaluation order. Frozen legacy v1 adds `unbound=0`; prior Ravo `amount` v1 maps deterministically and retains its zero skip. Strict import accepts only the verbatim enabled-v2 singleton/priority-zero/unnamed/default-unmasked blend-v10 record from 0038 plus a synthetic legacy-v1 upgrade under the same presentation envelope; the full masked document and all other presentation state reject structurally. Develop/CLI/Catalog/Studio, cache, cancellation, finite/error, owned-output, and source-immutability contracts are tested. GTK sliders and OpenCL are not product contracts; shared `extended.cl`, order/modulegroup/manual names, example style, and fixtures remain D0.3/D0.4/S14/E1 owners |
| Color Harmonizer core | `iop/colorharmonizer.c` | `ravo.color.colorharmonizer` | In progress | exact schema v1 has 17 flat fields and a `profile_linear_rgb_d50` / `dt_ucs_harmony_v1` identity. The accepted enabled, unmasked, smoothing-zero CPU path retains negative `fmaxf` clipping, declared-profile D50↔dt-UCS conversion, immutable S2.1 RYB/node geometry, cubic neutral protection, source float order, cancellation, validation, and owned publication. Frozen 0176 records 12/13 are parameter/bit-golden evidence, not an importer claim. Strict legacy import, Develop/CLI/Catalog/Studio, canonical ROI scale plus S2.2 recursive Gaussian, masks/presentation, and old-owner retirement remain unfinished and currently unauthorized |
| Graduated filter | `iop/graduatednd.c` | `ravo.effect.graduatednd` | Old implementation removed | `_compute_density` + hue/saturation RGB; positive density darkens along the positive rotated axis (sky at the top by default) |
| Tone equalizer | `iop/toneequal.c` | `ravo.core.toneequal` | Old implementation removed | before Sigmoid, nine-band [-8,0] EV RBF LUT; default RGB L2 luminance |
| Local export | imageio / `libs/export.c` | services + raster encoder + CLI/Studio | Old implementation removed | JPEG owns typed quality/subsampling; PNG owns typed 8/16-bit depth plus compression 0–9/default 5 and a private bounded libpng RGB8/ICC/cICP encoder, with true RGB16 structured unsupported. TIFF owns typed uint8/uint16/float16/float32 plus none/Deflate/predictor, level 1–9/default 6, frozen conditional grayscale, and 72–9600 inch resolution through a pinned private LibTIFF RGB8/ICC encoder; high-precision requests are structured unsupported from the current source. After asset lookup, Catalog copies the normalized destination and current writable metadata once; the main IFD owns bounded UTF-8 DocumentName/description/creator/copyright with title deliberately unmapped, while EXIFIFD/IPTC/XMP remain absent. Complete bytes then use ADR-0032 atomic no-replace publication without an Exiv2 reopen, source rewrite, or sidecar mutation. `catalog export --format tiff|tif` exposes the four TIFF-qualified flags, rejects them for other formats, uses last-value-wins for value flags, and rejects a duplicate grayscale toggle. Original copy is available. Complete EXIF/IPTC/XMP packets, capture/timezone/GPS, XMP attach/history/sidecar policy, PNG resolution, TIFF multipage masks, explicit PNG CLI and TIFF Studio options, high-precision sources, and batch presets remain unfinished under I13/S9/J6. Old `libs/export*.c` are deleted; `imageio/format/png.c` and `imageio/format/tiff.c` remain under active TODO I12/I13, while the original-copy plugin and its shared dynamic consumers remain tracked below and under active I10 |
| Original-copy publication | `imageio/format/copy.c` + dynamic imageio/storage/job consumers | services + CLI | In progress | explicit source→destination exact bytes stream through 64 KiB; unique exclusive adjacent temp, sync, atomic no-replace, conflict/cancellation/source-change/disk-full taxonomy, cleanup, source immutability, new destination metadata, and complete CLI context are tested. No XMP is generated. I1/I14/U10/J2 must reach zero consumers before the legacy plugin or registration can retire |
| Tone curve | `iop/tonecurve.c` | `ravo.core.tonecurve` + Develop Inspector | Old implementation removed | frozen C default `RGB, linked`: Lab D50 → ProPhoto, `preserve_colors=average`, monotone Hermite LUT. `lab` / `xyz` / `lab_independent` are explicit modes. `rgbcurve` remains a leftover |
| Default display transform | `iop/sigmoid.c` | `ravo.display.sigmoid` + RAW baseline + Develop Inspector | Old implementation removed | default per-channel generalized log-logistic + hue preservation; `rgb_ratio` is the C second mode. Linear sRGB, Standard SDR target. `filmicrgb`/`agx` remain leftovers |
| CLI | `src/cli` | cli | In progress | engine/recipe/catalog/develop/export JSON all use supported services/engine |
| GPU | OpenCL/pixelpipe | engine adapter | Deferred | Start only after active TODO / GPU baseline CPU goldens and end-to-end benefit proof |

Use only these statuses: “Not started / Baseline frozen / In progress / Ravo
accepted / Old implementation removed / Deferred / Unsupported.” Physically
delete an old owner under the active migration TODO acceptance for that item;
do not wait for the entire package to retire.
