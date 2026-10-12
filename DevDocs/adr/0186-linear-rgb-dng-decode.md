# ADR-0186: Owned linear RGB DNG decode

- Status: Accepted
- Date: 2026-10-12
- Scope: user-requested real-library RAW support and REL-01/IQ-00 reliability
- Extends: [Architecture](../ARCHITECTURE.md#preview)

## Decision

The Engine admits three-colour, non-CFA DNGs unpacked by the pinned LibRaw
provider into 16-bit interleaved RGB buffers. This includes Apple ProRAW. The
decoder owns a cropped RGB copy, subtracts channel/repeating black offsets,
and retains camera matrices, white levels, as-shot coefficients and orientation.
For this new RGB path, DNG `BaselineExposure` is source normalization (bounded
to ±16 EV), applied in scene-linear float independently of user exposure; its
absence means zero. Existing CFA rendering is unchanged.
It never substitutes the embedded JPEG or runs LibRaw's display processing.
`DecodedRaw` explicitly distinguishes CFA16 from linear RGB16; inspection calls
the latter `linear_rgb` with zero CFA dimensions and no demosaic algorithm.

Linear preparation normalizes sensor codes, applies supported DNG corrections
and white balance in camera RGB, then uses the existing Input Color, working
recipe and Output Color owners. Preview, native ROI and export share that
preparation. RGB samples require no demosaic. CFA-only repair operations retain
explicit applicability errors rather than silently changing algorithms.

The existing default opposed highlight intent gains a bounded RGB evaluation:
unclipped neighbouring RGB provides opponent chroma, and clipped channels are
raised from available opposing channels without changing unclipped samples.
Clip mode and partial strength remain explicit. CFA reconstruction is unchanged.
RAW histogram analysis counts the three native sensor samples per RGB pixel;
white-balance picking samples all three channels at each spatial location.
The immutable histogram context retains the already-applied source exposure;
deflicker subtracts it from its correction so source normalization is not
applied twice. CFA contexts retain zero and their existing numeric references.

All copies and preparation are row-cancellable. Dimensions, stride, buffer
length, black/white ranges and owned bytes are validated before allocation;
memory estimates include the three-channel source and preparation buffers.
LibRaw handles remain local RAII objects; no new thread, dependency, database
schema, transport, renderer or fallback is introduced.

## Evidence and validation

Local RapidRAW `raw_processing.rs` and RawTherapee `rawimage.cc` were read as
reference for distinguishing pre-demosaiced DNG and owning LibRaw colour samples.
Implementation remains Ravo C++20, using the existing pinned provider. No source
is copied from a reference repository and no reference application is run.

Acceptance requires synthetic linear-DNG decode/black/crop/orientation, numeric
RGB preparation and white-balance checks, histogram and memory accounting,
clipping/cancellation/malformed-state contracts, same-scale ROI equality,
CLI/service edit/reopen/export, immutable real iPhone originals, and unchanged
Bayer/X-Trans frozen fixtures. macOS evidence does not qualify Windows/Linux,
float DNG, arbitrary multispectral sensors or full camera colour quality.
