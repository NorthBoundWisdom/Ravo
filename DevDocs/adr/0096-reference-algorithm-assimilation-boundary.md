# ADR-0096: Reference-algorithm assimilation boundary

- Status: Accepted
- Date: 2026-08-30
- Extends: [ADR-0015](0015-migrate-all-non-ui-algorithms.md),
  [ADR-0047](0047-first-frame-raw-cache-lifecycle.md), and
  [ADR-0050](0050-ashift-rotation-and-export-scale.md)

## Context

Static review of ART (`6f511409a`), RawTherapee (`498f62378`), Filmulator
(`57fbaec`), and vkdt (`b95b3a0a`) found useful implementations for DNG
opcodes, Bayer/X-Trans demosaic, perspective crop, 3D-LUT interpolation,
camera-noise fitting, local detail, and physical film development. Those
projects also carry application-global state, UI, OpenMP or Vulkan scheduling,
subprocess colour tools, and per-file licensing that do not match Ravo's
ownership model.

The external implementations are not acceptance oracles. Remaining frozen
legacy algorithms and fixtures still define retirement compatibility, while
the DNG format and each new Ravo schema define input validity. In particular,
review found unchecked arithmetic, weak per-read bounds, silent unsupported
opcode skips, NaN coercion, and hidden demosaic fallbacks that Ravo must not
inherit.

## Decision

- Assimilation proceeds through the serial program in the root migration TODO:
  bounded DNG OpcodeList2/3, CPU RCD/PPG, perspective plus safe crop, X-Trans
  Markesteijn and denoise, profile-explicit 3D LUT, offline camera-noise
  calibration, and a measured local-detail selection. A predecessor must pass
  its acceptance gate before the next cross-layer tranche starts.
- Reference code may inform an independently integrated Ravo owner or a
  source-faithful port. Every borrowed file or material function records its
  upstream project, exact commit, source path, copyright, license, and local
  changes. Ravo remains GPLv3, but compatibility does not waive attribution or
  a more specific per-file license.
- Engine owns image mathematics and immutable task-local resources. Recipe owns
  versioned operation state. Services own cancellation, cache generations and
  publication. CLI and Studio consume that same service path; QML remains
  presentation-only. Third-party structs, handles, schedulers, globals, and
  mutable caches do not cross these boundaries.
- All byte parsers use checked cursor arithmetic, explicit payload/count/memory
  limits, finite-value checks and structured errors. Unknown mandatory DNG
  opcodes fail. Unknown optional opcodes may be skipped only when the owned
  decode result records that fact. Known malformed or unsupported correction
  payloads fail instead of yielding uncorrected pixels.
- Algorithms never hide an unsupported CFA, mode, profile, resource, or
  numerical failure behind a lower-quality implementation. A separately named
  user-selected mode is allowed; an implicit IGV/3×3/sRGB/identity fallback is
  not.
- External LUTs are immutable fingerprinted inputs with declared colour
  domains. The initial adapter is a bounded `.cube` contract and supports
  independently tested trilinear and tetrahedral interpolation. Missing,
  changed, malformed, non-finite, or unsupported files fail before publication.
- Camera-noise fitting is an offline deterministic `ravo` CLI calibration
  command. It writes a versioned checksummed artifact only to an explicit
  no-replace destination and never mutates a catalog, original, or implicit
  user configuration directory.
- The measured local-detail choice is `ravo.detail.texture`: a bounded
  two-band guided-luminance operation derived from the useful core of ART
  Texture Boost. Local Laplacian is rejected for this tranche. Only Texture
  introduces production schema and the shared scalar guided-filter primitive.
- Filmulator's reaction/diffusion/reservoir/agitation model remains outside the
  product. The isolated Ravo-owned prototype proves a distinct spatial result,
  but its measured CPU latency and parameter/resource surface fail the
  interactive gate. No Filmulator engine, dlib/OpenMP ownership, recipe state,
  or Studio controls enter production.
- Wholesale ART/RawTherapee engine or UI architecture, vkdt's Vulkan/GLSL
  graph, ART's OCIO/CTL/external-CLUT subprocess stack, vkdt OpenDRT/OIDN/
  spectral-film resources, and duplicate replacements for accepted Ravo
  dehaze/lensfun/denoise/sharpen are outside this program. GPU work remains
  behind `DevDocs/GPU_Baseline.md` CPU-golden and measured-benefit gates.

## Consequences

The selected reference projects can accelerate exact algorithms without
creating a second engine, scheduler, UI or control plane. Errors remain visible
and source pixels/caches publish only complete owned results. The serial
reference-algorithm program is complete: durable accepted and rejected results
live here, while the migration TODO retains no completed research checklist.

## First implementation: DNG opcodes

The first tranche follows Adobe DNG 1.7.1. Opcode lists are parsed as bounded
big-endian envelopes and retain file order. List2 GainMap runs after black/
white linear-reference normalization; List3 GainMap and FixVignetteRadial run
after demosaic while pixels remain camera RGB. WarpRectilinear is parsed and
reported on inspect, then skipped in the default colour decode so import and
Develop are not blocked by out-of-frame lens geometry. Each executed
List2/List3 operation clips to DNG's logical `[0, 1]` range.

LibRaw 0.21 retains selected-IFD opcode payloads without reliably propagating
the corresponding `parsedfields` bits. The adapter therefore treats a non-empty
owned `rawopcodes` payload as authoritative presence, validates the length and
pointer together, copies all supported values before decoder destruction, and
never exposes LibRaw storage. The parser accepts at most 4 MiB per list, gain
maps at most 4096 points per axis, and finite positive gains no greater than 64.

Automated evidence includes a libtiff-written CFA DNG crossing inspect, decode,
correction and source-immutability boundaries. The public Pixel 6 reference
`PXL_20211119_004121420.dng`, SHA-256
`c564190aa06cc8006abf3e856e9ca40f9d8af699b1a7f917b6dcfb72975fdf58`,
provides four List2 GainMaps and one List3 Warp for the reproducible CLI probe.
Ravo includes the Adobe-required notice, “This product includes DNG technology
under license by Adobe,” in the implementation and third-party notices.

## Second implementation: Bayer RCD and PPG

RCD 2.3 is the default Bayer owner; PPG is a separately selected compatibility
mode. Both accept only a complete RGB 2×2 CFA. Invalid modes, X-Trans, CYGM,
four-colour and incomplete CFA metadata return structured errors and never
select IGV, the removed 3×3 interpolator, or another hidden fallback. Full-size
work consumes exact decoded samples; bounded previews first area-reduce only
samples with the same CFA colour and phase. DNG List2 remains between
normalization and demosaic, while List3 remains after demosaic and before white
balance/input colour.

The Ravo owner adapts RawTherapee commit `498f62378`
`rtengine/rcd_demosaic.cc` and its bundled LibRaw PPG implementation. It keeps
RCD's 194-pixel tiles, 176-pixel output step and nine-pixel algorithm border,
but replaces OpenMP, globals and UI callbacks with Ravo row-task cancellation
and task-local storage. PPG operates in floating point and preserves positive
headroom instead of applying LibRaw's integer clip. Recipe absence means RCD;
PPG serializes as `ravo.raw.demosaic` v1 and persists through CLI `--set
demosaicModeIndex=1`, Catalog, styles and Studio's RAW panel.

Automated evidence includes smooth colour fields, a high-contrast monochrome
edge with false-colour bounds, cancellation/allocation estimates, duplicate and
unsupported-state rejection, source immutability, and quantized RCD/PPG goldens
from `Ravo/tests/fixtures/frozen/images/mire1.cr2`. File-level authorship, exact source paths,
licenses and Ravo modifications are recorded in the implementation and
`DevDocs/THIRD_PARTY_NOTICES.md`.

## Third implementation: Perspective and safe crop

`ravo.geometry.perspective` schema v1 is the single scene-linear owner for
rotation, vertical and horizontal correction, shear, safe-crop policy and
bilinear/Lanczos2/Lanczos3 interpolation. Its ShiftN-style matrix composition
is adapted from ART commit `6f511409afe28b2096c38483a6dfa3afcf167f5b`,
`rtengine/perspectivecorrection.{cc,h}`, which credits darktable `ashift` and
ShiftN. Ravo replaces application globals, OpenMP, UI callbacks and implicit
geometry state with checked homography inversion, finite input, a four-times
dimension-growth cap, task-local row scheduling, cancellation and owned
publication. The deterministic maximal inscribed rectangle is searched and
refined in output space before integer pixel bounds are committed.

An enabled Canvas establishes the mask-evaluation basis. Perspective/legacy
straighten and subsequent normalized crop transform preview alpha through the
same geometry and dimensions as the image, using bounded bilinear sampling for
alpha. The stale attached frame is discarded after geometry. Post-Canvas
rotate/flip/lens and another masked consumer after composed geometry remain
explicit unsupported states; no guessed coordinate remapping is introduced.

Automatic correction is a bounded Ravo-owned analysis adapter: it renders an
in-memory preview no larger than 900 pixels on the long edge with current crop
and Perspective disabled, performs Sobel edge detection, non-maximum
suppression and an oriented Hough search capped at 48,000 edge points and 48
lines, then robustly fits the selected vertical, horizontal or full model.
No usable lines, a degenerate fit, cancellation, malformed input or allocation
failure returns a structured error and leaves the recipe unchanged. CLI and
Studio consume the same Engine/CatalogService path; the CLI reports guide
endpoints in normalized last-pixel coordinates.

Frozen v4/v5 generic `ashift` records map rotation, lens shifts and shear to
the canonical operation with Lanczos3. Specific-lens mode, unowned crop modes
or boxes, masks, custom blends, duplicate instances and ambiguous state reject.
The exact `0018-ashift` payload is positive field-layout evidence, but its full
history is not a positive end-to-end oracle because an unrelated
`mask_manager` record remains unsupported. Synthetic grids freeze all three
resamplers and robust analysis; Catalog and Studio tests cover save, reopen,
export, crop-analysis isolation and composed Canvas overlay. The opt-in Release
probe measures the real CatalogService manual-correction path without catalog
mutation. With those gates accepted, the exclusive legacy `ashift.c`, LSD and
Nelder–Mead sources, registration and two darkroom icons are retired; shared
Overlay/order/module-group/manual strings remain owned by later cleanup rows.

## Fourth implementation: X-Trans Markesteijn and RAW denoise

LibRaw's crop-phased `idata.xtrans[6][6]` is copied into `DecodedRaw` before
decoder destruction; X-Trans never reuses Bayer margin offsets. The Engine
accepts exactly the standard 8-red/20-green/8-blue layout. Recipe absence is a
sensor-aware default: RCD for Bayer and Markesteijn 3-pass for X-Trans.
`markesteijn1` and `markesteijn3` are explicit v1 values; RCD/PPG on X-Trans or
Markesteijn on Bayer return `demosaic_sensor_mismatch` without fallback.
`ravo inspect --json` exposes CFA family/size and the selected default without
requiring a render.

The scalar C++20 owner adapts the frozen repository commit
`f7ea869a2bd3daafd04186c49f72861b2a574102`,
`legacy/src/iop/demosaicing/xtrans.c`, and RawTherapee commit
`498f623784e33fd9a7077fcd8937fe0734033366`,
`rtengine/xtrans_demosaic.cc`. It retains Markesteijn's 122-pixel tiles,
four/eight direction candidates, BT.2020 YPbPr homogeneity selection and
1/3-pass policy. Ravo owns same-CFA preview reduction, mirrored boundary
extension, explicit 12/17-pixel border interpolation, finite validation,
task-local buffers, cancellation and complete-output publication. The memory
estimate accounts for every concurrent tile buffer rather than one nominal
scratch allocation.

The separate X-Trans `ravo.raw.denoise` path retains the frozen per-channel
nearest-neighbour fill, square-root variance stabilization, five-level
hat-wavelet soft threshold and square write-back to matching sensels. It uses
the same v2 threshold/five-band recipe as Bayer. A separate normalized output
plane prevents cancellation from partially mutating the decoded copy; its
four-float-plane peak is included in RAW preflight.

Synthetic 6×6 fields freeze sample preservation, smooth-scene accuracy,
1/3-pass differences, deterministic preview reduction, cancellation, source
immutability and strict sensor/mode errors. `Ravo/tests/fixtures/frozen/images/mire1-xtrans.raf`
freezes decoded CFA counts and a quantized 320-pixel Markesteijn golden; Catalog
publishes an Engine-rendered preview through the same service path. CLI,
Recipe, Catalog and Studio expose the four indexed choices. With the RAW
denoise parity gate accepted, the exclusive old `rawdenoise.c`, registration
and icons retire; `demosaic.c` and its helpers remain for unfinished dual/green
matching, while shared order/module-group/manual strings remain later cleanup.

## Fifth implementation: profile-explicit 3D LUT

`ravo.color.lut3d` schema v1 is an optional colour operation after Velvia. It
stores one `.cube` path, declared input and output spaces, tetrahedral or
trilinear interpolation, and strength. The Engine converts canonical linear
Rec.709 into the declared LUT primaries and transfer function, honours
per-channel `DOMAIN_MIN/MAX`, then converts the mapped result back and blends
strength in unbounded linear light. No output `[0,1]` clamp is introduced.

The private parser accepts exactly bounded 3D `.cube` text: 2–65 nodes per
axis, red-fastest samples, at most 64 MiB, finite bounded samples, optional
title, and exact sample count. It rejects 1D, pyramid/Hald/OCIO/CTL inputs,
unknown directives and malformed domains. A thread-safe process LRU retains at
most eight immutable parsed snapshots identified by canonical path and a
complete-content fingerprint. Catalog validates the resource before recipe
commit and adds the fingerprint to persistent preview identity; missing or
changed-to-invalid data never reuses stale output. Cancellation and allocation
fail before result publication.

Recipe/Develop expose strict numeric and text fields; CLI adds `lut inspect`
and generic `--set-text`; Catalog probe/save/reopen/export and Studio use the
same service/engine owner. Independent cross-term goldens distinguish the two
interpolators, while an actual CLI subprocess proves persistence, read-only
probe, PNG export, resource mutation failure, and original immutability. The
three frozen LUT histories retain only mutable external paths and no LUT bytes
or checksum, so strict import returns
`unsupported_legacy_lut3d_resource` rather than guessing a local file. With
that explicit rejection strategy, the exclusive old CPU/OpenCL owners retire;
shared order/module-group/manual names and frozen fixtures remain evidence.

## Sixth implementation: offline camera-noise calibration

The calibration boundary accepts a strict v1 JSON document containing camera
make/model/ISO and 8–1024 weighted signal mean/variance observations. Signal is
explicitly black-subtracted uint16 sensor code value, not normalized display or
demosaiced RGB. Samples must be finite, positive-weighted, bounded to the uint16
domain, and span at least 256 code values. This keeps the fitted coefficients'
units explicit and prevents a plausible but unusable scale mismatch.

The Engine fits `variance = gaussian_variance + poisson_slope × signal` without
copying vkdt's graph or failure behavior. It computes a deterministic Theil–Sen
line, rejects residuals outside 4.5 median-absolute-deviation sigma, requires at
least eight and at least half of the input samples to remain, then performs
weighted non-negative least squares. Both coefficients and statistics are
finite and bounded. Insufficient samples/span/inliers, malformed values,
allocation and cancellation fail structurally; Ravo never writes vkdt's random
`a=100, b=1` fallback.

The JSON adapter canonicalizes the source document and emits a v1 profile whose
payload includes identity, units, fit policy, source SHA-256 and statistics.
The enclosing SHA-256 is verified by `ravo noise inspect`. Services publishes
the complete bytes through its cross-platform atomic no-replace owner, so an
existing or concurrently created destination survives unchanged. The CLI uses
only explicit input/output paths and has no catalog, original or user-profile
directory mutation. RAW histogram extraction and denoiser profile lookup are
deliberately not inferred by the fitter and remain later contracts.

## Seventh implementation: bounded Texture

The local-detail comparison used independent test-only CPU prototypes for
ART-style two-band Texture Boost and a six-gamma Local Laplacian. On the
committed synthetic fields, both stayed below their halo bounds, but Texture
produced 1.93184× texture deviation with 24,576,000 bytes of prototype scratch;
Local Laplacian produced 1.19862× with 27,852,968 bytes. Accepted Sharpen
produced 1.68146× and Tone Equalizer 1.27869×. Texture changed mean luminance by
about 0.00456, while Tone Equalizer changed it by about 0.08699. This establishes
a distinct texture-scale outcome instead of another edge sharpener or broad
tone control.

`ravo.detail.texture` schema v1 therefore owns strength `[-2,2]`, detail scale
`[0.01,100]` original-input pixels and one to five iterations in explicit
linear Rec.709. Strength zero is identity and remains the default. Develop
orders it before Sharpen; Studio exposes the photographic `[-100,100]` Texture
control first in Detail and keeps scale/iterations in a collapsed advanced
group. CLI, Catalog save/reopen/preview/export and Studio all use the same
Develop fields and operation owner.

The Engine forms Rec.709 luminance, bounds only the guided-filter guide to
`[1e-5,32]`, separates fine and four-times-coarser bands with the shared
self-guided filter, and applies a positive luminance ratio to RGB. This
preserves channel ratios and positive HDR highlights without an output
`[0,1]` clamp. Canonical ROI scale fixes authored radius across previews. Six
float planes beyond the normal borrowed/published RGB buffers are preflighted;
validation, allocation, non-finite data and cancellation before either filter,
each output row and publication fail without mutating the input or publishing
a partial result.

The opt-in Release probe measured the production operation at 7.63–8.20 ms per
960×640 committed RAW working buffer on the acceptance macOS host, below its
30 ms algorithm ceiling. Schema strictness, identity, signed strength,
canonical-scale consistency, hue ratios, unclipped HDR, cancellation, memory,
real RAW determinism, CLI, Catalog and QML ordering are contract-tested. ART's
mask graph, application globals, resampling owner, OpenMP scheduler and UI do
not enter Ravo.

## Final research gate: physical film development

A separate test-only Ravo-owned prototype models Filmulator's activation,
silver/developer reaction, seven-pass diffusion, active-layer/reservoir
exchange and agitation using the upstream default 12-step parameters. Equal
input values in different depletion neighborhoods differ by 10.55%, and
agitation changes the normalized output mean by 3.73%, so the result is
genuinely spatial and not reducible to the accepted Sigmoid or Tone Equalizer.

That distinctness does not clear the product gate. On the same two 960×640 RAW
working buffers, Release median latency was 154.85–157.12 ms and extra peak
storage was 29,491,200 bytes. Diffusion alone consumed 143.69–145.73 ms. This
is more than five times the complete 30 ms interaction budget before service,
packing and display, while the reviewed model exposes nineteen coupled
parameters. Ravo therefore retains only the deterministic research test and
provenance: it adds no recipe schema, dependency, production image path or UI.
A later dated decision may reopen a settled-only or accelerated design only
with a new user outcome and measured end-to-end budget.

## Rejected alternatives

- Copying a reference application's processing graph or UI wholesale.
- Treating a compatible repository-wide license as sufficient provenance for
  every file and embedded dataset.
- Silently ignoring malformed known opcodes, mapping NaN to zero, or switching
  demosaic algorithms when the selected CFA/mode is unsupported.
- Shipping every researched creative algorithm before measuring overlap with
  accepted Ravo controls.
- Productizing the current Filmulator physical-development prototype despite
  its distinct output, because its diffusion cost and coupled parameter model
  fail the interactive contract.
- Moving the program to GPU before deterministic CPU quality and performance
  baselines exist.
