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
| `ravo.raw.demosaic` | `demosaic` | first RAW vertical-slice planning |
| `ravo.color.input` | `colorin` | first colour-chain planning |
| `ravo.core.exposure` | `exposure` | accepted v2 manual/black, metadata compensation, and original-RAW deflicker; exact default-unmasked legacy boundary |
| `ravo.color.temperature` | `temperature` | pre-demosaic/RGB four-channel scaling with explicit as-shot, camera-reference, late-reference and manual ownership |
| `ravo.color.channelmixerrgb` | `channelmixerrgb` | frozen V3 matrix normalization, explicit adaptation, XYZ gamut and LMS/RGB saturation-lightness calibration |
| `ravo.color.colorchecker` | `colorchecker` | explicit 0–49 ordered D50 Lab patch pairs, exact polynomial/thin-plate RBF fitting, eight frozen presets, and strict enabled default-unmasked v2 plus synthetic-v1 import boundary |
| `ravo.color.colorharmonizer` | `colorharmonizer` | exact 17-field profile-aware dt-UCS/RYB smoothing-zero CPU core; frozen 0176 records 12/13 are parameter evidence, while import, consumers, recursive-Gaussian smoothing, masks, and retirement remain later |
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
| `ravo.core.gamma` | none | simplified P1 display gamma; it is not frozen `gamma` acceptance |
| `ravo.color.profilegamma` | `profile_gamma` | explicit pre-input logarithmic/gamma profile correction with frozen LUT/extrapolation |
| `ravo.core.tonecurve` | `tonecurve` | frozen C default RGB-linked ProPhoto curve; `lab`/`xyz`/`lab_independent` are explicit modes |
| `ravo.display.sigmoid` | `sigmoid` | RAW Standard SDR baseline; per-channel default and C `rgb_ratio` color processing |
| `ravo.raw.highlights` | `highlights` | Bayer CFA opposed/clip/inpaint/LCh before demosaic |
| `ravo.raw.hotpixels` | `hotpixels` | frozen Bayer four-neighbour threshold repair before highlights/demosaic |
| `ravo.raw.cacorrect` | `cacorrect` | frozen RawTherapee Bayer tile statistics, polynomial shift fit and optional color-shift avoidance |
| `ravo.detail.denoiseprofile` | `denoiseprofile` | default wavelets + Y0U0V0 profile denoise on linear RGB |
| `ravo.geometry.lens` | `lens` | lensfun poly3/poly5 + linear TCA + manual vignette spline; lookup uses a versioned calibration table |
| `ravo.color.colorequal` | `colorequal` | selected 8-node dt UCS 22 RBF equalizer |
| `ravo.effect.graduatednd` | `graduatednd` | `_compute_density` graduated exposure as the first local adjustment |
| `ravo.core.toneequal` | `toneequal` | 9-band [-8,0] EV RBF equalizer under Sigmoid |
| `ravo.color.colorbalance` | `colorbalance` | complete frozen v4 Lab D50/ProPhoto lift/gamma/gain and slope/offset/power contract; independent from Color Balance RGB |
| `ravo.color.colorbalancergb` | `colorbalancergb` | full Filmlight Yrg grading, DT UCS default and explicit JzAzBz; the old lift/gamma/gain approximation was removed |
| `ravo.color.colorcontrast` | `colorcontrast` | explicit schema-v2 D50 Lab per-axis affine contrast, both v1 upgrades, and strict 0038 default-unmasked import |
| `ravo.color.velvia` | `velvia` | saturation weighted toward low-sat pixels |
| `ravo.color.monochrome` | `monochrome` | luma mix |
| `ravo.color.splittoning` | `splittoning` | shadow/highlight hue mix |
| `ravo.detail.sharpen` | `sharpen` | unsharp mask |
| `ravo.detail.clarity` | `highpass` | large-radius local contrast |
| `ravo.effect.vignette` | `vignette` | radial darkening |
| `ravo.effect.grain` | `grain` | deterministic luminance noise |
| `ravo.effect.bloom` | `bloom` | highlight glow |
| `ravo.effect.soften` | `soften` | blur mix |
| `ravo.effect.dehaze` | `hazeremoval` | simple airlight mix |
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
| `ashift` | yes | defer until geometry/ROI contract exists |
| `atrous` | yes | defer until shared denoise facilities exist |
| `basecurve` | yes | queued by ADR-0015; requires display-transform overlap contract |
| `bilat` | yes | defer until shared denoise facilities exist |
| `bilateral` | yes | defer until shared denoise facilities exist |
| `bloom` | yes | queued by ADR-0015; existing simplified Ravo bloom is not frozen-owner acceptance |
| `blurs` | yes | defer until shared blur facilities exist |
| `borders` | yes | queued by ADR-0015; requires canvas/export geometry contract |
| `cacorrectrgb` | no | defer with RAW geometry capability |
| `censorize` | yes | queued by ADR-0015; requires mask/ROI graph |
| `colorharmonizer` | yes | bounded smoothing-zero core accepted by ADR-0035; strict import, product consumers, S2.2/ROI smoothing, masks/presentation, and old-owner retirement remain later |
| `colorize` | yes | defer until colour operation policy exists |
| `colormapping` | yes | defer until colour operation policy exists |
| `colorreconstruct` | yes | defer with RAW reconstruction capability |
| `colorzones` | yes | queued by ADR-0015 as optional HSL zoning; `colorequal` remains default |
| `crop` | yes | defer until geometry/ROI contract exists |
| `demosaic` | yes | reserved `ravo.raw.demosaic`; the exact frozen nop baseline currently selects the first 3×3 Bayer implementation only |
| `diffuse` | yes | defer until shared denoise facilities exist |
| `dither` | yes | defer until output quantisation contract exists |
| `enlargecanvas` | yes | defer until geometry/ROI contract exists |
| `filmicrgb` | yes | queued by ADR-0015 as an explicit optional transform; Sigmoid remains default |
| `finalscale` | no | reserved `ravo.output.scale`; render width/height currently drive the first bounded nearest-sample output path, not a complete scaling operation |
| `flip` | yes | defer until geometry/ROI contract exists |
| `grain` | yes | queued by ADR-0015; existing simplified Ravo grain is not frozen-owner acceptance |
| `hazeremoval` | yes | defer until shared dehaze facilities exist |
| `highpass` | yes | defer until shared blur facilities exist |
| `liquify` | yes | queued by ADR-0015; requires deformation/ROI and mask contracts |
| `lowlight` | yes | defer until colour operation policy exists |
| `lowpass` | yes | defer until shared blur facilities exist |
| `lut3d` | yes | defer until LUT adapter and colour policy exist |
| `mask_manager` | yes | defer until canonical mask graph exists |
| `monochrome` | yes | defer until colour operation policy exists |
| `negadoctor` | yes | queued by ADR-0015; requires negative-input color contract |
| `nlmeans` | yes | defer until shared denoise facilities exist |
| `overexposed` | no | diagnostic computation queued by ADR-0015; legacy presentation UI is deleted |
| `overlay` | yes | queued by ADR-0015; requires asset/ROI/mask composition contract |
| `rasterfile` | no | defer until raster adapter and mask contract exist |
| `rawdenoise` | yes | defer with RAW decode capability |
| `rawoverexposed` | no | RAW diagnostic computation queued by ADR-0015; legacy presentation UI is deleted |
| `rawprepare` | yes | reserved `ravo.raw.prepare`; the exact frozen nop baseline is absorbed into crop and black/white normalization in the first RAW slice |
| `retouch` | yes | queued by ADR-0015; requires canonical mask graph and patch-source ownership |
| `rgbcurve` | yes | defer until curve schema and colour policy exist |
| `rgblevels` | yes | defer until colour operation policy exists |
| `rotatepixels` | no | defer until geometry/ROI contract exists |
| `scalepixels` | no | defer until geometry/ROI contract exists |
| `shadhi` | yes | defer until shared denoise facilities exist |
| `sharpen` | yes | defer until shared convolution facilities exist |
| `soften` | yes | queued by ADR-0015; existing simplified Ravo soften is not frozen-owner acceptance |
| `splittoning` | yes | queued by ADR-0015; existing simplified Ravo split toning is not frozen-owner acceptance |
| `velvia` | yes | queued by ADR-0015; existing simplified Ravo velvia is not frozen-owner acceptance |
| `vignette` | yes | queued by ADR-0015; existing simplified Ravo vignette is not frozen-owner acceptance |
| `watermark` | yes | queued by ADR-0015; requires deterministic resource/font and export contract |

## Data and export inventory

| Capability | Current Ravo state | Rationale |
| --- | --- | --- |
| Legacy XMP input | Parse only bounded strict mappings; exposure selects the greatest v5/v6/v7 revision independently of XML order, Color Checker accepts its sole evidenced enabled-v2 default-unmasked singleton plus a synthetic v1 upgrade, Color Correction accepts the enabled-v1 singleton/priority-zero/unnamed/default-unmasked envelope represented by 0029/0092, and Color Contrast accepts the evidenced enabled-v2 singleton/priority-zero/unnamed/default-unmasked record from 0038 plus a synthetic legacy-v1 upgrade under that presentation envelope | Modified payload/version/enabled/blend/mask/multi/conflicting-revision state returns structured incompatibility; inactive Color Checker v2 tail planes are intentionally ignored and no old ABI bytes cross the boundary |
| Canonical recipe | Versioned JSON, immutable snapshots, explicit schema upgrades and operation presence | Exposure v1 upgrades to v2 with explicit mode, black, percentile/target, and compensation flags; Color Checker preserves every ordered source/target Lab pair; Color Correction distinguishes absence from an explicitly present default five-parameter operation; Color Contrast distinguishes absence from explicit schema-v2 defaults and normalizes both frozen legacy v1 and prior Ravo `amount` v1 state |
| RAW/JPEG/PNG/TIFF decode | 16-bit Bayer RAW inspect/decode implemented through fixed LibRaw; raster inputs remain unsupported | Real `mire1.cr2` contract coverage exists, but other sensors and JPEG/PNG/TIFF still need fixture-backed behaviour |
| Default display transform | `ravo.display.sigmoid` v1 on RAW; no implicit transform on display-referred raster inputs | Fixed per-channel linear-sRGB/Standard-SDR policy keeps CLI, Studio and export deterministic; advanced primaries and alternate transforms are unsupported |
| JPEG/PNG/TIFF/original export | Catalog export writes typed JPEG, typed PNG through a private bounded libpng RGB8 encoder, typed TIFF through a bounded pinned private LibTIFF RGB8 encoder, or an original-byte copy; existing or racing targets win without replacement | CLI `catalog export` and Studio File → Export Photo share CatalogService and atomic no-replace publication. PNG defaults to 8-bit/compression 5, embeds resolved ICC plus known built-in cICP, and rejects RGB16 requests from the current RGB8 source. TIFF defaults to uint8/Deflate-predictor/level 6/RGB/300 dpi, accepts 72–9600 inch resolution, embeds exact ICC, optionally applies the frozen grayscale test, and rejects uint16/float16/float32 from RGB8. After asset lookup, TIFF snapshots the normalized destination and current writable metadata once; its main IFD owns bounded UTF-8 DocumentName/description/creator/copyright, deliberately omits title and EXIFIFD/IPTC/XMP, and never changes the source or sidecar. TIFF CLI exposes format-qualified sample/compression/level/grayscale flags under `--format tiff|tif` with canonical validation and structured unsupported high precision. S9/J6 complete packets, capture/timezone/GPS and XMP attach/history/sidecar policy, PNG resolution, TIFF multipage masks, explicit PNG CLI and TIFF Studio options, high-precision sources, shared consumers, and format-plugin retirement remain later |
| Masks and blending | Exposure, Color Checker, Color Correction, and Color Contrast accept their exact default-unmasked legacy states and reject mask/custom-blend/multi state structurally; the general mask graph remains deferred | Operation-specific retirement boundaries do not invent general mask/ROI semantics |
| Catalog, history, styles | New SQLite catalog/viewer authorized for M1; history and styles remain later work | The first product imports originals by reference and does not migrate the legacy catalog |
| GPU/OpenCL/Metal | Explicitly out of Phase 1 | CPU-only reference work precedes any backend adapter |

The next inventory update must cite the Ravo test, legacy mapping, fixture, and
product decision that changes a row.  A compile-only change is not sufficient
evidence to move any disposition to "Ravo accepted".
