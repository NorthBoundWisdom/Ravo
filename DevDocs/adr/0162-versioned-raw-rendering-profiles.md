# ADR-0162: Versioned RAW calibration and rendering profiles

- Status: Accepted design; implementation pending
- Date: 2026-09-30
- Extends: [ADR-0157](0157-agpl-rapidraw-tone-pipeline.md),
  [ADR-0151](0151-iq-cpu-gpu-consistency-gate.md)
- Execution: [RAW rendering profile gates](../TODO_RAW_RENDERING_PROFILES.md)

## Context

A photographer needs a usable, reproducible RAW starting appearance with edit
controls at their neutral positions. Matching the camera JPEG is a separate
capability from obtaining a neutral camera-to-working-space conversion. A fixed
exposure/shadow adjustment cannot substitute for either capability.

Ravo currently synthesizes a baseline from as-shot white balance, the LibRaw
camera matrix, RAW highlight reconstruction, RapidRAW controls/Basic, and Lab
USM. Explicit stored recipes remain authoritative. Camera Matching profiles,
Adobe DCP resources and a profile-selection contract are not implemented.

The inspected RapidRAW reference is revision
`772c76b752062427698b210046560b38871862d0` from
[CyberTimon/RapidRAW](https://github.com/CyberTimon/RapidRAW).
`src-tauri/src/app_settings.rs` selects AgX for RAW by default;
`raw_processing.rs` owns RAW normalization/calibration, and
`image_processing.rs` / `shaders/shader.wgsl` own its AgX matrices and response.
Ravo's accepted Basic operation is only one possible display response and is
not equivalent to that complete default pipeline. AgX itself is not a
camera-style matching profile.

## Decision

### Separate calibration, rendering and edits

1. **Camera calibration** converts normalized sensor samples into a declared
   scene-linear working space. It owns the chosen black/white interpretation,
   as-shot white balance, camera matrices and applicable camera/DNG exposure
   baseline. Each metadata term must have one named application point and a
   test proving it is not applied twice. Missing, malformed and unsupported
   metadata remain distinguishable; no guessed exposure offset is substituted.
2. **Rendering profile** defines tone response, gamut mapping and any colour
   look, with declared input/output spaces, working range and processing order.
   This is an immutable, versioned Engine resource, not a preset that writes
   user Exposure/Shadows sliders. It can use analytic transforms or bounded
   sampled tables; a 3D LUT is an implementation option, not the whole contract.
3. **User edits** remain explicit recipe operations. New profile-aware recipes
   distinguish scene adjustments from display finishing; existing operation
   order is never changed when reopening an older recipe. A profile switch is
   one undoable command that preserves the user's adjustment values.
4. **Output encoding** remains the existing output-colour/ICC owner. Every
   rendering profile declares whether its result is linear or encoded. A port
   must prove the single final encoding with ramps and profile metadata before
   preview or export admission.

The first built-in family is **Ravo Standard**, a camera-independent rendering
profile over correctly calibrated scene-linear colour. RapidRAW AgX is the
first candidate response to evaluate for this family, not an automatic default
change merely because the reference application selects it. Basic and Sigmoid
remain explicit legacy profile identities for reproducible existing work.

The second family is **Camera Matching**, keyed by camera make/model and
supported picture-style identity. Where needed it includes illuminant-dependent
calibration, colour corrections and a tone/look component. A profile name must
not imply a match to a model or style that has not passed its corpus gate.
Embedded camera JPEG is the appearance reference, not interchangeable linear
RAW input. Per-image JPEG fitting belongs to a later explicit reference-match
command, never an invisible import heuristic.

### Persist the resolved starting point

Profiles have a schema version, stable identifier, immutable revision/content
hash, resource bounds, colour-space declarations, applicability, provenance and
licence. The domain owns these value types; Engine owns numerical evaluation;
Services resolve, validate and bind profiles; adapters own resource I/O.
Desktop presents choices and state. QML does not parse profiles or perform
colour mathematics. There is no second RAW decoder or renderer.

Import resolves the default policy once and stores a baseline binding separately
from user-edit history, so a profile does not set `has_edits`. The effective
recipe includes this binding and its content identity. Editing, duplicating,
exporting, reopening, backup/recovery and offline work use the same binding.
Changing application defaults affects future imports only. An explicit batch
command changes existing bindings with revision/conflict checks and undo.

Catalog migration must preserve explicit existing recipes verbatim. Unedited
assets without a binding receive an explicit identity for the currently
accepted pre-migration baseline; migration must not re-render them with the
new Standard response. A replaced profile resource is a new revision, not an
in-place update. Missing or hash-mismatched referenced resources fail visibly;
the application must not silently select a different look.

An **Auto** default policy may choose a validated camera/style match, otherwise
an explicitly identified Standard profile. The resolved result records the
selection reason and exposes it to both Studio and CLI. Explicit Camera
Matching selection for an unsupported camera/style returns unsupported instead
of quietly substituting Standard.

### Selection, lifecycle and machine control

The existing service/command owner handles discovery, inspection, selection and
batch selection through a versioned CLI JSON contract. Responses expose the
resolved calibration/profile IDs, hashes, selection reason, relevant metadata
and effective recipe revision. Studio commands bind to observed selection and
recipe revisions; delayed results are rejected after selection/catalog changes.

Resource loading and evaluation stay on existing owner-managed workers with
bounded buffers and cancellation. Cache keys include calibration identity,
profile revision/hash, recipe and output profile. No global writable profile
singleton or network fetch during rendering is introduced. Shutdown cancels and
joins the existing tasks before releasing profile resources.

Gallery may use the camera JPEG as a labelled quick preview. When a selected
rendering profile or edit differs, the settled thumbnail, Develop and export
must use the same effective recipe; the transition must not claim those two
different appearances are identical. CLI image artifacts remain immutable,
no-replace and carry dimensions, MIME type, profile identity and hash.

CPU is the initial numerical reference. A profile becomes GPU-admitted only
after CPU/QRhi parity and resource/cancellation gates; the existing explicit
hybrid-preview policy applies before admission. A failed GPU pass cannot
silently use another transform.

### Acceptance and rollout

- First establish metadata/normalization observability and synthetic boundaries.
  Then compare a versioned Standard/AgX candidate against Basic and camera JPEG
  on a representative corpus. Default selection changes only after this gate.
- Camera Matching is admitted per camera/style, using calibrated targets and
  independent validation shots spanning illumination, skin, saturation,
  shadows and highlights. A few edited photographs cannot establish a general
  camera profile. Metrics include tone placement, highlight roll-off, clipping,
  neutral balance and colour error on suitable targets, plus visual review.
- Recipe round-trip, old-recipe pixel preservation, create/reopen/migrate,
  rollback, recovery, source preservation, cancellation, output conflicts,
  missing/corrupt profiles and latest-result rejection are required contracts.
- Preview, full-resolution export, crop/ROI and CPU/GPU paths must meet the
  existing IQ comparison rules. macOS evidence does not certify Windows/Linux.
- Distributed profile resources need reproducible generation and redistribution
  rights. Adobe or camera-vendor profiles are not bundled by assumption.

## Consequences and rejected alternatives

This makes new defaults reproducible while allowing Standard and Camera Matching
to evolve independently. It requires a catalog baseline-binding migration,
resource packaging and a measured profile corpus; it is not a curve-only patch.
The current runtime remains unchanged until those contracts are implemented.

Rejected: global Exposure/Shadows offsets; one display LUT for every camera;
per-image automatic JPEG fitting at import; camera-specific constants without
calibration evidence; overwriting existing Basic/Sigmoid operation semantics;
silently following the newest installed profile; and importing RapidRAW's
Rust/Tauri/wgpu ownership graph. The historical darktable AgX/IOP leftovers stay
closed under ADR-0106; an independently accepted Engine profile does not reopen
that migration.
