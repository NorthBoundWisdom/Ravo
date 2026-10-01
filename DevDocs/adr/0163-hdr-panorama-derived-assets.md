# ADR-0163: Exposure-bracket and planar panorama derived assets

- Status: Accepted
- Date: 2026-10-01
- Supersedes: ADR-0153's unselected HDR/Panorama direction only

## Decision

The explicit user request admits both workflows as an exception to the
specialization/WIP freeze. Tethering remains deferred. These are new Ravo
workflows; they do not reopen the removed darktable application or IOP ports.

Engine owns synchronous immutable linear-working RGB registration and merge.
FAST-9/BRIEF correspondences, mutual ratio matching and deterministic RANSAC
estimate projective transforms. HDR uses one reference; panorama uses a
connected overlap tree and requires all selected frames to participate. Failed
registration is explicit. HDR may explicitly disable alignment for a tripod
sequence. No automatic unaligned retry or simple-overwrite seam is introduced.

HDR uses shutter × ISO / aperture² capture exposure, or one explicit EV per
selected frame. Missing metadata fails. Saturation-aware common RGB weights
combine normalized radiance; a configurable discrepancy threshold rejects moving
contributions against the best-exposed local anchor. All-clipped regions retain
the shortest exposure without claiming recovered detail. Engine results retain
scene-linear radiance normalized to median capture exposure (independent of
selection order); Catalog delivery applies a v1 luminance Reinhard shoulder
and writes an sRGB 16-bit TIFF. Float DNG/CFA merge and HDR-display delivery are
outside this version.

Planar panorama warps RGB, estimates scalar overlap exposure compensation,
finds a minimum-disagreement dynamic-programming seam and feathers across it.
Auto-crop finds the largest completely covered rectangle, including projective
wedges and holes. Without auto-crop, uncovered panorama pixels are black.
Cylindrical/spherical/360° projection, bundle adjustment, severe parallax and
multiband blending are not claimed.

## Ownership and publication

CatalogService reads explicit asset IDs, original identities, saved recipes and
baseline recipes. Inputs use original baseline linear pixels: saved global
grading, crop and local edits are not baked in. Source SHA-256/size/mtime are
checked before and after processing; original bytes are never written.

The service owns decode, bounded memory, TIFF encoding, image-artifact
verification, no-replace publication and registration. Default output lives in
`<catalog>.ravo/derived/<new-asset-id>/`, with adjacent versioned provenance
containing ordered source IDs/URIs/hashes, observed recipes, capture EVs,
transforms, crop origin, options, profile and output hash. This tree participates
in existing backup/restore. An explicit CLI output is externally located and
follows normal external-original backup policy.

Image and provenance publish before registration. Failed/cancelled pre-commit
attempts remove only their own files; cleanup failure is structured. Revision
preconditions are checked inside the existing SQLite import transaction, with
cancellation immediately before commit. Post-commit recovery errors retain
artifacts and report the committed asset/output. A process crash before
registration can leave unregistered inspectable files but cannot overwrite
originals or existing destinations. No database schema changes are introduced.

Bounds are 2–16 inputs, 80 million pixels per input/output, 50,000 pixels per
dimension, optional maximum input edge 16,000, default 2 GiB and hard 4 GiB owned
memory. Decode preflight includes native RAW scratch even for reduced output.
Canvas dimensions are checked before allocation. Allocation failure, missing or
corrupt files, output collision, source change, disconnected frames and stale
catalog revision fail explicitly. There is no GPU path or silent fallback.

## Clients and lifecycle

CLI `catalog hdr-merge` / `catalog panorama` requires repeated `--asset-id` and
observed `--revision`. JSON returns `ravo.photo_merge` v1 and
`ravo.image_artifact` v1 with path, MIME, dimensions, profile identity, byte count,
SHA-256 and persistent lifecycle. CLI owns no second renderer.

Studio exposes commands through the C++ registry and a dedicated QML options
dialog. C++ binds pending dialogs to ordered selected IDs, catalog identity and
observed revision. QML presents options and forwards intent. Execution uses the
existing owned catalog executor and cancel token; window destruction cancels
and joins work. Late UI publication checks shutdown and catalog identity.
Menu, palette, context menu and machine commands share the ready 2–16-selection
condition. Main.qml gains no orchestration.
Successful creation returns the originating selection to the full library,
newest imports first, and selects the derived result; source-folder/Last Import
filters cannot hide it. A changed selection retains its current presentation.

## References and validation

User-supplied darktable HDR weighting and RapidRAW registration/seam code are
algorithm references. The adapted darktable envelope retains GPL attribution.
Application, Rust tasks and state owners are not linked or copied. No dependency
is added. Source provenance lives in [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

Acceptance covers clipped-highlight/chromaticity truth, exposure registration,
moving pixels, connected/unordered panorama, seam exposure and crop coordinates;
invalid/featureless/cancel/memory failures; source preservation, immutable TIFF,
collision, cancellation after publication, transaction rollback, concurrent
revision, reopen/preview/export and backup verification; CLI JSON and Studio
dialog/revision/lifecycle contracts. Real bracket/panorama corpus and installed
three-platform package evidence remain release gates, not synthetic claims.
