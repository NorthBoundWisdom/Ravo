# Ravo Capability Inventory

## Purpose and status

This is a historical Phase 0 implementation inventory, not a promise to reproduce every
legacy IOP. [`TODO_REWRITE.md`](../../../TODO_REWRITE.md) retains the
product-boundary authority; several creative and specialised modules are still
explicit candidates for a separate keep/remove decision. The table identifies
work that must not be silently implied by a successful Ravo build.

ADR-0007 supersedes the old sequencing constraint: a new private SQLite
catalog and minimal desktop viewer are authorized in M1. That decision does not
authorize legacy catalog migration or any operation marked deferred below.

The source inventory was collected from the 76 non-comment `add_iop(...)`
registrations in [`legacy/src/iop/CMakeLists.txt`](../../../legacy/src/iop/CMakeLists.txt).
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
| `ravo.core.exposure` | `exposure` | P1 CPU develop control |
| `ravo.color.white_balance` | `temperature` / `colorin` | P1 temperature/tint multipliers on linear RGB |
| `ravo.core.contrast` | `filmicrgb` / `colisa` | P1 global contrast around mid-grey |
| `ravo.core.highlights` | `filmicrgb` | P1 highlight compression |
| `ravo.core.shadows` | `shadows` / `filmicrgb` | P1 shadow lift |
| `ravo.core.whites` | none | P1 white-point control |
| `ravo.core.blacks` | none | P1 black-point control |
| `ravo.color.vibrance` | `vibrance` | P1 vibrance |
| `ravo.color.saturation` | `colorcontrast` | P1 saturation |
| `ravo.geometry.rotate` | `flip` | P1 90° quarter turns |
| `ravo.geometry.crop` | `crop` | P1 normalized free crop |
| `ravo.geometry.flip` | `flip` | horizontal/vertical mirror |
| `ravo.core.gamma` | `gamma` / `profile_gamma` | display gamma on linear RGB |
| `ravo.color.colorbalance` | `colorbalance` | lift/gamma/gain factor |
| `ravo.color.colorcontrast` | `colorcontrast` | chroma steepness |
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
| `agx` | yes | product decision pending; no Phase 1 implementation |
| `ashift` | yes | defer until geometry/ROI contract exists |
| `atrous` | yes | defer until shared denoise facilities exist |
| `basecurve` | yes | product decision pending; no Phase 1 implementation |
| `bilat` | yes | defer until shared denoise facilities exist |
| `bilateral` | yes | defer until shared denoise facilities exist |
| `bloom` | yes | product decision pending; no Phase 1 implementation |
| `blurs` | yes | defer until shared blur facilities exist |
| `borders` | yes | product decision pending; no Phase 1 implementation |
| `cacorrect` | yes | defer with RAW geometry capability |
| `cacorrectrgb` | no | defer with RAW geometry capability |
| `censorize` | yes | product decision pending; no Phase 1 implementation |
| `channelmixerrgb` | yes | defer until colour operation policy exists |
| `colorbalance` | yes | defer until colour operation policy exists |
| `colorbalancergb` | yes | defer until colour operation policy exists |
| `colorchecker` | yes | defer until colour operation policy exists |
| `colorcontrast` | yes | defer until colour operation policy exists |
| `colorcorrection` | yes | defer until colour operation policy exists |
| `colorequal` | yes | defer until colour operation policy exists |
| `colorharmonizer` | yes | product decision pending; no Phase 1 implementation |
| `colorin` | yes | reserved `ravo.color.input`; the exact frozen nop baseline is absorbed by the first RAW slice, but general profile mapping remains deferred |
| `colorize` | yes | defer until colour operation policy exists |
| `colormapping` | yes | defer until colour operation policy exists |
| `colorout` | yes | reserved `ravo.color.output`; the first RAW slice has a fixed sRGB target, while general profile mapping remains deferred |
| `colorreconstruct` | yes | defer with RAW reconstruction capability |
| `colorzones` | yes | defer until colour operation policy exists |
| `crop` | yes | defer until geometry/ROI contract exists |
| `demosaic` | yes | reserved `ravo.raw.demosaic`; the exact frozen nop baseline currently selects the first 3×3 Bayer implementation only |
| `denoiseprofile` | yes | defer until denoise model and fixtures exist |
| `diffuse` | yes | defer until shared denoise facilities exist |
| `dither` | yes | defer until output quantisation contract exists |
| `enlargecanvas` | yes | defer until geometry/ROI contract exists |
| `exposure` | yes | reserved `ravo.core.exposure`; schema-6/v5 manual, zero-black, unblended, mask-free singleton history maps to `exp2(exposure_ev)` CPU execution; full fixture history and golden comparison remain incomplete |
| `filmicrgb` | yes | defer until colour operation policy exists |
| `finalscale` | no | reserved `ravo.output.scale`; render width/height currently drive the first bounded nearest-sample output path, not a complete scaling operation |
| `flip` | yes | defer until geometry/ROI contract exists |
| `gamma` | yes | defer until colour operation policy exists |
| `graduatednd` | yes | defer until mask/blend contract exists |
| `grain` | yes | product decision pending; no Phase 1 implementation |
| `hazeremoval` | yes | defer until shared dehaze facilities exist |
| `highlights` | yes | defer with RAW reconstruction capability |
| `highpass` | yes | defer until shared blur facilities exist |
| `hotpixels` | yes | defer with RAW decode capability |
| `lens` | yes | defer until lens database adapter is designed |
| `liquify` | yes | product decision pending; no Phase 1 implementation |
| `lowlight` | yes | defer until colour operation policy exists |
| `lowpass` | yes | defer until shared blur facilities exist |
| `lut3d` | yes | defer until LUT adapter and colour policy exist |
| `mask_manager` | yes | defer until canonical mask graph exists |
| `monochrome` | yes | defer until colour operation policy exists |
| `negadoctor` | yes | product decision pending; no Phase 1 implementation |
| `nlmeans` | yes | defer until shared denoise facilities exist |
| `overexposed` | no | diagnostics-only legacy behaviour; no Phase 1 implementation |
| `overlay` | yes | product decision pending; no Phase 1 implementation |
| `primaries` | yes | defer until colour operation policy exists |
| `profile_gamma` | no | defer until colour operation policy exists |
| `rasterfile` | no | defer until raster adapter and mask contract exist |
| `rawdenoise` | yes | defer with RAW decode capability |
| `rawoverexposed` | no | diagnostics-only legacy behaviour; no Phase 1 implementation |
| `rawprepare` | yes | reserved `ravo.raw.prepare`; the exact frozen nop baseline is absorbed into crop and black/white normalization in the first RAW slice |
| `retouch` | yes | product decision pending; no Phase 1 implementation |
| `rgbcurve` | yes | defer until curve schema and colour policy exist |
| `rgblevels` | yes | defer until colour operation policy exists |
| `rotatepixels` | no | defer until geometry/ROI contract exists |
| `scalepixels` | no | defer until geometry/ROI contract exists |
| `shadhi` | yes | defer until shared denoise facilities exist |
| `sharpen` | yes | defer until shared convolution facilities exist |
| `sigmoid` | yes | defer until colour operation policy exists |
| `soften` | yes | product decision pending; no Phase 1 implementation |
| `splittoning` | yes | product decision pending; no Phase 1 implementation |
| `temperature` | yes | defer with RAW colour capability |
| `tonecurve` | yes | defer until curve schema and colour policy exist |
| `toneequal` | yes | defer until colour operation policy exists |
| `velvia` | yes | product decision pending; no Phase 1 implementation |
| `vignette` | yes | product decision pending; no Phase 1 implementation |
| `watermark` | yes | product decision pending; no Phase 1 implementation |

## Data and export inventory

| Capability | Current Ravo state | Rationale |
| --- | --- | --- |
| Legacy XMP input | Parse only a bounded XML subset; return structured incompatibility outside proven mappings | Prevent old module ABI or opaque bytes from crossing the boundary |
| Canonical recipe | Version 1 JSON, immutable snapshots, explicit schema upgrades | Required by every future client |
| RAW/JPEG/PNG/TIFF decode | 16-bit Bayer RAW inspect/decode implemented through fixed LibRaw; raster inputs remain unsupported | Real `mire1.cr2` contract coverage exists, but other sensors and JPEG/PNG/TIFF still need fixture-backed behaviour |
| JPEG/PNG/TIFF/original export | Atomic RGB PNG implemented for the first RAW slice; other outputs remain unsupported | Existing-target conflict and bounded output are tested; bit depth, metadata, ICC, disk-full, JPEG/TIFF and original copy remain open |
| Masks and blending | Modelled as versioned data only | Pixel semantics require dedicated CPU tests and ROI rules |
| Catalog, history, styles | New SQLite catalog/viewer authorized for M1; history and styles remain later work | The first product imports originals by reference and does not migrate the legacy catalog |
| GPU/OpenCL/Metal | Explicitly out of Phase 1 | CPU-only reference work precedes any backend adapter |

The next inventory update must cite the Ravo test, legacy mapping, fixture, and
product decision that changes a row.  A compile-only change is not sufficient
evidence to move any disposition to "Ravo accepted".
