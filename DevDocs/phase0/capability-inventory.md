# Ravo Capability Inventory

## Purpose and status

This is a historical Phase 0 implementation inventory, not a promise to reproduce every
legacy IOP. [`Ravo/MIGRATION.md`](../../Ravo/MIGRATION.md) retains the
product-boundary authority; several creative and specialised modules are still
explicit candidates for a separate keep/remove decision. The table identifies
work that must not be silently implied by a successful Ravo build.

ADR-0007 supersedes the old sequencing constraint: a new private SQLite
catalog and minimal desktop viewer are authorized in M1. That decision does not
authorize legacy catalog migration or any operation marked deferred below.

The source inventory is collected from the leftover `add_iop(...)`
registrations in [`legacy/src/iop/CMakeLists.txt`](../../legacy/src/iop/CMakeLists.txt);
Ravo-accepted owners are removed from both lists when retired.
The generated fixture manifest records the 68 operation names currently
represented by XMP regression assets.  A `yes` in the fixture column means
only that one or more legacy XMP files name that operation; it is not a
validated Ravo compatibility result.

## Phase 1 reserved descriptors

These IDs are the versioned operation registry. P1 executes the develop
controls listed below on a linear RGB working buffer. It still does not claim
full legacy parameter compatibility. Unknown legacy operations remain
`unsupported`.

| Ravo operation ID | Legacy source operation | Planned first use |
| --- | --- | --- |
| `ravo.core.identity` | none | synthetic recipe and render-contract testing |
| `ravo.raw.prepare` | `rawprepare` | first RAW vertical-slice planning |
| `ravo.raw.demosaic` | `demosaic` | sensor-aware Bayer RCD/PPG and X-Trans Markesteijn 1/3-pass with bounded preview reduction and strict mode/CFA errors; dual/green matching remains with the old owner |
| `ravo.color.input` | `colorin` | first colour-chain planning |
| `ravo.core.exposure` | `exposure` | accepted v2 manual/black, metadata compensation, and original-RAW deflicker; exact default-unmasked legacy boundary |
| `ravo.color.temperature` | `temperature` | pre-demosaic/RGB four-channel scaling with explicit as-shot, camera-reference, late-reference and manual ownership |
| `ravo.color.channelmixerrgb` | `channelmixerrgb` | frozen V3 matrix normalization, explicit adaptation, XYZ gamut and LMS/RGB saturation-lightness calibration |
| `ravo.color.colorchecker` | `colorchecker` | explicit 0–49 ordered D50 Lab patch pairs, exact polynomial/thin-plate RBF fitting, eight frozen presets, and strict enabled default-unmasked v2 plus synthetic-v1 import boundary |
| `ravo.color.colorharmonizer` | `colorharmonizer` | exact 17-field profile-aware dt-UCS/RYB CPU core with canonical-ROI private recursive smoothing, Studio overlay/group/path/brush, and retired frozen IOP; frozen 0176 records 12/13 remain zero-smoothing import evidence |
| `ravo.color.colorcorrection` | `colorcorrection` | explicit-presence affine D50 Lab highlight/shadow a*/b* correction with strict enabled-v1 default-unmasked 0029/0092 import envelope |
| `ravo.core.contrast` | `filmicrgb` / `colisa` | P1 raster-input contrast and old-recipe compatibility; RAW Studio uses Sigmoid contrast |
| `ravo.core.highlights` | `filmicrgb` | P1 highlight compression |
| `ravo.core.shadows` | `shadows` / `filmicrgb` | P1 shadow lift |
| `ravo.core.whites` | none | P1 white-point control |
| `ravo.core.blacks` | none | P1 black-point control |
| `ravo.color.vibrance` | `vibrance` | P1 vibrance |
| `ravo.color.saturation` | none | P1 RGB average-saturation control; it is independent from frozen Color Contrast |
| `ravo.geometry.rotate` | `flip` | P1 90° quarter turns |
| `ravo.geometry.crop` | `crop` | P1 normalized free crop |
| `ravo.geometry.flip` | `flip` | horizontal/vertical mirror |
| `ravo.geometry.straighten` | `ashift` / `clipping` | P1 free-angle straighten before crop |
| `ravo.geometry.perspective` | `ashift` | scene-linear ShiftN homography, deterministic safe crop, three interpolation modes, bounded automatic line fit, and Canvas/mask-alpha composition |
| `ravo.core.gamma` | none | simplified P1 display gamma; it is not frozen `gamma` acceptance |
| `ravo.color.profilegamma` | `profile_gamma` | explicit pre-input logarithmic/gamma profile correction with frozen LUT/extrapolation |
| `ravo.core.tonecurve` | `tonecurve` | frozen C default RGB-linked ProPhoto curve; `lab`/`xyz`/`lab_independent` are explicit modes |
| `ravo.display.sigmoid` | `sigmoid` | RAW Standard SDR baseline; per-channel default and C `rgb_ratio` color processing |
| `ravo.raw.highlights` | `highlights` | Bayer CFA opposed/clip/inpaint/LCh before demosaic |
| `ravo.raw.hotpixels` | `hotpixels` | frozen Bayer four-neighbour threshold repair before highlights/demosaic |
| `ravo.raw.cacorrect` | `cacorrect` | frozen RawTherapee Bayer tile statistics, polynomial shift fit and optional color-shift avoidance |
| `ravo.raw.denoise` | `rawdenoise` | retired Bayer/X-Trans square-root five-level hat-wavelet threshold owner with versioned recipe/XMP import and RAW-only Studio/CLI control |
| `ravo.detail.denoiseprofile` | `denoiseprofile` | default wavelets + Y0U0V0 profile denoise on linear RGB |
| `ravo.geometry.lens` | `lens` | lensfun poly3/poly5 + linear TCA + manual vignette spline; lookup uses a versioned calibration table |
| `ravo.color.colorequal` | `colorequal` | selected 8-node dt UCS 22 RBF equalizer |
| `ravo.color.colorzones` | `colorzones` | optional three-curve D50 Lab/LCh zoning with full interpolation and mask contract; old owner retired |
| `ravo.effect.graduatednd` | `graduatednd` | `_compute_density` graduated exposure as the first local adjustment |
| `ravo.core.toneequal` | `toneequal` | 9-band [-8,0] EV RBF equalizer under Sigmoid |
| `ravo.color.colorbalance` | `colorbalance` | complete frozen v4 Lab D50/ProPhoto lift/gamma/gain and slope/offset/power contract; independent from Color Balance RGB |
| `ravo.color.colorbalancergb` | `colorbalancergb` | full Filmlight Yrg grading, DT UCS default and explicit JzAzBz; the old lift/gamma/gain approximation was removed |
| `ravo.color.colorcontrast` | `colorcontrast` | explicit schema-v2 D50 Lab per-axis affine contrast, both v1 upgrades, and strict 0038 default-unmasked import |
| `ravo.color.velvia` | `velvia` | frozen strength/bias low-saturation weighting with canonical masks and strict 0063 import; old owner retired |
| `ravo.color.lut3d` | `lut3d` | bounded profile-explicit `.cube` with tetrahedral/trilinear interpolation, content-addressed immutable cache, and explicit legacy-resource rejection; old owner retired |
| `ravo.color.monochrome` | `monochrome` | D50 Lab colour filter, shared bilateral base, highlight envelope and mask; old owner retired |
| `ravo.color.splittoning` | `splittoning` | full shadow/highlight HSL hue+saturation, balance, compression, mix and mask; old owner retired |
| `ravo.detail.sharpen` | `sharpen` | source-exact scale-aware D50 Lab unsharp mask; old owner retired |
| `ravo.repair.retouch` | `retouch` | ordered canonical-mask clone/heal/blur/fill with source geometry and wavelet scales; old owner retired |
| `ravo.detail.clarity` | `highpass` | large-radius local contrast |
| `ravo.effect.vignette` | `vignette` | radial darkening |
| `ravo.effect.grain` | `grain` | deterministic luminance noise |
| `ravo.effect.bloom` | `bloom` | highlight glow |
| `ravo.effect.soften` | `soften` | blur mix |
| `ravo.effect.dehaze` | `hazeremoval` | source-linear dark-channel/guided-filter dehaze; old owner retired |
| `ravo.color.output` | `colorout` | first colour-chain planning |
| `ravo.output.scale` | `finalscale` | output request negotiation |

The legacy adapter can map only a proven one-to-one operation with explicitly
handled parameters.  All other operation names must produce a structured
`unsupported_legacy_operation` diagnostic.  It must not retain opaque module
struct bytes or call the old dynamic module ABI.

## Legacy registry census

| Legacy IOP | Fixture | Phase 0 Ravo disposition |
| --- | --- | --- |
| `agx` | yes | queued by ADR-0015 as an explicit optional transform; Sigmoid remains default |
| `atrous` | yes | defer until shared denoise facilities exist |
| `basecurve` | yes | queued by ADR-0015; requires display-transform overlap contract |
| `bilat` | yes | defer until shared denoise facilities exist |
| `bilateral` | yes | defer until shared denoise facilities exist |
| `bloom` | yes | queued by ADR-0015; existing simplified Ravo bloom is not frozen-owner acceptance |
| `blurs` | yes | defer until shared blur facilities exist |
| `cacorrectrgb` | no | defer with RAW geometry capability |
| `censorize` | yes | queued by ADR-0015; requires mask/ROI graph |
| `colorize` | yes | defer until colour operation policy exists |
| `colormapping` | yes | defer until colour operation policy exists |
| `crop` | yes | defer until geometry/ROI contract exists |
| `demosaic` | yes | Ravo owns Bayer RCD/PPG and X-Trans Markesteijn 1/3-pass; keep the old owner only for unfinished dual/green matching |
| `diffuse` | yes | defer until shared denoise facilities exist |
| `filmicrgb` | yes | queued by ADR-0015 as an explicit optional transform; Sigmoid remains default |
| `finalscale` | no | reserved `ravo.output.scale`; render width/height currently drive the first bounded nearest-sample output path, not a complete scaling operation |
| `flip` | yes | defer until geometry/ROI contract exists |
| `grain` | yes | queued by ADR-0015; existing simplified Ravo grain is not frozen-owner acceptance |
| `highpass` | yes | defer until shared blur facilities exist |
| `liquify` | yes | queued by ADR-0015; requires deformation/ROI and mask contracts |
| `lowlight` | yes | defer until colour operation policy exists |
| `lowpass` | yes | defer until shared blur facilities exist |
| `mask_manager` | yes | defer until canonical mask graph exists |
| `negadoctor` | yes | queued by ADR-0015; requires negative-input color contract |
| `nlmeans` | yes | defer until shared denoise facilities exist |
| `overexposed` | no | diagnostic computation queued by ADR-0015; legacy presentation UI is deleted |
| `overlay` | yes | queued by ADR-0015; requires asset/ROI/mask composition contract |
| `rasterfile` | no | defer until raster adapter and mask contract exist |
| `rawoverexposed` | no | RAW diagnostic computation queued by ADR-0015; legacy presentation UI is deleted |
| `rawprepare` | yes | reserved `ravo.raw.prepare`; the exact frozen nop baseline is absorbed into crop and black/white normalization in the first RAW slice |
| `rgbcurve` | yes | defer until curve schema and colour policy exist |
| `rgblevels` | yes | defer until colour operation policy exists |
| `rotatepixels` | no | defer until geometry/ROI contract exists |
| `scalepixels` | no | defer until geometry/ROI contract exists |
| `shadhi` | yes | defer until shared denoise facilities exist |
| `soften` | yes | queued by ADR-0015; existing simplified Ravo soften is not frozen-owner acceptance |
| `vignette` | yes | queued by ADR-0015; existing simplified Ravo vignette is not frozen-owner acceptance |

## Data and export inventory

| Capability | Current Ravo state | Rationale |
| --- | --- | --- |
| Legacy XMP input | Parse only bounded strict mappings; exposure selects the greatest v5/v6/v7 revision independently of XML order, Color Checker accepts its sole evidenced enabled-v2 default-unmasked singleton plus a synthetic v1 upgrade, Color Correction accepts the enabled-v1 singleton/priority-zero/unnamed/default-unmasked envelope represented by 0029/0092, Color Contrast accepts the evidenced enabled-v2 record from 0038 plus a synthetic v1 upgrade, and Velvia accepts the exact enabled-v2 default-unmasked singleton from 0063 | Modified payload/version/enabled/blend/mask/multi/conflicting-revision state returns structured incompatibility; inactive Color Checker v2 tail planes are intentionally ignored and no old ABI bytes cross the boundary |
| Canonical recipe | Versioned JSON, immutable snapshots, explicit schema upgrades and operation presence, plus the bounded S3.1 typed mask-schema-v2 DAG | Mask v1 identity `all` upgrades strictly; graph bounds, topology, attached-frame ROI and normal mix are owned by ADR-0043. Existing operation-specific schema upgrades remain unchanged |
| RAW/JPEG/PNG/TIFF decode | Pinned LibRaw owns bounded Bayer and X-Trans RAW/DNG first-frame decode; private raster adapters own validated JPEG/PNG/TIFF decode and publication | Dual/green demosaic matching, complete rawprepare exactness and old wrapper zero-consumer deletion remain; recognized raster failures never silently fall through except TIFF RAW-container routing |
| Default display transform | `ravo.display.sigmoid` v1 on RAW; no implicit transform on display-referred raster inputs | Fixed per-channel linear-sRGB/Standard-SDR policy keeps CLI, Studio and export deterministic; advanced primaries and alternate transforms are unsupported |
| JPEG/PNG/TIFF/original export | Catalog export writes typed JPEG, typed PNG through private bounded libpng RGB8/RGB16 paths, typed TIFF through a bounded pinned private LibTIFF RGB8/RGB16/float encoder, or an original-byte copy; existing or racing targets win without replacement | CLI `catalog export` and Studio File → Export Photo share CatalogService and atomic no-replace publication. PNG defaults to 8-bit/compression 5, embeds resolved ICC plus known built-in cICP, and exposes PNG-qualified bit-depth/compression CLI flags. Its private RGB16 path consumes real host-endian samples; product PNG16 now uses engine-owned RGB16, while an RGB8 source still returns structured unsupported. TIFF defaults to uint8/Deflate-predictor/level 6/RGB/300 dpi, accepts 72–9600 inch resolution, embeds exact ICC, optionally applies the frozen grayscale test, and writes uint16/float16/float32 from engine-owned sources while still rejecting those requests from RGB8. Catalog snapshots writable metadata, capture values, and sorted tags once per rendered export; full/no-location/none privacy filters the JPEG/PNG/TIFF Exif/XMP/IPTC payload while retaining ICC. Source and sidecar are never changed, and no sidecar is generated automatically. Studio single- and multi-selection export share the same typed options. Remaining work is PNG pHYs, TIFF multipage masks, shared consumers, and format-plugin retirement. |
| Batch local storage | CatalogService owns strict portable filename expansion, complete known-conflict preflight, ordered typed item export, and partial-delivery error context | CLI `catalog export-batch` and Studio multi-selection share `{stem}`/`{asset_id}`/`{sequence}`/`{ext}` plus atomic no-replace publication. The GTK disk module is retired; its shared dynamic ABI waits for U10/J2 zero consumers (ADR-0068) |
| Masks and blending | Canonical all/linear-gradient/circle/ellipse/parametric/path/brush/group graphs, normal attachments, Studio overlay, and owned group-child authoring for Color Harmonizer and Graduated ND. Exposure, Color Checker, Color Correction, and Color Contrast retain exact default-unmasked legacy boundaries | Further blend modes, leftover GTK mask-manager consumers, and strict legacy mask/custom-blend import remain deferred |
| Catalog, history, styles | SQLite schema v5 owns reference assets, review, metadata, recipes, transactional history/snapshots, and portable complete `.rstyle.json` artifacts | Legacy catalog/dtstyle databases do not migrate; strict explicit XMP conversion is the only accepted old edit interchange |
| GPU/OpenCL/Metal | Explicitly out of Phase 1 | CPU-only reference work precedes any backend adapter |

The next inventory update must cite the Ravo test, legacy mapping, fixture, and
product decision that changes a row.  A compile-only change is not sufficient
evidence to move any disposition to "Ravo accepted".
