# Professional Workflow Gap TODO

> **Status: ranked; PW0 accepted; remaining ranks not independently ready**
>
> **Updated: 2026-09-01**

This queue ranks unfinished photographer-visible outcomes against Lightroom
Classic and Capture One. It is not a clone list, a second P0/P1, or permission
to start work. Current capability stays in `Ravo/README.md`. Undecided contracts
stay in `ProductRoadmap.md`. Algorithm exactness stays in
`TODO_LEGACY_MIGRATION.md`. Gallery latency stays in
`TODO_GALLERY_PERFORMANCE.md`. Private-corpus and Windows/Linux release
evidence stays in `TODO_PHOTO_MANAGEMENT.md` and keeps repository-wide
precedence.

A `PW*` row may move to Ready only after a dated ADR names owner, persisted
contract, failure/cancellation, and validation, and only when it does not
collide with those active queues. Completing a row means moving durable facts
into README/ARCHITECTURE/TESTING/ADR, then deleting the row here.

## 1. Execution rules

- Do not implement from this file alone. Write or extend the dated contract
  first; placeholder tables, empty Studio panes, and compatibility switches
  are out of scope.
- Keep SQLite as the live edit authority. Originals stay read-only except for
  an explicit destructive command. Do not add automatic adjacent-XMP writeback
  as a second live owner.
- Studio and CLI must share one service contract. QML displays state and
  forwards intents only.
- Fail closed on unsupported interchange. Do not emulate Adobe/Capture One
  pixels, catalogs, or AI masks by silent approximation.
- GPU remains exclusively under `GPU_Baseline.md`.
- Map, tethering, print, slideshow, and remote publishing stay leftovers from
  `MIGRATION.md` until a new dated product decision reverses that.

## 2. Already covered — do not reopen as gaps

Ravo already ships a professional *edit* core that Lightroom/Capture One users
often assume is missing:

- non-destructive canonical recipe, history, snapshots, session undo, and
  selective copy/paste and presets, including fail-closed Lightroom CRS import
  (ADR-0078/0086/0098);
- Add/Copy/Move ingest with conflict preflight and `YYYY/MM/DD` or preserved
  hierarchy (ADR-0102);
- ratings, color labels, reject, folder tree, named manual/smart collections,
  tags, title/creator/copyright, and validated `LibraryQuery` filters
  (ADR-0059/0103);
- scene-referred Light/Curves/Color Mixer/Color Balance RGB/Calibration stack,
  tone equalizer, lens, denoise, retouch, perspective, crop, soft proof, and a
  bounded mask graph (gradient/circle/ellipse/parametric/path/brush/groups);
- typed JPEG/PNG/TIFF/original-copy export with privacy modes;
- catalog recovery generations, verified backups, scheduled retention, and
  stable folder relink (ADR-0097/0099–0101).

The remaining gap is mostly DAM organization, ingest/delivery convenience,
masked-grading authoring on the everyday stack, and interchange — not “no
Develop.”

## 3. Rank order

| Rank | User outcome | Why it is obvious versus Lightroom/Capture One | Contract owner | Readiness |
| --- | --- | --- | --- | --- |
| **PW1 — versions, stacks, and N-up cull** | Virtual copies/variants, RAW+JPEG or burst stacks, and compare/survey of several photos | Culling is half of a professional day. Ravo rates/rejects one grid and compares Before/After of the *same* photo | ProductRoadmap stacking/versions; schema is one recipe per asset | Blocked |
| **PW2 — shoot ingest** | Card/camera ingest, rename templates, second-copy backup, HEIC/HEIF | First hour of a shoot. ADR-0102 explicitly left PTP/MTP, rename, second copy, DNG conversion, and Smart Previews out; HEIC/video have no product contract | ProductRoadmap ingest/format expansion | Blocked |
| **PW3 — metadata depth** | Hierarchical keywords, IPTC/location write, camera/lens/date facets | Client delivery and stock workflows. Ravo writes three catalog fields and flat tags; capture Exif is read-only; GPS writeback is unsupported | ProductRoadmap metadata; ADR-0064/S9 | Blocked |
| **PW4 — apply Develop to many** | Sync/copy current edits onto an explicit multi-selection with a field chooser | After culling, one grade is applied to tens of frames. Current copy/paste is a session clipboard onto destinations one command at a time, not a selection-wide sync | ADR-0078/0098 plus a new multi-asset mutation contract | Blocked |
| **PW5 — local grading on the stack** | Masked Light/Color/Curves instances, picker-assisted authoring, named extra blend modes | Capture One layers and Lightroom masking are how local color is done. Ravo’s mask graph exists but everyday grading tools are still mostly global; multi-instance legacy state still rejects | ProductRoadmap “Local adjustment expansion”; MR2 | Blocked |
| **PW6 — delivery export** | Long-edge/box resize, output sharpen, reusable export presets, restartable background jobs | “Export for web/client” is daily. Ravo already has typed codecs and batch atomic publication, but no geometry/sharpen stage, remembered preset, or durable job | ProductRoadmap “Export and background work” | Blocked |
| **PW7 — interchange** | Explicit XMP/catalog round-trip and external-editor round-trip without a second live authority | Studios move between Ravo, Lightroom, and Photoshop. Adjacent XMP writeback, legacy catalog import, and external editors are undecided; automatic sidecars stay forbidden (ADR-0063) | ProductRoadmap “Originals, catalogs, and interchange” | Blocked |
| **PW8 — finishing tools** | Optional grain, vignette, bloom, and extra display transforms as non-default recipe operations | Common look finishing. Defaults stay Sigmoid/Color Equalizer | `TODO_LEGACY_MIGRATION.md` MR4 (`M7`/`M8`/`M6`/`T4`/`T5`) | Blocked behind P0/P1 then MR0–MR3 |
| **PW9 — capture and presentation leftovers** | Tether, print, map, slideshow, publish | Lightroom/Capture One include these modules. Ravo records them as deleted leftovers, not ports | `MIGRATION.md` explicit leftovers | Unauthorized unless a new dated decision reverses that |

Interactive preview P90 and Gallery browse concurrency are professional-feeling
gaps that already have owners: do not duplicate them here.

## 4. Rank details

PW0 named library sets are accepted under ADR-0103 (schema v10). Durable facts
live in README/ARCHITECTURE/TESTING, not here.

### PW1 — versions, stacks, and N-up cull

**Current fact.** One canonical recipe per asset. Filmstrip and Gallery show
whole images. Comparison is Before/After or synchronized two-pane of one photo.

**Missing contract.** Version/variant identity (same original, several recipes),
stack grouping (burst or RAW+JPEG), pick of stack representative, and an N-up
cull view that is ID-based and revision-safe.

**Dependencies.** PW0 is not required, but both touch asset identity. RAW+JPEG
pairing needs an explicit duplicate/group policy (ADR-0059 currently leaves
group IDs unsupported).

**Risk.** Silent recipe forking, or using preview placeholders as cull oracles.

**Validation.** Variant create/delete/export isolation; stack collapse/expand;
N-up selection does not start N Develop pipelines; close/reopen preserves
group identity.

**Acceptance gate.** A photographer can keep two grades of one RAW and cull a
burst without duplicating files on disk.

### PW2 — shoot ingest

**Current fact.** ADR-0102 accepted Add, Copy, and Move from a local scanned
root, with name-preserving copy, same-stem XMP carriage, and no overwrite.

**Still out of scope (ADR-0102).** PTP/MTP, DNG conversion, renaming templates,
second-copy backup during ingest, metadata presets, and Smart Previews.

**Unrecorded format contract.** HEIC/HEIF (phone libraries) and video are not
accepted inputs. Architecture currently leaves Exiv2 video off.

**Dependencies.** Filesystem planner reuse; no new decoder inside QML.

**Risk.** Unique-name fallback on collision; treating HEIC as JPEG; background
DNG conversion that mutates sources.

**Validation.** Preflight of rename/second-copy/card paths with zero publication
on conflict; HEIC fail-closed until a decoder ADR exists; originals’
SHA-256/mtime unchanged for Add/Copy.

**Acceptance gate.** A card ingest can copy to a dated tree, write a second
verified copy, apply a bounded rename template, and refuse collisions without
guessing.

### PW3 — metadata depth

**Current fact.** Writable catalog title, creator, copyright, optional
description (CLI), and flat tags. Capture Exif is a refreshable read-only
snapshot. Faces/map/GPS writeback are unsupported.

**Missing contract.** Hierarchical keywords, IPTC-style fields needed for
delivery, optional GPS write into *catalog* (not the original), and facet
browsing by camera/lens/date without SQL in QML.

**Dependencies.** Export already embeds a bounded Catalog snapshot
(ADR-0038/0040/0064); new fields must define embed vs omit.

**Risk.** Writing originals or adjacent XMP “to stay compatible.”

**Validation.** Transactional multi-select tag/IPTC edits; hierarchy rename
without losing membership; export privacy still strips location on request;
refresh from source does not clobber catalog-only keywords.

**Acceptance gate.** A job can be keyworded hierarchically and exported with a
defined IPTC subset, originals untouched.

### PW4 — apply Develop to many

**Current fact.** Selective Copy/Paste Parameters uses the same field chooser
as presets and pastes onto destinations through ordinary history/undo
(ADR-0078/0098). Develop itself operates on the active photo.

**Missing contract.** One explicit command that applies a chosen field set to
every ID in the current selection (or a named PW0 set), with per-asset
revision checks, partial-failure reports, and cancellation that leaves
completed photos committed.

**Dependencies.** Existing chooser/merge owner; do not invent a second recipe
model.

**Risk.** Resetting unselected fields; applying pending in-memory sliders from
the wrong photo.

**Validation.** Multi-asset dry-run preflight, stale-revision rejection,
cancellation mid-batch, reopen equality of each recipe.

**Acceptance gate.** Ten selected photos receive the same Exposure/WB subset
and keep unrelated local edits.

### PW5 — local grading on the stack

**Current fact.** Canonical mask DAG and Studio authoring exist
(ADR-0043–0045). Color Harmonizer and Graduated ND consume them. Everyday
Light/Color/Curves remain global. Historic blend modes and extra masked
operations are undecided. Legacy mask/custom-blend/multi-instance import still
rejects.

**Missing contract.** Which grading operations may carry an owned mask; whether
multiple instances of one operation are allowed; picker/histogram-assisted
authoring that keeps graph math in C++.

**Dependencies.** ProductRoadmap local-adjustment bullets; MR2 for additional
blend modes. Do not wait on AI subject detection.

**Risk.** QML-owned mask pixels; coordinate drift after Canvas/Perspective/crop.

**Validation.** Preview/export/reopen equality with attached masks; geometry
after crop/perspective; cancellation/resource bounds; no change to unmasked
defaults.

**Acceptance gate.** A radial or brush mask can grade Color Balance RGB (or
another named consumer) through the same recipe CLI/Studio path.

### PW6 — delivery export

**Current fact.** Foreground typed batch export, atomic no-replace, bounded
filename template, and codec options are accepted (ADR-0068). Text watermark
is a built-in 5×7 ASCII font (ADR-0071). No long-edge resize, no output
sharpen, no remembered export preset, no durable background job.

**Missing contract.** Output geometry (long edge, box, don’t enlarge), when
sharpen runs relative to resize/watermark/frame, preset publication beside the
library, and a restartable job that does not own a second encoder.

**Dependencies.** ProductRoadmap export bullets; watermark/font resources also
live there.

**Risk.** Resizing inside QML; overwrite/skip/unique-name guessing; treating
display-referred sharpen as a Develop recipe default.

**Validation.** Preflight conflicts; pixel size of JPEG/PNG/TIFF; cancel leaves
completed files; reopen does not change recipes; preset apply is exact.

**Acceptance gate.** A saved “web JPEG 2048 px” preset exports a selection in
the background and survives application restart without rewriting sources.

### PW7 — interchange

**Current fact.** No automatic sidecar attach/read/write/watch (ADR-0063).
`recipe import-xmp` is explicit and fail-closed, including Lightroom CRS.
Rendered XMP is newly embedded. Ravo does not open a Lightroom or frozen
darktable catalog in place.

**Missing contract.** Optional *explicit* sidecar or catalog conversion with
conflict/authority rules; external-editor round-trip that snapshots pixels,
waits, and writes a new raster asset or version rather than mutating the
original RAW.

**Risk.** Two live authorities; in-place catalog conversion; Photoshop as a
hidden renderer.

**Validation.** Conflict matrix (catalog newer, sidecar newer, both changed);
conversion artifact is read-only; originals’ bytes unchanged.

**Acceptance gate.** A user can export/import an explicit interchange package
and round-trip a TIFF to an external editor as a new catalog asset.

### PW8 — finishing tools

**Current fact.** Texture, dehaze, split toning, velvia, monochrome, 3D LUT,
and watermark exist. Grain, vignette, bloom, and extra display transforms
(`filmicrgb`/`agx`) remain MR4 leftovers.

**Dependencies.** P0/P1 evidence closeout, then Ready `MR*` order. Defaults
must stay Sigmoid/Color Equalizer.

**Validation.** Frozen CPU math, fixtures, CLI/Catalog/Studio persistence, no
default-output change.

**Acceptance gate.** Optional grain/vignette are recipe operations with goldens;
unedited photos still match the current baseline.

### PW9 — leftovers

Do not port GTK tethering, print, map, slideshow, or remote publishing to
advance this TODO. If product demand appears, record a new dated decision that
replaces the leftover, then add a Ready rank. Until then the gap is acknowledged
and unauthorized.

## 5. Suggested validation once a rank is authorized

Minimum set for any newly authorized `PW*` (adapt owners, do not skip):

```text
python3 configs/source_roots.py verify
python3 Ravo/tools/freeze_legacy_manifest.py --check
python3 Ravo/tools/check_capability_inventory.py
python3 Ravo/tools/check_freeze_reference.py
python3 Ravo/tools/check_ravo_dependency_boundary.py
cmake --preset mac_clang_debug -DBUILD_TESTING=ON
cmake --build --preset mac_clang_debug --target ravo_catalog_tests ravo_desktop_command_tests ravo_cli_tests
ctest --test-dir build/mac_clang_debug --output-on-failure -R 'CatalogServiceTest|StudioPresenterTest|StudioQmlContract'
```

Add the rank’s new service/CLI tests in the same change. Windows and Linux
remain untested until those hosts run. Do not describe a skip as a pass.

## 6. Do not do

- Do not start PW1–PW9 while `TODO_PHOTO_MANAGEMENT.md` release evidence is
  the active repository-wide gate, unless that gate names the owner as a hard
  dependency.
- Do not add empty Collections/Map/Print/Tether panes.
- Do not infer a monitor profile in QML, treat browse JPEG as Develop, or
  reuse RapidRAW/Adobe/Capture One code as a silent engine.
- Do not persist session filters as fake smart collections.
- Do not authorize C15 Colorize or `cacorrectrgb` from this file.
