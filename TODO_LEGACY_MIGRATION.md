# Ravo Legacy Migration TODO

> **Status: in progress**
>
> **Updated: 2026-08-29**
>
> **Current execution status:** P1 and the accepted everyday P2 surface are
> complete; P3 has reached typed batch export, deterministic output dither,
> Canvas, final Output Frame, and deterministic text Watermark
> (ADR-0068–0071); H3's rejected `.dtstyle` examples are retired by ADR-0072.
> P3's remaining work is zero-consumer cleanup blocked on the named shared
> owners. C17 Color Zones is accepted and its old owner retired (ADR-0073).
> C18 Monochrome is accepted and its old owner retired (ADR-0074). Next Ready
> C20 Split Toning is accepted and its old owner retired (ADR-0075). Studio's
> default Develop grading workspace is accepted (ADR-0082) and is not leftover
> consumption. ADR-0083 accepts the eight-band Color Equalizer editor and Bayer
> CFA white-balance pick. Next leftover Ready remains P4's C21 Velvia contract;
> do not start it from this workspace round. Shared S8–S11, J5–J7,
> U5/U10/J2, format-wrapper,
> and dynamic-storage deletion still waits for their named old consumers to
> reach zero. Leftover GTK `mask_manager` / `libs/masks.c` wait for zero
> develop/history consumers. Do not start C15 or cacorrectrgb until a later
> exact tranche explicitly authorizes them. Full R1/R2 ALG (GainMap,
> RCD/PPG/X-Trans) remains outside the first-frame contract.

This document records only unfinished execution work, risks, dependencies,
verification commands, and acceptance gates. Current capability, architecture,
migration policy, leftover boundary, and test contracts are owned by
Ravo/README.md, Ravo/ARCHITECTURE.md, Ravo/MIGRATION.md, and Ravo/TESTING.md.
DevDocs/ProductRoadmap.md records cross-layer design constraints that are not
ready for execution.

## 1. Execution rules

- Select the highest-priority **Ready** user outcome in section 2.1. Do not
  select work merely because its old source file or inventory row comes next.
- Finish an active tranche before switching. If it is dependency-blocked, the
  next Ready outcome may advance when owners/files do not overlap and the
  blocker is recorded explicitly.
- One product tranche may cover several ALG/CORE/DATA/DELETE IDs when they are
  jointly required for one user-visible outcome. Acceptance and old-owner
  retirement still apply to every included owner; bundling is not permission
  to skip evidence or delete unrelated code.
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
- Cleanup and ABI deletion piggyback on an accepted user outcome or a proven
  zero-consumer boundary. They never displace a higher-value Ready capability
  merely to reduce the legacy file count.
- Mark an unrun platform or manual check as untested; never use historic results
  to represent current acceptance.

## 2. User-value execution queue

Priority is based on how often photographers need the outcome, whether it
blocks the import → edit → export loop, and data-loss/reliability risk. It is
not based on legacy directory layout, historical IOP order, or ease of deleting
an old owner. Within a priority, take the first Ready sub-slice after checking
the dependencies named in the owner inventory.

**Ready** means the named dependencies and evidence prerequisites are
satisfied, the owner/files do not conflict with active work, and the tranche is
explicitly authorized. A priority label alone does not waive those conditions.

### 2.1 Priority order

| Priority | User outcome and internal order | Owner IDs / readiness | Acceptance gate |
| --- | --- | --- | --- |
| P0 — finish active local-adjustment work | Mask overlay/group editing, path/brush authoring, and Color Harmonizer IOP retirement | `S3` remaining blend modes; leftover `M1`/`L11` GTK consumers; **Accepted product surface**. `C15` and `R4` remain explicitly unauthorized | Canonical graph/evaluator/service path with Studio overlay/group/path/brush; C14 retired; leftover GTK mask-manager files wait for zero-consumer proof |
| P1 — reliably open and view common photos | **Accepted first-frame.** JPEG/PNG/TIFF publication plus Bayer LibRaw/DNG inspect/decode, structured failures, unpack-before-publish, corrupt PNG cache miss, and close/reopen | ADR-0046/0047. Remaining: full `R1`/`R2` ALG, `I1` dispatcher, `S11` byte-budget LRU, leftover `J*` jobs, and wrapper deletion for `I2`/`I4`/`I5`/`I6`/`I7` | Common files open with correct orientation/profile/bit depth; corrupt/missing/oversized/cancelled inputs fail structurally; first preview, cache, close and reopen stay bounded |
| P2 — everyday Develop controls | **Accepted essential surface.** Flip/crop/rotation-only straighten, RGB levels/curve, Bayer RAW denoise import, highlight colour reconstruction, source-exact sharpen, source-linear guided dehaze, and ordered Retouch are accepted | Remaining `G3` pixel-aspect, full `G6` perspective, leftover `(int)` crop ROI, X-Trans rawdenoise, and dedicated RGB-curve/rawdenoise editors retain independent gates; they do not reopen M2 | One recipe/CLI/Catalog/Studio path per accepted control; real fixture and source-order CPU evidence; interactive preview/save/reopen/export and failure/cancellation tests |
| P3 — library and delivery workflow | **Dependency-blocked cleanup.** Product workflow is accepted; remove shared old owners only when their direct consumers reach zero | `S8`–`S11`, `J5`–`J7`, `U5`, `U10`, `J2`, `I1`, `I10`–`I13` cleanup gates | Fast large-library interaction; original-safe metadata/history rollback; reproducible presets; JPEG/PNG/TIFF/original export with conflict/cancel/disk-full and sidecar policy |
| P4 — commonly requested creative alternatives | Add popular optional looks only after the essential workflow: display transforms/LUT, Velvia, local contrast/blur variants, grain/vignette/bloom/soften | `T4`, `T5`, `T6`, `T1`, `C21`, `F2`, `F3`, `F4`, `F8`, `F12`, `M6`, `M7`, `M8`; Ready only when their colour/mask/filter foundations are accepted | Defaults remain Sigmoid/Color Equalizer; optional operations reproduce frozen CPU math and have full product persistence without changing default output |
| P5 — specialist and low-frequency tools | Film-negative, clustering/mapping, sensor corrections, advanced filters/deformation, diagnostics and uncommon formats follow demonstrated demand | `C15`, `C16`, `C19`, `T7`, `R4`, `R6`, `G2`, `G9`, `F5`–`F7`, `F9`, `F10`, `M3`, `M4`, `O2`, `O3`, `I3`, `I8`, `I9`; `C15`/`R4` stay forbidden until a later exact tranche explicitly authorizes them | Named user outcome, fixture/resource budget and explicit unsupported-state policy; no simplified substitute or speculative shell |
| P6 — retirement and repository cleanup | Delete dynamic ABI, old UI, shared globals, resources, runners and empty directories only after their last accepted consumer disappears | `D0.*`, remaining `S5`–`S7`/`S12`–`S14`, `J2`, `I15`, `U*`, cleanup-only `L*`, `H*`, `E*`; Not a standalone feature queue | Whole-repository zero-consumer search, synchronized freeze/inventory/docs, three-platform Ravo package/install evidence, and recoverable release transition |

Foundation IDs (`S1`, `S2`, `S4`, calibration data, and job owners) advance
with the highest-priority consumer that needs them; they are not independent
porting projects. A lower-priority item may move earlier only when it is a hard
dependency of a higher-priority outcome, and the same change must state that
relationship and the smallest validation set.

Independent adapter or reliability work may run only when its dependencies are
met and its owners and files do not overlap.

### 2.2 Continuous product gates

These gates apply to every priority and can block the affected tranche or a
release. They are user-safety work, not a second feature queue:

- [ ] Database non-writable/corrupt, schema-migration fixture, cache corruption,
  full disk, moved source, batch cancellation, crash reopen, backup/rollback,
  and original safety.
- [ ] Gallery thumbnail virtualization, long-list memory, explicit worker/
  preview/cache budgets, import-to-first-frame metrics, and resource destruction
  after window/catalog close.
- [ ] Keyboard, focus, HiDPI, and accessibility for each new Studio surface.
- [ ] Windows/macOS/Linux configure/build/test, staged install, and installed
  application loop for release-bound changes.
- [ ] Metadata/ICC and output-colour contracts meet the formats promised by the
  corresponding P1/P3 outcome.
- [ ] Name the packaged-release product owner and code-review owner; before
  release transition prove rollback and no legacy production dependency.

GPU is outside this queue. Create a dedicated TODO under
`DevDocs/GPU_Baseline.md` only after the relevant CPU path is accepted, goldens
are stable, and end-to-end measurements prove benefit.

## 3. Complete remaining-module owner inventory

This section is a lookup index for ownership, dependencies, fixtures, and
deletion gates. **Its section and row order is not execution priority**; only
section 2.1 chooses work. Snapshot baseline:

- legacy/src/iop/CMakeLists.txt has 40 unconditional IOP registrations plus the
  conditional `liquify`; every remaining owner has a row in section 3.2.
- legacy/src/libs/CMakeLists.txt has 23 source-backed modules/tools; retired
  export/copy_history/tagging/metadata/history/snapshots registrations are gone.
- legacy/src/views has darkroom/lighttable; imageio has four formats, one
  storage owner, and nine dispatcher/decoder owner groups.
- common, control, develop, GUI, and host resources are divided into ownership
  units in sections 3.3–3.7.
- The 158 fixture sets and five source images in legacy/tests remain read-only
  throughout algorithm migration; old runners never run.

Status terms: **paused** means accepted work remains but no new migration
tranche is authorized until feature convergence is reviewed. **Queued** waits
for its user-value priority, dependencies, and explicit authorization; it does
not wait merely for an earlier inventory row. **Delete** means no UI/ABI port.
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
| D0.3 | host/data/kernels exclusive kernels/entries for retired IOPs | Delete | Distinguish shared extended.cl/colour helpers; remove only entries/program registrations with no remaining consumer |
| D0.4 | common/iop_order.c, libs/modulegroups.c, usermanual_url.c retired names | Final deletion | These files still serve old UI/registry, including shared exposure order/module-group/manual names; remove with their DELETE batch after all algorithm consumers clear |
| D0.5 | unconsumed iop/choleski.h, equalizer_eaw.h, svd.h, unregistered useless.c | Delete | Search all includes/targets/fixture owners again; add retired list and pass freeze check |

### 3.2 IOP algorithm owner index (48 owners: 46 unconditional + 2 conditional)

The groups below make legacy ownership searchable. They do not define product
priority or serial order. fixture means static evidence exists, not that it is
covered.

#### A. Current colour scheduling and foundation

| ID | IOP / owner | Fixture | Status / dependency / special gate |
| --- | --- | --- | --- |
| C15 | colorize — iop/colorize.c | yes | Queued / ALG; depends on S1/M1; Lab hue/saturation/source mix |
| C16 | colormapping — iop/colormapping.c | yes | Queued / ALG; depends on S1; source/target statistics, clusters, determinism |
| C19 | lowlight — iop/lowlight.c | yes | Queued / ALG; depends on S1; low-light perception curve and LUT |
| C21 | velvia — iop/velvia.c | yes | Queued / ALG; simplified velvia is not acceptance; reproduce luminance/saturation weighting |

#### B. Display transforms, curves, and LUTs

| ID | IOP / owner | Fixture | Status / dependency / special gate |
| --- | --- | --- | --- |
| T1 | basecurve — iop/basecurve.c | yes | Queued / ALG; depends on explicit input-profile state; camera presets, exposure fusion, curve interpolation |
| T2 | rgbcurve — iop/rgbcurve.c | yes | Accepted linked/independent/preserve-colors, middle-grey uncompensate, leftover interpolators, and Studio Curves editor plus parametric regions (ADR-0052/0053/0084). Leftover IOP stays until freeze census is zero |
| T3 | rgblevels — iop/rgblevels.c | yes | Accepted recipe/engine/Studio path (ADR-0051). Auto-levels picker stays history-baked; leftover IOP remains until freeze census is zero |
| T4 | filmicrgb — iop/filmicrgb.c | yes | Queued / ALG; Sigmoid remains default; complete scene/display, chroma/gamut/reconstruction modes |
| T5 | agx — iop/agx.c | yes | Queued / ALG; Sigmoid remains default; AgX curve, primaries, gamut path |
| T6 | lut3d — iop/lut3d.c | yes | Queued / ALG; depends on explicit input/output profile state; LUT format adapter, missing/invalid file, interpolation |
| T7 | negadoctor — iop/negadoctor.c | yes | Queued / ALG; negative input, film base, scanner/profile, picker UI separated |

#### C. RAW preprocess and demosaic

| ID | IOP / owner | Fixture | Status / dependency / special gate |
| --- | --- | --- | --- |
| R1 | rawprepare — iop/rawprepare.c | yes | Queued / ALG; first-frame LibRaw crop/black/white/CFA/flip is accepted (ADR-0047). Remaining: DNG GainMap OpcodeList2/3 and the complete frozen crop/black/white path |
| R2 | demosaic — iop/demosaic.c + iop/demosaicing/* | yes | Queued / ALG; first-frame Bayer 3×3 is accepted (ADR-0047). Remaining: RCD/PPG, dual/green matching, X-Trans Markesteijn, memory/ROI. Do not treat 3×3 as ALG retirement |
| R3 | rawdenoise — iop/rawdenoise.c | yes | Accepted Bayer wavelet/threshold (ADR-0054). Remaining: X-Trans path; G1/G3 pull-together; leftover IOP until freeze census is zero |
| R4 | cacorrectrgb — iop/cacorrectrgb.c | no | Queued / ALG; separate from migrated pre-demosaic cacorrect; create synthetic/RAW fixture first |
| R6 | rasterfile — iop/rasterfile.c | no | Queued / ALG; depends on I1/M1; raster-source/mask ownership and no-fixture baseline |

#### D. Geometry, canvas, and scale

| ID | IOP / owner | Fixture | Status / dependency / special gate |
| --- | --- | --- | --- |
| G1 | flip — iop/flip.c | yes | Queued / ALG; leftover v2 orientation bits and Studio rotate/flip/crop-follow are accepted (ADR-0048). Remaining: mask/ROI distort, leftover NONE as EXIF undo, and `flip.c` retirement |
| G2 | rotatepixels — iop/rotatepixels.c | no | Queued / ALG; depends on G1; sensor/pixel rotation synthetic fixture |
| G3 | scalepixels — iop/scalepixels.c | no | Queued / ALG; depends on S3; interpolation/ROI/scale contract and synthetic fixture |
| G4 | crop — iop/crop.c | yes | Queued / ALG; leftover v1–v3 box import and Studio normalized crop/aspect lock are accepted (ADR-0049). Remaining: leftover `(int)` ROI truncation, export `ratio_n`/`ratio_d` snap, mask/distort, and `crop.c` retirement. Keystone is G6 |
| G6 | ashift — iop/ashift.c + ashift_lsd.c + ashift_nmsimplex.c | yes | Queued / ALG; rotation-only leftover import maps to canonical straighten (ADR-0050). Remaining: lens shift/shear, automatic crop, LSD/RANSAC fit. Do not treat straighten as complete G6 |
| G7 | finalscale — iop/finalscale.c | no | Queued / ALG; Catalog/CLI export `max_edge` is the product output-size owner (ADR-0050). Remaining: leftover hidden scaler retirement with G3; O1 is accepted |
| G9 | liquify — iop/liquify.c | yes | Queued / ALG; depends on M1/G4; deformation graph, ROI, cancellation |

#### E. Detail, denoise, blur, and local contrast

| ID | IOP / owner | Fixture | Status / dependency / special gate |
| --- | --- | --- | --- |
| F2 | highpass — iop/highpass.c | yes | Queued / ALG; depends on S3/M1; Lab/RGB blend and contrast path |
| F3 | lowpass — iop/lowpass.c | yes | Queued / ALG; depends on S3/M1; Gaussian/bilateral modes and saturation |
| F4 | shadhi — iop/shadhi.c | yes | Queued / ALG; depends on S3; bilateral shadows/highlights path |
| F5 | atrous — iop/atrous.c | yes | Queued / ALG; complete a-trous multiscale bands, boost/threshold, mask |
| F6 | bilat — iop/bilat.c | yes | Queued / ALG; frozen fast bilateral grid; do not merge it with bilateral by assumption |
| F7 | bilateral — iop/bilateral.cc + Permutohedral.h | yes | Queued / ALG; permutohedral lattice, memory budget, CPU determinism |
| F8 | nlmeans — iop/nlmeans.c | yes | Queued / ALG; depends on S3; patch/search/scattering and cancellation |
| F9 | diffuse — iop/diffuse.c | yes | Queued / ALG; iterative anisotropic diffusion, scale/ROI, blend |
| F10 | blurs — iop/blurs.c | yes | Queued / ALG; Gaussian/lens/motion modes; one blur is not a substitute |
| F12 | soften — iop/soften.c | yes | Queued / ALG; simplified soften is not acceptance; reproduce blur/mix/colour path |

#### F. Masks, compositing, repair, effects, and diagnostics

| ID | IOP / owner | Fixture | Status / dependency / special gate |
| --- | --- | --- | --- |
| M1 | mask_manager — iop/mask_manager.c | yes | Queued / DELETE; Studio owns mask presentation. Old dummy IOP remains while develop/history/styles still reference it |
| M3 | overlay — iop/overlay.c | yes | Queued / ALG; G5 Canvas is accepted; remaining M1/I1 resource lifecycle and alpha composition, including the frozen Overlay literal reference to retired `enlargecanvas` |
| M4 | censorize — iop/censorize.c | yes | Queued / ALG; depends on M1/S2; pixelate/blur/noise modes |
| M6 | bloom — iop/bloom.c | yes | Queued / ALG; simplified bloom is not acceptance; reproduce threshold/blur/mix |
| M7 | grain — iop/grain.c | yes | Queued / ALG; deterministic noise substitute is not acceptance; reproduce Lab/ISO/channel mode |
| M8 | vignette — iop/vignette.c | yes | Queued / ALG; simple radial darkening is not acceptance; reproduce shape/dither/colours |
| O2 | overexposed — iop/overexposed.c | no | Queued / ALG; migrate diagnostic calculation/threshold only, delete GTK overlay, create synthetic fixture |
| O3 | rawoverexposed — iop/rawoverexposed.c | no | Queued / ALG; RAW CFA threshold/channel diagnostic, create synthetic fixture |

### 3.3 Shared algorithms, pixelpipe, masks, and domain owners

| ID | Owner paths | Action | Dependency and acceptance gate |
| --- | --- | --- | --- |
| S1 | common/colorspaces*, chromatic_adaptation.h, illuminants.h, matrices*, custom_primaries*, gamut_mapping.h, darktable_ucs_22_helpers.h, color_*, colorchecker.h, curve_tools*, wb_presets* | CORE: move private colour science into engine | S1.1 frozen D50 Lab and S1.2 source-order dt-UCS bridges are complete behind bit-goldens; S1 remains incomplete and still needs explicit workspace/LUT owners for C3–C21/T1–T7, with no GTK/LCMS concrete type leakage |
| S2 | common/bilateral*, box_filters*, distance_transform*, dwt*, eaw*, eigf.h, gaussian*, guided_filter*, fast_guided_filter.h, heal*, locallaplacian*, nlmeans_core*, splines*, interpolation*, noiseprofiles*, bspline.h, luminance_mask.h, rgb_norms.h, focus*, histogram*, develop/noise_generator.h, develop/openmp_maths.h | CORE: move primitives into engine | Accept remaining primitives only for an explicitly reopened consumer and do not port OpenCL twins |
| S3 | develop/blend*, develop/blends/*, develop/masks/*, develop/masks.h | CORE: canonical mask/blend graph | Remaining: further source-backed blend-mode consumers and leftover GTK mask-manager files. Overlay, owned groups, and path/brush are accepted |
| S4 | develop/develop*, pixelpipe*, pixelpipe_cache*, pixelpipe_hb*, tiling*, imageop*, format* | CORE: converge into Engine facade + services cache/scheduler | `borders_helper.*` is retired with G5/G8. Every other old consumer reaches zero; shared exposure proxy/imageop hooks are cleanup state, not a runtime exposure owner; do not copy dynamic pixelpipe/global state |
| S5 | iop/iop_api.h, common/module*, module_api.h, dynload*, introspection.h, action.h, darktable*, darktable_api.h, poison.h, iop_group*, iop_order*, iop_profile* | DELETE dynamic IOP/module ABI and global composition | Delete after all remaining IOPs retire; built-in versioned operation registry remains |
| S6 | common/database*, database_schema*, sqliteicu* | CORE/DELETE | Compare against Ravo schema/FTS/ICU; move needed data contracts into SQLite adapter and delete remaining old catalog ABI |
| S7 | common/image*, film*, import_session*, grouping* | CORE/DELETE | Delete global image/film owner after Asset/import/folder/group contracts cover it |
| S8 | common/collection*, selection*, ratings*, colorlabels*, act_on* | CORE/DELETE | ADR-0059 accepts validated LibraryQuery fields and explicit unsupported legacy-only filters; selection/review are accepted. Remaining: delete shared old core only after view/import/action consumers reach zero |
| S9 | common/exif*, metadata*, metadata_export*, tags* | CORE/DATA | Bounded import/export, explicit atomic refresh, no-automatic-sidecar decision, and full/no-location/none privacy are accepted (ADR-0038/0040/0063/0064). Remaining: shared old image/crawler/storage/direct-writer consumers and deletion gates; no faces/map/GPS writeback or opaque packet compatibility |
| S10 | common/history*, history_snapshot*, styles*, presets*, undo* | CORE/DATA | Canonical recipe history/snapshot/rollback and complete `.rstyle.json` preset create/validate/apply/reject are accepted (ADR-0011/0065); L2 is retired. Remaining: shared old preset/history/undo consumers and zero-consumer deletion |
| S11 | common/cache*, image_cache*, mipmap_cache*, imagebuf* | CORE/DELETE | Ravo's atomic PNG cache, corrupt-signature miss, close/reopen rebuild, 512 MiB hard budget, deterministic persistent LRU, pre-commit cancellation, and accounting are accepted (ADR-0047/0067). Remaining: old cache/imagebuf owners wait for direct develop/imageio/job/GTK consumers to reach zero |
| S12 | common/atomic*, dtpthread*, resource_limits*, system_signal_handling*, utility*, datetime*, variables*, calculator*, file_location*, math.h, points.h, heap.h, tea.h, dttypes.h, debug.h, extra_optimizations.h, sse.h, grealpath.h, win_file_trash* | CORE/DELETE | Move only value/thread/path/platform logic with Ravo consumers; delete the rest with global core |
| S13 | common/curl_tools*, dbus*, gimp*, pwstorage/*, overlay* | DELETE or adapter | Create an independent port only for an explicitly active IOP/service; do not keep remote-publish/UI credential shells |
| S14 | common/opencl*, dlopencl*, opencl_drivers_blacklist.h | DELETE | Delete after CPU goldens complete, including the shared `host/data/kernels/basic.cl` exposure kernel/program path; Ravo GPU never reuses OpenCL API |

### 3.4 Codec, imageio, and output plugins

| ID | Owner | Action | Acceptance gate |
| --- | --- | --- | --- |
| I1 | imageio/imageio.c, imageio_module*, imageio_common.h | CORE/DELETE dispatcher | All formats/storage use Ravo ports; delete dynamic imageio ABI/global registry |
| I2 | imageio_libraw* | Adapter audit | Core accepted: pinned LibRaw inspect/embedded JPEG/Bayer first-frame decode and structured missing/directory/unrecognized/unpack/sensor/oversized errors (ADR-0047). Remaining: freeze census then delete `imageio_libraw.c`; I1 dispatcher is a separate owner |
| I3 | imageio_rawspeed* + external RawSpeed wiring | DELETE or independent decoder | Pinned `RawSpeed` source root is reserved. Remaining: format/performance/fixture decision against that root; do not silently fall back to LibRaw |
| I4 | imageio_dng*, common/dng_opcode* | ALG/adapter | Core accepted: `.dng` and TIFF RAW-container read through LibRaw first-frame (ADR-0047). Remaining: OpcodeList2/3 GainMap with R1; leftover `imageio_dng` writer (J2) and `dng_opcode` (S9) censuses; do not treat LibRaw crop as complete DNG opcode coverage |
| I5 | imageio_jpeg* | Adapter audit | Core accepted: content recognition, EXIF 1–8 scaling, strict APP2 ICC, opaque RGB8, and Catalog full-decode before publication (ADR-0023/0046). Remaining: zero-consumer census then delete `imageio_jpeg.c`; I11 export is a separate owner |
| I6 | imageio_png* | Adapter audit | Core accepted: content recognition, bit-depth/ICC/cICP/orientation, opaque RGB8, probe-time pixel validation, and Catalog full-decode before publication (ADR-0046). Remaining: interlaced/low-bit stay structured unsupported; zero-consumer census then delete `imageio_png.c`; I12 export is a separate owner |
| I7 | imageio_tiff* | Adapter audit | Core accepted: classic/BigTIFF recognition, 8/16 RGB/gray, ICC/orientation/alpha discard, probe-time pixel validation, Catalog full-decode before publication, and RAW-container routing that does not steal float/tiled/multi-page layouts (ADR-0046). Remaining: float/multi-page/tiled stay structured unsupported; zero-consumer census then delete `imageio_tiff.c`; keep QTiffPlugin; I13 export is a separate owner |
| I8 | imageio_qoi* + qoi.h | ALG/adapter | If Ravo keeps QOI, add decoder/encoder fixtures; otherwise explicit unsupported then delete |
| I9 | imageio_rgbe* | ALG/adapter | HDR RGBE decode/colour contract and fixture; do not treat it as ordinary raster |
| I10 | imageio/format/copy.c | DELETE/reuse original-copy service | Ravo exact-byte 64 KiB streaming, exclusive temp, atomic no-replace, conflict/cancellation/source/error/disk-full and CLI context are hardened; still blocked on I1/I14/U10/J2 zero consumers before removing plugin/registration |
| I11 | imageio/format/jpeg.c | Adapter/export | Core accepted: typed quality/subsampling, ICC APP2, and Catalog-owned Exif APP1 / XMP APP1 / optional IPTC APP13 embed before ADR-0032 publication. CLI and Studio expose quality plus `auto|444|440|422|420` subsampling, capture time/offset/GPS, privacy, and the no-automatic-sidecar policy. Remaining: shared imageio/storage/job consumers, then zero-consumer plugin/registration retirement. Keep the old owner. |
| I12 | imageio/format/png.c | Adapter/export | Core accepted: typed 8/16-bit depth and compression 0–9/default 5 propagate to a bounded private libpng RGB8/RGB16/opaque/non-interlaced ICC + known-built-in-cICP encoder and shared atomic publication. The RGB16 encoder path consumes a real host-endian 16-bit source. Catalog/Qt product PNG16 now renders engine-owned RGB16; an RGB8 source still returns `unsupported_png_16bit_source` without 8-to-16 expansion. Rendered PNG embeds one eXIf TIFF profile and one uncompressed XMP iTXt from the Catalog snapshot, including privacy-filtered capture time/offset/GPS; CLI and Studio expose the typed controls. Remaining: PNG `pHYs`, shared imageio/storage/job consumers, then zero-consumer plugin/registration retirement. Keep the old owner. |
| I13 | imageio/format/tiff.c | Adapter/export | Core accepted: typed uint8/uint16/float16/float32 and none/Deflate/predictor with level 1–9/default 6 plus conditional grayscale propagate to a bounded pinned private LibTIFF classic-LE opaque RGB8/RGB16/float contiguous-planar strip/exact-ICC encoder and ADR-0032 atomic publication. Baseline directory metadata owns 72–9600 inch resolution, bounded normalized-destination DocumentName, and privacy-filtered Catalog EXIFIFD/GPSIFD/XMP/IPTC; there is no sidecar change or post-publish rewrite. CLI and Studio expose the same typed options. Remaining: multipage masks, shared imageio/storage/job consumers, then zero-consumer plugin/registration retirement. Keep QTiffPlugin for separate I7 input and keep the old output owner until zero consumers. |
| I14 | imageio/storage/disk.c | DELETE/reuse CatalogService export | Strict bounded filename templates, whole-batch source/name/conflict preflight, shared typed options, item atomic no-replace, partial failure/cancellation context, CLI and multi-selection Studio are accepted; the disk module/registration are deleted (ADR-0068). Remaining: shared storage header/loader waits for U10/J2 zero consumers before dynamic ABI deletion |
| I15 | external/CMakeLists.txt, external/LibRaw-cmake, cie_colorimetric_tables.c, ThreadSafetyAnalysis.h | DATA/DELETE | Move needed tables into owned data; dependencies use FreeCM source roots only; delete vendored/build shims |

### 3.5 Control, jobs, and application lifecycle

| ID | Owner | Action | Acceptance gate |
| --- | --- | --- | --- |
| J1 | control/control*, jobs*, progress*, signal* | CORE/DELETE | First-frame import/preview cancellation and Catalog close/reopen resource drop are accepted (ADR-0047). Remaining: leftover global controller/progress/signal deletion |
| J2 | control/jobs/control_jobs* | DELETE/map use case | Delete old job wrapper after command/service contracts cover it |
| J3 | control/jobs/develop_jobs* | CORE/DELETE | First-frame preview cancel/close/reopen are accepted (ADR-0047). Remaining: leftover develop job wrappers and supersede/export queue retirement |
| J4 | control/jobs/film_jobs* | CORE/DELETE | Folder/import batch/reopen and RAW/raster publication-before-insert are owned by CatalogService (ADR-0047). Remaining: leftover film job deletion |
| J5 | control/jobs/image_jobs* | CORE/DELETE | Ravo asset mutation/duplicate/missing/error and transactional/quarantined removal are accepted (ADR-0062). Remaining: `common/mipmap_cache.c` speculative-load consumers and J4/S11 deletion gate |
| J6 | control/jobs/sidecar_jobs* | DATA/DELETE | No-automatic-sidecar policy, explicit strict XMP conversion, embedded-export distinction, and original/sidecar safety are accepted (ADR-0063). Remaining: old common/darktable.c, common/image.c, history/crawler/GUI direct consumers under S7/S9/J2/D0 |
| J7 | control/conf*, settings.h, crawler* | DELETE or Ravo settings port | Typed language persistence is accepted with corrupt-state repair and explicit write failure (ADR-0066). Remaining: old configuration headers/store and crawler wait for direct control/common/IOP/GUI consumers to reach zero; no other old key migrates |

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
| L3 | libs/image.c | DELETE | Asset actions/metadata/review service covers it |
| L4 | libs/select.c | DELETE | Studio multi/range selection tests cover it |
| L9 | libs/modulegroups.c + header | DELETE | Studio Inspector grouping complete; delete old names/quick-access config |
| L10 | libs/backgroundjobs.c | DELETE | J1 progress/task presentation covers it |
| L11 | libs/masks.c | DELETE | Studio owns mask intents; leftover develop proxy still references this GTK module |
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
| H5 | host/data/themes/*, pixmaps/*, shortcutsrc, darktable config XML/DTD | DELETE | Studio theme/icon/command truth source covers it; do not retain GTK resources |
| H6 | host/packaging/*, host/cmake/*, host tools/scripts/CMake | DELETE | RavoPackage three-platform install loop passes |
| E1 | legacy/tests source images, XMP, expected PNG | Keep evidence → DATA | As each IOP finishes, move remaining goldens/metadata summaries into Ravo fixture truth and check hash; read-only until queue clears |
| E2 | legacy/src/tests/*, legacy/tests/run, check/delta/performance binaries/scripts | DELETE | Ravo regression/performance/sanitizer entry points exist; old target/runner never runs |
| E3 | legacy/benchmarks/* | DELETE/DATA | Move valid gates into DevDocs/GPU_Baseline.md; do not execute old scripts |
| E4 | legacy/docs/*, RELEASE_NOTES.md, legacy README | Keep evidence → DELETE | Delete after algorithm/ownership conclusions enter ADR/ARCHITECTURE/TESTING |
| E5 | legacy/host, legacy/src empty directories, root legacy/ | DELETE | All ALG/CORE/DATA/DELETE rows clear, fixtures move, repository links/package checks pass |

Sections 3.1–3.7 list every current remaining module.
DevDocs/ProductRoadmap.md keeps only not-yet-frozen cross-layer design
constraints; it cannot be used to hide modules from this TODO. Section 2.1 is
the sole execution order; these owner tables cannot be used to promote an easy
legacy deletion over a higher-value Ready outcome.

## 4. Completion gate for this TODO

Delete this document only when all are true:

- [ ] Every remaining algorithm is accepted and removed from this document,
  with each accepted old owner retired in the same change.
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
