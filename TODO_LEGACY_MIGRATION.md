# Ravo Legacy Migration TODO

> **Status: in progress**
>
> **Updated: 2026-08-27**
>
> **Current execution focus: C14 colorharmonizer test-first default-unmasked,
> smoothing-zero CPU core.** The owner/dependency audit is accepted; S1.1 D50
> Lab, S1.2 dt-UCS, and S2.1 harmony geometry are complete prerequisites.
> Nonzero smoothing remains gated on S2.2 recursive Gaussian and canonical ROI-
> scale semantics. Do not start C15 or cacorrectrgb in parallel.

This document records only unfinished execution work, risks, dependencies,
verification commands, and acceptance gates. Current capability, architecture,
migration policy, leftover boundary, and test contracts are owned by
Ravo/README.md, Ravo/ARCHITECTURE.md, Ravo/MIGRATION.md, and Ravo/TESTING.md.
DevDocs/ProductRoadmap.md records cross-layer design constraints that are not
ready for execution.

## 1. Execution rules

- Advance only the first item in the queue. Do not start another before it is
  complete.
- A capability implemented in engine/catalog but not meeting this section's gate
  is not “Ravo accepted.”
- For every item, statically read old owner/fixtures first; then define Ravo
  owner, lifecycle, failure/cancellation path, and minimum validation set.
- Algorithm work reproduces frozen C default CPU behavior: formula, colour space,
  filters, and default mode. Do not call a simplified replacement “migrated” or
  use it to delete old implementation. GUI, lifecycle, OpenCL, and dynamic ABI
  may be removed, but core mathematics may not be substituted.
- Use Ravo/MIGRATION.md for “Ravo accepted” and old-owner deletion gates. Until
  those gates are met, modify only Ravo.
- On completion, move durable conclusions into README/ARCHITECTURE/MIGRATION/
  TESTING/ADR/code/tests, then remove the item here. Do not leave checked-off
  history or archived TODOs.
- Reliability findings can block the current item but must not be used to bulk
  clean GTK/OpenCL/shared imageio/fixtures.
- Mark an unrun platform or manual check as untested; never use historic results
  to represent current acceptance.

## 2. Migration queue

C14 colorharmonizer's owner/dependency audit is accepted. S1.1 owns the frozen
D50 Lab bridge, S1.2 owns the source-order dt-UCS bridge, and S2.1 owns immutable
RYB lookup and harmony-node geometry. The current authorized tranche is a
test-first canonical recipe and CPU core for the evidenced default-unmasked,
`smoothing == 0` boundary. Nonzero smoothing remains unsupported until S2.2
freezes the two-channel recursive Gaussian and C14 has canonical ROI-scale
semantics. M1 and the general mask graph do not block the evidenced exact
default-unmasked envelope; every other mask/blend state remains structurally
unsupported. Strict legacy import, Develop/CLI/Catalog/Studio consumers,
nonzero smoothing, stable authority, and atomic legacy retirement remain
separately reviewed later tranches. C15 remains unauthorized.

Independent adapter or reliability work may run only when its dependencies are
met and its owners and files do not overlap.

## 3. Complete remaining-module inventory and serial order

This section is an execution inventory for the current worktree, not the
long-term capability authority. Snapshot baseline:

- legacy/src/iop/CMakeLists.txt has 51 unconditional IOP registrations plus two
  conditional owners (`liquify`, `watermark`); all 53 have a row in section 3.2.
- legacy/src/libs/CMakeLists.txt has 23 source-backed modules/tools plus stale
  registrations whose source has retired.
- legacy/src/views has darkroom/lighttable; imageio has four formats, one
  storage owner, and nine dispatcher/decoder owner groups.
- common, control, develop, GUI, and host resources are divided into ownership
  units in sections 3.3–3.7.
- The 158 fixture sets and five source images in legacy/tests remain read-only
  throughout algorithm migration; old runners never run.

Status terms: **current** means only the explicitly authorized C14 tranche named
above; it does not authorize later C14 tranches or the next queue row. **Queued**
waits for dependencies and all earlier rows. **Delete** means no UI/ABI port.
**Keep evidence** means do not move it before migration completes. When a module
meets its gate, first update stable truth, then remove its row. Do not leave
historical checked marks here.

General completion gates:

- **ALG:** statically read owner/fixture → versioned schema/workspace/ROI →
  complete default CPU mathematics → synthetic + real fixture + error/
  cancellation/resource unit tests → supported CLI/Studio/services consumption
  → delete old source, registration, exclusive helper/kernel/resource.
- **CORE:** list all consumers; move capability into a private
  foundation/domain/services/engine owner; test thread/cache/transaction
  lifecycle; delete shared C/global state after consumers reach zero.
- **DELETE:** prove no unaccepted algorithm consumer exists; delete CMake,
  dynamic loading, GTK resources, configuration keys, and documentation
  references without a Qt fallback or empty shell.
- **DATA:** move needed fixture/schema/resource into a Ravo truth source and
  check hash/round-trip before deleting old runner/assets.

### 3.1 Cleanup left after retired owners

| ID | Module / owner | Action | Dependency and gate |
| --- | --- | --- | --- |
| D0.1 | iop/hlreconstruct/* | Delete | highlights.c retired; prove no consumer, add retired list, pass freeze check |
| D0.2 | stale export/copy_history/tagging/metadata/history/snapshots entries in libs/CMakeLists.txt | Delete registrations | Do not modify other frozen modules; prove corresponding sources are on retired list |
| D0.3 | host/data/kernels exclusive kernels/entries for retired IOPs | Delete | Distinguish shared extended.cl/colour helpers; remove only entries/program registrations with no remaining consumer |
| D0.4 | common/iop_order.c, libs/modulegroups.c, usermanual_url.c retired names | Final deletion | These files still serve old UI/registry, including shared exposure order/module-group/manual names; remove with their DELETE batch after all algorithm consumers clear |
| D0.5 | unconsumed iop/choleski.h, equalizer_eaw.h, svd.h, unregistered useless.c | Delete | Search all includes/targets/fixture owners again; add retired list and pass freeze check |

### 3.2 IOP algorithm queue (53 owners: 51 unconditional + 2 conditional)

The group order below is dependency order; rows in a group are serial by
default. fixture means static evidence exists, not that it is covered.

#### A. Current colour scheduling and foundation

| ID | IOP / owner | Fixture | Status / dependency / special gate |
| --- | --- | --- | --- |
| C14 | colorharmonizer — iop/colorharmonizer.c | yes | **Current / test-first default-unmasked, smoothing-zero CPU core authorized**; audit accepted and S1.1/S1.2/S2.1 complete; smoothing above zero waits for S2.2 plus canonical ROI scale; mask/presentation, strict import, consumers, and retirement remain later structured gates; C15 is forbidden |
| C15 | colorize — iop/colorize.c | yes | Queued / ALG; depends on S1/M1; Lab hue/saturation/source mix |
| C16 | colormapping — iop/colormapping.c | yes | Queued / ALG; depends on S1; source/target statistics, clusters, determinism |
| C17 | colorzones — iop/colorzones.c | yes | Queued / ALG; colorequal stays default; migrate complete optional HSL/Lab partitions and curves |
| C18 | monochrome — iop/monochrome.c | yes | Queued / ALG; current one-amount control is not acceptance; reproduce channel filter/colour-space path |
| C19 | lowlight — iop/lowlight.c | yes | Queued / ALG; depends on S1; low-light perception curve and LUT |
| C20 | splittoning — iop/splittoning.c | yes | Queued / ALG; simplified split toning is not acceptance; reproduce balance/compress |
| C21 | velvia — iop/velvia.c | yes | Queued / ALG; simplified velvia is not acceptance; reproduce luminance/saturation weighting |

#### B. Display transforms, curves, and LUTs

| ID | IOP / owner | Fixture | Status / dependency / special gate |
| --- | --- | --- | --- |
| T1 | basecurve — iop/basecurve.c | yes | Queued / ALG; depends on explicit input-profile state; camera presets, exposure fusion, curve interpolation |
| T2 | rgbcurve — iop/rgbcurve.c | yes | Queued / ALG; distinct from migrated tonecurve; reproduce linked/independent/preserve-colour modes |
| T3 | rgblevels — iop/rgblevels.c | yes | Queued / ALG; auto/manual, linked channels, picker UI deleted |
| T4 | filmicrgb — iop/filmicrgb.c | yes | Queued / ALG; Sigmoid remains default; complete scene/display, chroma/gamut/reconstruction modes |
| T5 | agx — iop/agx.c | yes | Queued / ALG; Sigmoid remains default; AgX curve, primaries, gamut path |
| T6 | lut3d — iop/lut3d.c | yes | Queued / ALG; depends on explicit input/output profile state; LUT format adapter, missing/invalid file, interpolation |
| T7 | negadoctor — iop/negadoctor.c | yes | Queued / ALG; negative input, film base, scanner/profile, picker UI separated |

#### C. RAW preprocess and demosaic

| ID | IOP / owner | Fixture | Status / dependency / special gate |
| --- | --- | --- | --- |
| R1 | rawprepare — iop/rawprepare.c | yes | Queued / ALG; complete crop/black/white/CFA/orientation, replacing current absorbed subset |
| R2 | demosaic — iop/demosaic.c + iop/demosaicing/* | yes | Queued / ALG; Bayer/X-Trans modes, dual/green matching, memory/ROI; basic 3×3 is only a subset |
| R3 | rawdenoise — iop/rawdenoise.c | yes | Queued / ALG; pre-demosaic wavelet/threshold and sensor rejection |
| R4 | cacorrectrgb — iop/cacorrectrgb.c | no | Queued / ALG; separate from migrated pre-demosaic cacorrect; create synthetic/RAW fixture first |
| R5 | colorreconstruct — iop/colorreconstruction.c | yes | Queued / ALG; depends on R2 and explicit input-profile state; complete highlight colour propagation/ROI |
| R6 | rasterfile — iop/rasterfile.c | no | Queued / ALG; depends on I1/M1; raster-source/mask ownership and no-fixture baseline |

#### D. Geometry, canvas, and scale

| ID | IOP / owner | Fixture | Status / dependency / special gate |
| --- | --- | --- | --- |
| G1 | flip — iop/flip.c | yes | Queued / ALG; complete EXIF/orientation/ROI; existing mirror/quarter-turn is only a subset |
| G2 | rotatepixels — iop/rotatepixels.c | no | Queued / ALG; depends on G1; sensor/pixel rotation synthetic fixture |
| G3 | scalepixels — iop/scalepixels.c | no | Queued / ALG; depends on S3; interpolation/ROI/scale contract and synthetic fixture |
| G4 | crop — iop/crop.c | yes | Queued / ALG; depends on M1/G1; complete aspect/keystone/ROI; current normalized crop is only a subset |
| G5 | enlargecanvas — iop/enlargecanvas.c | yes | Queued / ALG; canvas coordinates, fill/alpha, mask transform |
| G6 | ashift — iop/ashift.c + ashift_lsd.c + ashift_nmsimplex.c | yes | Queued / ALG; line detection, lens geometry, automatic/manual fit; current straighten is not a substitute |
| G7 | finalscale — iop/finalscale.c | no | Queued / ALG; depends on G3/O1; formal resampling/output-size contract |
| G8 | borders — iop/borders.c | yes | Queued / ALG; depends on G5/O1; frame/aspect/colour/metadata and export |
| G9 | liquify — iop/liquify.c | yes | Queued / ALG; depends on M1/G4; deformation graph, ROI, cancellation |

#### E. Detail, denoise, blur, and local contrast

| ID | IOP / owner | Fixture | Status / dependency / special gate |
| --- | --- | --- | --- |
| F1 | sharpen — iop/sharpen.c | yes | Queued / ALG; simple USM is not acceptance; reproduce blur/threshold/ROI |
| F2 | highpass — iop/highpass.c | yes | Queued / ALG; depends on S3/M1; Lab/RGB blend and contrast path |
| F3 | lowpass — iop/lowpass.c | yes | Queued / ALG; depends on S3/M1; Gaussian/bilateral modes and saturation |
| F4 | shadhi — iop/shadhi.c | yes | Queued / ALG; depends on S3; bilateral shadows/highlights path |
| F5 | atrous — iop/atrous.c | yes | Queued / ALG; complete a-trous multiscale bands, boost/threshold, mask |
| F6 | bilat — iop/bilat.c | yes | Queued / ALG; frozen fast bilateral grid; do not merge it with bilateral by assumption |
| F7 | bilateral — iop/bilateral.cc + Permutohedral.h | yes | Queued / ALG; permutohedral lattice, memory budget, CPU determinism |
| F8 | nlmeans — iop/nlmeans.c | yes | Queued / ALG; depends on S3; patch/search/scattering and cancellation |
| F9 | diffuse — iop/diffuse.c | yes | Queued / ALG; iterative anisotropic diffusion, scale/ROI, blend |
| F10 | blurs — iop/blurs.c | yes | Queued / ALG; Gaussian/lens/motion modes; one blur is not a substitute |
| F11 | hazeremoval — iop/hazeremoval.c | yes | Queued / ALG; atmospheric model, distance estimate, guided filter |
| F12 | soften — iop/soften.c | yes | Queued / ALG; simplified soften is not acceptance; reproduce blur/mix/colour path |

#### F. Masks, compositing, repair, effects, and diagnostics

| ID | IOP / owner | Fixture | Status / dependency / special gate |
| --- | --- | --- | --- |
| M1 | mask_manager — iop/mask_manager.c | yes | Queued / ALG+CORE; first complete the canonical mask/blend graph in 3.3 S3 |
| M2 | retouch — iop/retouch.c | yes | Queued / ALG; depends on M1/S2; clone/heal/blur/fill and source geometry |
| M3 | overlay — iop/overlay.c | yes | Queued / ALG; depends on M1/I1/G5; resource lifecycle and alpha composition |
| M4 | censorize — iop/censorize.c | yes | Queued / ALG; depends on M1/S2; pixelate/blur/noise modes |
| M5 | watermark — iop/watermark.c | yes | Queued / ALG+DATA; depends on M1/I1; SVG/text/font/metadata resource determinism |
| M6 | bloom — iop/bloom.c | yes | Queued / ALG; simplified bloom is not acceptance; reproduce threshold/blur/mix |
| M7 | grain — iop/grain.c | yes | Queued / ALG; deterministic noise substitute is not acceptance; reproduce Lab/ISO/channel mode |
| M8 | vignette — iop/vignette.c | yes | Queued / ALG; simple radial darkening is not acceptance; reproduce shape/dither/colours |
| O1 | dither — iop/dither.c | yes | Queued / ALG; quantization method, bit depth, deterministic random, export |
| O2 | overexposed — iop/overexposed.c | no | Queued / ALG; migrate diagnostic calculation/threshold only, delete GTK overlay, create synthetic fixture |
| O3 | rawoverexposed — iop/rawoverexposed.c | no | Queued / ALG; RAW CFA threshold/channel diagnostic, create synthetic fixture |

### 3.3 Shared algorithms, pixelpipe, masks, and domain owners

| ID | Owner paths | Action | Dependency and acceptance gate |
| --- | --- | --- | --- |
| S1 | common/colorspaces*, chromatic_adaptation.h, illuminants.h, matrices*, custom_primaries*, gamut_mapping.h, darktable_ucs_22_helpers.h, color_*, colorchecker.h, curve_tools*, wb_presets* | CORE: move private colour science into engine | S1.1 frozen D50 Lab and S1.2 source-order dt-UCS bridges are complete behind bit-goldens; S1 remains incomplete and still needs explicit workspace/LUT owners for C3–C21/T1–T7, with no GTK/LCMS concrete type leakage |
| S2 | common/bilateral*, box_filters*, distance_transform*, dwt*, eaw*, eigf.h, gaussian*, guided_filter*, fast_guided_filter.h, heal*, locallaplacian*, nlmeans_core*, splines*, interpolation*, noiseprofiles*, bspline.h, luminance_mask.h, rgb_norms.h, focus*, histogram*, develop/noise_generator.h, develop/openmp_maths.h | CORE: move primitives into engine | S2.1 immutable harmony geometry is complete; S2.2 must freeze the two-channel recursive Gaussian before C14 nonzero smoothing; accept remaining primitives per F/G/M/diagnostic consumer and do not port OpenCL twins |
| S3 | develop/blend*, develop/blends/*, develop/masks/*, develop/masks.h | CORE: create canonical mask/blend graph | Shape/group/coordinate/parametric blend/ROI/schema/cancellation tests; prerequisite for M1 |
| S4 | develop/develop*, pixelpipe*, pixelpipe_cache*, pixelpipe_hb*, tiling*, imageop*, format*, borders_helper* | CORE: converge into Engine facade + services cache/scheduler | Every old consumer reaches zero; shared exposure proxy/imageop hooks are cleanup state, not a runtime exposure owner; do not copy dynamic pixelpipe/global state |
| S5 | iop/iop_api.h, common/module*, module_api.h, dynload*, introspection.h, action.h, darktable*, darktable_api.h, poison.h, iop_group*, iop_order*, iop_profile* | DELETE dynamic IOP/module ABI and global composition | Delete after all remaining IOPs retire; built-in versioned operation registry remains |
| S6 | common/database*, database_schema*, sqliteicu* | CORE/DELETE | Compare against Ravo schema/FTS/ICU; move needed data contracts into SQLite adapter and delete remaining old catalog ABI |
| S7 | common/image*, film*, import_session*, grouping* | CORE/DELETE | Delete global image/film owner after Asset/import/folder/group contracts cover it |
| S8 | common/collection*, selection*, ratings*, colorlabels*, act_on* | CORE/DELETE | Ravo LibraryQuery/selection/review coverage; map each missing collection query or mark unsupported |
| S9 | common/exif*, metadata*, metadata_export*, tags* | CORE/DATA | Capture/writable/ICC/export metadata schema, originals read-only; Exiv2 types remain adapter-private |
| S10 | common/history*, history_snapshot*, styles*, presets*, undo* | CORE/DATA | Canonical recipe history/style/preset import/reject and rollback; delete GUI preset owner |
| S11 | common/cache*, image_cache*, mipmap_cache*, imagebuf* | CORE/DELETE | Explicit byte budget/LRU/atomic publication; replace with CatalogService/PreviewCache |
| S12 | common/atomic*, dtpthread*, resource_limits*, system_signal_handling*, utility*, datetime*, variables*, calculator*, file_location*, math.h, points.h, heap.h, tea.h, dttypes.h, debug.h, extra_optimizations.h, sse.h, grealpath.h, win_file_trash* | CORE/DELETE | Move only value/thread/path/platform logic with Ravo consumers; delete the rest with global core |
| S13 | common/curl_tools*, dbus*, gimp*, pwstorage/*, overlay* | DELETE or adapter | Create an independent port only for an explicitly active IOP/service; do not keep remote-publish/UI credential shells |
| S14 | common/opencl*, dlopencl*, opencl_drivers_blacklist.h | DELETE | Delete after CPU goldens complete, including the shared `host/data/kernels/basic.cl` exposure kernel/program path; Ravo GPU never reuses OpenCL API |

### 3.4 Codec, imageio, and output plugins

| ID | Owner | Action | Acceptance gate |
| --- | --- | --- | --- |
| I1 | imageio/imageio.c, imageio_module*, imageio_common.h | CORE/DELETE dispatcher | All formats/storage use Ravo ports; delete dynamic imageio ABI/global registry |
| I2 | imageio_libraw* | Adapter audit | Ravo pinned LibRaw covers RAW metadata/embedded/decode/sensor error, then delete old wrapper |
| I3 | imageio_rawspeed* + external RawSpeed wiring | DELETE or independent decoder | Make format/performance/fixture decision; do not silently fall back to LibRaw |
| I4 | imageio_dng*, common/dng_opcode* | ALG/adapter | DNG opcode/crop/black/metadata fixture; LibRaw partial coverage is not complete coverage |
| I5 | imageio_jpeg* | Adapter audit | Orientation/ICC/alpha/error contract for Qt decoder; delete old libjpeg wrapper without consumers |
| I6 | imageio_png* | Adapter audit | Bit depth/ICC/alpha/error contract for Qt decoder |
| I7 | imageio_tiff* | Adapter audit | 8/16/float, multi-page, ICC/alpha/error Qt contract |
| I8 | imageio_qoi* + qoi.h | ALG/adapter | If Ravo keeps QOI, add decoder/encoder fixtures; otherwise explicit unsupported then delete |
| I9 | imageio_rgbe* | ALG/adapter | HDR RGBE decode/colour contract and fixture; do not treat it as ordinary raster |
| I10 | imageio/format/copy.c | DELETE/reuse original-copy service | Ravo exact-byte 64 KiB streaming, exclusive temp, atomic no-replace, conflict/cancellation/source/error/disk-full and CLI context are hardened; still blocked on I1/I14/U10/J2 zero consumers before removing plugin/registration |
| I11 | imageio/format/jpeg.c | Adapter/export | Quality/ICC/metadata/subsampling/disk-full contract |
| I12 | imageio/format/png.c | Adapter/export | Core accepted: typed 8/16-bit depth and compression 0–9/default 5 propagate to a bounded private libpng RGB8/opaque/non-interlaced ICC + known-built-in-cICP encoder and shared atomic publication. Remaining: real RGB16 source, EXIF/XMP/resolution, explicit PNG CLI options, shared imageio/storage/job consumers, then zero-consumer plugin/registration retirement; do not delete the old owner yet |
| I13 | imageio/format/tiff.c | Adapter/export | Core accepted: typed uint8/uint16/float16/float32 and none/Deflate/predictor with level 1–9/default 6 plus conditional grayscale propagate to a bounded pinned private LibTIFF classic-LE RGB8/opaque/strip/300-dpi/exact-ICC encoder and ADR-0032 atomic publication. TIFF-qualified CLI sample/compression/level/grayscale options now preserve those defaults, canonical values, structured errors, and high-precision unsupported state. Remaining: real uint16/float16/float32 rendered sources, EXIF/IPTC/XMP, multipage masks, explicit TIFF Studio options, shared imageio/storage/job consumers, then zero-consumer plugin/registration retirement; keep QTiffPlugin for separate I7 input and do not delete the old output owner yet |
| I14 | imageio/storage/disk.c | DELETE/reuse CatalogService export | Path template/conflict/cancellation/atomic write covered, then delete dynamic storage ABI |
| I15 | external/CMakeLists.txt, external/LibRaw-cmake, cie_colorimetric_tables.c, ThreadSafetyAnalysis.h | DATA/DELETE | Move needed tables into owned data; dependencies use FreeCM source roots only; delete vendored/build shims |

### 3.5 Control, jobs, and application lifecycle

| ID | Owner | Action | Acceptance gate |
| --- | --- | --- | --- |
| J1 | control/control*, jobs*, progress*, signal* | CORE/DELETE | SerialExecutor/task-handle/cancellation/progress/close resource contracts; no detached/global controller |
| J2 | control/jobs/control_jobs* | DELETE/map use case | Delete old job wrapper after command/service contracts cover it |
| J3 | control/jobs/develop_jobs* | CORE/DELETE | Preview/render/export queue, supersede/cancel/close tests |
| J4 | control/jobs/film_jobs* | CORE/DELETE | Folder/import batch/reopen owned by CatalogService |
| J5 | control/jobs/image_jobs* | CORE/DELETE | Asset mutation, duplicate/missing/error service contract |
| J6 | control/jobs/sidecar_jobs* | DATA/DELETE | Explicit XMP read/write policy, original safety, conflict/rollback before deletion |
| J7 | control/conf*, settings.h, crawler* | DELETE or Ravo settings port | Move only typed setting with a product consumer; keep no old-key compatibility shell |

### 3.6 Old UI, views, and Lighttable/libs (delete, do not port)

| ID | Module / owner | Action | Deletion gate |
| --- | --- | --- | --- |
| U1 | bauhaus/* | DELETE | Every IOP UI is consumed by Studio or headless recipe; do not create a Qt/Bauhaus adapter |
| U2 | dtgtk/* | DELETE | button/range/expander/thumbnail/thumbtable/culling and similar have no algorithm consumer |
| U3 | gui/gtk*, workspace*, accelerators*, context_menu*, system_commands* | DELETE | Studio command registry/composition covers required intent |
| U4 | gui/color_picker_proxy*, guides*, hist_dialog*, import_metadata*, metadata_tags*, log_history* | DELETE | Move required calculation/metadata service first; do not port GTK dialog/picker |
| U5 | gui/preferences*, presets*, styles_dialog*, about*, splash* | DELETE/DATA | Complete typed settings/style schema first; delete old windows/resources |
| U6 | views/darkroom.c | DELETE | All Develop consumers are in Studio; old view/module ABI reaches zero |
| U7 | views/lighttable.c | DELETE | Gallery/import/review consumers are in Studio |
| U8 | views/view* | DELETE | Remove after U6/U7 and dynamic view loader |
| L1 | libs/import.c | DELETE | Import service plus Studio/CLI contracts cover it |
| L2 | libs/styles.c | DATA/DELETE | Complete style schema/import/reject |
| L3 | libs/image.c | DELETE | Asset actions/metadata/review service covers it |
| L4 | libs/select.c | DELETE | Studio multi/range selection tests cover it |
| L5 | libs/recentcollect.c | DELETE | Product decision and query contract for recent/filter |
| L6 | libs/filtering.c + libs/filters/* | CORE/DELETE | Map every filter field to LibraryQuery or explicit unsupported |
| L7 | libs/navigation.c | DELETE | Studio zoom/pan/navigation-state coverage |
| L8 | libs/histogram.c + libs/scopes/* | CORE/DELETE | Separate RGB histogram/waveform/vectorscope/split tests; current histogram/parade is only a subset |
| L9 | libs/modulegroups.c + header | DELETE | Studio Inspector grouping complete; delete old names/quick-access config |
| L10 | libs/backgroundjobs.c | DELETE | J1 progress/task presentation covers it |
| L11 | libs/masks.c | DELETE | S3/M1 canonical mask service plus Studio intents cover it |
| L12 | libs/ioporder.c | DELETE | Canonical recipe operation-order/version contract covers it |
| L13 | libs/tools/viewswitcher.c | DELETE | Studio Gallery/Edit command covers it |
| L14 | libs/tools/darktable.c | DELETE | Old brand/label tool has no consumer |
| L15 | libs/tools/flags.c | DELETE | Reject/flag review state covers it |
| L16 | libs/tools/colorlabels.c | DELETE | Colour-label service/Studio covers it |
| L17 | libs/tools/ratings.c | DELETE | Rating service/Studio covers it |
| L18 | libs/tools/lighttable.c | DELETE | Studio Gallery-mode command covers it |
| L19 | libs/tools/view_toolbox.c | DELETE | Studio view commands cover it |
| L20 | libs/tools/module_toolbox.c | DELETE | Studio Inspector/command registry covers it |
| L21 | libs/tools/filmstrip.c | DELETE | Studio filmstrip/model contract covers it |
| L22 | libs/tools/hinter.c | DELETE | Studio visible status/error presentation covers it |
| L23 | libs/tools/image_infos.c | DELETE | Asset-metadata presenter covers it |
| U9 | libs/lib* / lib_api.h | DELETE | Delete dynamic Lighttable module loader after L1–L23 reach zero |
| U10 | main.c, cli/main.c, src/CMakeLists.txt, config.cmake.h, strings.h old app/CLI targets | DELETE | Ravo CLI/Studio/package three-platform loop and no old entry consumer |
| U11 | osx/*, win_msvc_compat.h, unistd.h, launcher/plist templates | DELETE | Ravo platform composition/package covers it |

### 3.7 Host resources, test evidence, and final directory cleanup

| ID | Path | Action | Acceptance gate |
| --- | --- | --- | --- |
| H1 | host/data/kernels/* | DELETE | All matching CPU algorithms accepted; any needed GPU mathematics is rewritten from Ravo CPU truth, not OpenCL |
| H2 | host/data/noiseprofiles*, wb_presets*, colour tables | DATA | Move to versioned Ravo calibration resource + schema/checksum, or delete when capability explicitly does not need it |
| H3 | host/data/styles/* | DATA | Move into Ravo test truth or delete after canonical style import/reject fixture |
| H4 | host/data/watermarks/* | DATA | Move resources needed by M5 into versioned product/test assets; delete unused resources |
| H5 | host/data/themes/*, pixmaps/*, shortcutsrc, darktable config XML/DTD | DELETE | Studio theme/icon/command truth source covers it; do not retain GTK resources |
| H6 | host/packaging/*, host/cmake/*, host tools/scripts/CMake | DELETE | RavoPackage three-platform install loop passes |
| E1 | legacy/tests source images, XMP, expected PNG | Keep evidence → DATA | As each IOP finishes, move remaining goldens/metadata summaries into Ravo fixture truth and check hash; read-only until queue clears |
| E2 | legacy/src/tests/*, legacy/tests/run, check/delta/performance binaries/scripts | DELETE | Ravo regression/performance/sanitizer entry points exist; old target/runner never runs |
| E3 | legacy/benchmarks/* | DELETE/DATA | Move valid gates into DevDocs/GPU_Baseline.md; do not execute old scripts |
| E4 | legacy/docs/*, RELEASE_NOTES.md, legacy README | Keep evidence → DELETE | Delete after algorithm/ownership conclusions enter ADR/ARCHITECTURE/TESTING |
| E5 | legacy/host, legacy/src empty directories, root legacy/ | DELETE | All ALG/CORE/DATA/DELETE rows clear, fixtures move, repository links/package checks pass |

### 3.8 Cross-cutting reliability and release blockers

The following are all unfinished. They are not a second feature queue, but can
block affected work or release:

- [ ] Gallery thumbnail virtualization, long-list memory, explicit worker/
  preview/cache budgets.
- [ ] Database non-writable/corrupt, cache corruption, full disk, moved source,
  batch cancellation, crash reopen.
- [ ] Schema-migration fixture, import-to-first-frame metric, and resource
  destruction after window/catalog close.
- [ ] Keyboard, focus, HiDPI, and accessibility.
- [ ] Windows/macOS/Linux configure/build/test, staged install, and installed
  application loop.
- [ ] Metadata/ICC and output-colour contract meet promised format gates.
- [ ] Name packaged-release product owner and code-review owner.
- [ ] Before release transition, prove backup/rollback, original safety, and no
  legacy production dependency.

GPU is outside the current queue. Create a dedicated TODO under
DevDocs/GPU_Baseline.md only after the relevant CPU path is accepted, goldens
are stable, and end-to-end measurements prove benefit.

Sections 3.1–3.7 list every current remaining module.
DevDocs/ProductRoadmap.md keeps only not-yet-frozen cross-layer design
constraints; it cannot be used to hide modules from this TODO. Implement only
the explicitly authorized current C14 tranche, and do not begin C15 audit or
implementation while C14 remains current.
Independent adapter or reliability work requires satisfied dependencies and
explicitly non-overlapping owners and files.

## 4. Completion gate for this TODO

Delete this document only when all are true:

- [ ] C14 completes its staged migration gate, and C14 and every later raised
  algorithm are accepted and removed from this document, with each accepted old
  owner retired in the same change.
- [ ] Shared old owners have only explicit consumers left and the remaining tree
  maps to leftovers in Ravo/MIGRATION.md.
- [ ] Cross-cutting reliability and promised platform-install loops meet release
  gates, or become independent root TODOs with owners.
- [ ] Original safety, schema migration, backup/rollback, and structured
  unsupported state have test evidence.
- [ ] Repository search, target/link graph, and runtime acceptance prove no Ravo
  ↔ legacy production dependency.
- [ ] Durable conclusions are synchronized into owning docs/ADRs/code/manifests/
  checkers/tests.
- [ ] This document contains no unfinished item and is deleted directly rather
  than moved into a DevDocs archive.
