# Ravo capability and developer reference

Ravo Studio is the desktop photo library, non-destructive RAW editor, and video
browser. The `ravo` CLI is its supported automation client. Both use the same
C++20 domain, services, and Engine; Qt Quick presents the desktop interface.

For a product introduction and downloads, start at the
[repository home](../README.md). For task instructions, use the
[user handbook](../userdoc/README.md). This file records the current source
baseline and build/command entry points. Published releases may lag `main`.

## Current workflows

| Workflow | Available behavior | Important boundary |
| --- | --- | --- |
| Import | Add, Copy and Move; recursive folder scan; early filenames and background thumbnails; media counts and bytes; duplicate preflight; organisation, rename and verified second copy | Move removes sources only after requested copies verify and the primary is cataloged. Explicit ingest transports reject Move. |
| Organise and review | Folders, filters, ratings, colours, Pick/Reject, hierarchical keywords, writable metadata, manual/smart collections, versions and stacks | Explicit relinking validates replacement identity; moving a file externally does not automatically update its catalog path. |
| Inspect | Gallery, Loupe, Survey, Fit, 30% and 1:1; pan, navigator, filmstrip; histogram, waveform, RGB parade, vectorscope and split scopes | View state does not change the recipe. The 30% long-edge mode can exceed native-pixel scale. |
| Develop | Exposure, white balance, curves, colour, tone, geometry, denoise, detail, repair and effects; mask-scoped local adjustments; presets and selected-parameter copy | Camera/CFA, operation, mask and colour-profile admission are explicit; unsupported combinations fail. |
| Revisit edits | Versioned recipes, history, snapshots, session Undo/Redo and Before/After comparison | Existing explicit Sigmoid/RapidRAW recipes retain their rendering. Newly synthesised RAW baselines use Sigmoid Standard SDR. |
| Video | MOV/MP4/M4V H.264, HEVC and standard ProRes; posters, metadata, duration, Loupe playback, seeking, mute/volume and original export | No video Develop, trimming or transcoding. HLG/PQ presentation is SDR; Dolby Vision is limited to supported compatible base layers. |
| Deliver | Single/batch JPEG, PNG, TIFF and original copy; explicit sizes, codec options, privacy and conflicts; exact companion JPEG; external-editor round trip | Video supports original copy only. No implicit overwrite or invented unique filename. |
| Recover | Recovery generations, verified catalog backup/restore, schedule/retention and preview rebuild | Backups exclude original media and previews. Restore requires an absent destination. |
| Settings | General/language, workspace panel sizes/reset, current-catalog automatic backups and Assistant connection | Global preferences are per user; backup policy is catalog state. Schedules run while Studio is open. |
| Automate | Versioned JSON, strict fields, revision checks, immutable image artifacts and local Studio sessions | CLI commands and desktop commands share owners; there is no UI-automation or direct-SQL mutation shortcut. |

### Photo processing and colour

The Engine covers Bayer RCD/PPG and standard Fujifilm X-Trans Markesteijn
demosaic, RAW preparation and repair, profile denoise, white balance and camera
calibration, ICC input/working/output profiles, proofing, exposure and tonal
controls, RGB/Lab curves, colour grading, texture/sharpening, lens and
perspective correction, clone/heal/blur/fill retouch, canvas, framing and output
effects. Brush, gradient, radial, path and range selections support local
photographic adjustments.

Discover the exact installed operation and parameter schemas with:

```text
ravo operations --json
ravo develop-fields --json
ravo catalog fields --json
```

Synthesised RAW recipes include as-shot white balance, the camera input matrix,
Sigmoid Standard SDR and mild Lab sharpening. Explicit stored looks are not
reinterpreted. The accepted algorithms, colour invariants, mask/geometry limits
and schema ownership are specified in
[Architecture](../DevDocs/ARCHITECTURE.md) and the
[migration ledger](../DevDocs/MIGRATION.md).

CPU is the correctness reference. Admitted interactive stages and Bayer RCD ROI
can use the Engine QRhi GPU adapter; remaining stages use the declared hybrid
path. macOS can present an owned IOSurface. Persisted previews, export, CLI PNG
and gold tests use CPU pixels. This is not blanket GPU support on every host.

### Supported media

- JPEG, PNG, supported TIFF/BigTIFF layouts, BMP, GIF and WebP.
- RAW files accepted by the pinned LibRaw provider with validated Bayer 2×2 or
  standard X-Trans 6×6 data. Embedded JPEG availability does not establish full
  RAW decode support.
- Primary HEIC/HEIF photos through native ImageIO on macOS 14+. Windows/Linux
  do not claim that provider.
- MOV/MP4/M4V with H.264, HEVC or standard ProRes. Supported AAC/PCM or no audio
  is admitted; unsupported supplemental tracks can coexist with a supported
  track and remain in the original. Audio-only unsupported combinations reject.

The [format matrix](../userdoc/docs/en/qa/format-coverage.md) distinguishes
candidates from accepted content. Decoder, ICC, transform and audio failures
remain structured. Known codec/container support does not guarantee every
camera recording mode.

### Source safety and persistence

Add references existing files. Copy verifies new destinations. Move verifies
all requested copies and catalogs the primary before source cleanup; cleanup
failures retain the safe copies and report the incomplete operation.
Ordinary editing never rewrites an original.

The SQLite catalog owns recipes, review state, metadata, relationships and
history. `<catalog>.preview/` holds rebuildable previews.
`<catalog>.ravo/sidecars/` holds catalog-owned recovery mirrors; these are
durability artifacts and never an independent edit authority.
Adjacent XMP interchange is explicit and conflict-checked. No automatic
sidecar watcher merges edits behind the user's back.

**Settings → Catalog & Backup** edits a draft for the open catalog. Save failure
preserves the draft; concurrent policy changes require Reload. Switching
catalogs discards the old draft and rejects late folder selections.
Backup destination, interval and retention can be saved while disabled.
Run Now and cancellation use the existing recovery task owner. Panel sizes,
language and Assistant settings use their existing per-user owners.

Back up originals separately. A verified catalog backup includes the catalog
snapshot and recovery records, and excludes original media and preview caches.
Retention removes only reverified, owned scheduled artifacts.

### Conversion and external tools

Studio can import a closed supported Lightroom Classic `.lrcat` into a new empty
Ravo catalog, with a report of converted and omitted fields. Source paths,
ratings, pick/reject flags, labels, hierarchical keywords and catalog XMP text
can be mapped. Compatible database Develop groups, independent virtual copies,
named history/snapshots and static collection membership are converted with
explicit omissions. Source `.lrcat` bytes are preserved in the destination.
Existing vendor catalogs are never opened as
Ravo catalogs or modified in place
([ADR-0164](../DevDocs/adr/0164-lightroom-catalog-reader.md),
[ADR-0166](../DevDocs/adr/0166-lightroom-data-preservation-and-develop.md)).

Supported Camera Raw XMP presets and evidenced historic darktable XMP history
have explicit converters. Unsupported history fails rather than approximating
a look. External-editor registration copies derived outputs under the catalog
support root and records provenance. Optional Assistant HTTP requests use the
configured endpoint and key; they are separate from the local editing engine.
Experimental DNG conversion, suggestion and tethering contracts remain explicitly
unavailable/stubbed where their runtime provider is absent; they are not product
workflow claims.

## Build and test

Prerequisites are CMake 3.26+, a C++20 toolchain, Python, Ninja and a complete Qt
6.11.2 kit with the required image plugins, ShaderTools, Multimedia,
LinguistTools and SQLite runtime. The selected Qt kit also supplies the matching
FFmpeg 7.1.5 shared libraries. Linux requires the documented desktop,
PipeWire and VA-API host libraries. Dependency preparation needs access to the
pinned seed repositories; details are in
[Dependency Workflow](../DevDocs/Dependency_Workflow.md).

Prepare a new checkout from the repository root:

```text
git submodule update --init FreeCM
python3 configs/source_root_workflow.py --init
python3 configs/source_root_workflow.py --update
```

For an already prepared checkout, inspect before changing dependency state:

```text
python3 configs/source_roots.py show --format json
python3 configs/source_roots.py resolve --format json
python3 configs/source_roots.py verify
```

`--init` is the network-enabled preparation action. `--update` materialises the
active lock offline and regenerates host presets. Build/Test/Run do not
implicitly configure or update dependencies. The active lock, generated
presets, managed checkouts and build trees remain local state.

Configure and validate on macOS:

```text
cmake --preset mac_clang_debug -DBUILD_TESTING=ON
cmake --build --preset mac_clang_debug
ctest --test-dir build/mac_clang_debug --output-on-failure --parallel 4
cmake --build --preset mac_clang_debug --target RavoCodeQuality
```

Use `win_msvc_debug|release` on Windows and `linux_clang_debug|release` on
Linux. Each platform needs its own toolchain and validation. FreeCM Config,
Build, Run, Test and Package bind to these same presets; the Python/PowerShell
helpers under `tools/` are optional wrappers.

| Host | Studio executable, relative to repository root |
| --- | --- |
| macOS Debug | `build/mac_clang_debug/Ravo/desktop/ravo_studio.app/Contents/MacOS/ravo_studio` |
| Windows Release | `build/win_msvc_release/Ravo/desktop/ravo_studio.exe` |
| Linux Release | `build/linux_clang_release/Ravo/desktop/ravo_studio` |

Studio accepts `--catalog <library.sqlite>` and `--language <locale>`.
The CLI is beside the corresponding build-tree CLI target at
`build/<preset>/Ravo/cli/ravo` (`.exe` on Windows).
For Release installation and packaging:

```text
cmake --preset mac_clang_release
cmake --build --preset mac_clang_release
cmake --install build/mac_clang_release --prefix install/mac_clang_release
cmake --build build/mac_clang_release --target RavoPackage
```

The same package target produces macOS ARM64/Intel DMGs, Windows x86_64 ZIP,
and Linux x86_64/ARM64 AppImage and DEB on their respective hosts. Tagged CI
publication depends on successful builds and fresh-runner startup of the final
packages. [Packaging](../DevDocs/Packaging.md) owns tool prerequisites,
runtime deployment, output paths and qualification gaps.

## Localisation

The [locale manifest](desktop/i18n/locales.json) is the inventory for all nine
languages. TS files are generated from source and persistent translation
memories; QM files are build output. Use the
[i18n workflow](../.codex/skills/i18n-translation-workflow/SKILL.md):

```text
python3 .codex/skills/i18n-translation-workflow/run_i18n_workflow.py --repo-root . --part 1 --lupdate /path/to/lupdate
# Fill unfinished entries in the locale memory inputs.
python3 .codex/skills/i18n-translation-workflow/run_i18n_workflow.py --repo-root . --part 2
cmake --build --preset mac_clang_debug --target ravo_studio_translations ravo_studio_localization_smoke
```

Incomplete catalogs, malformed memories and placeholder mismatches fail
explicitly. A selected missing language package reports an error.

## Current CLI capabilities

```text
ravo --version --json
ravo operations --json
ravo develop-fields --json
ravo inspect <input> --json
ravo lut inspect <look.cube> --json
ravo noise calibrate <samples.json> --output <profile.json> --json
ravo noise inspect <profile.json> --json
ravo recipe import-xmp <legacy-or-crs.xmp> --asset-id <id> --input <input-uri> --output <recipe> --json
ravo recipe validate <recipe> --json
ravo recipe style-create <recipe> --name <name> --output <style.rstyle.json> --json
ravo recipe style-validate <style.rstyle.json> --json
ravo recipe style-apply <style.rstyle.json> --asset-id <id> --input <input-uri> --output <recipe> --json
ravo recipe style-apply <style.rstyle.json> --target-recipe <current-recipe> --output <recipe> --json
ravo render <input> --recipe <recipe> --output <png> --backend cpu [--width N] [--height N] --json
ravo catalog create --path <library.sqlite> --json
ravo catalog import --catalog <library.sqlite> --input <file-or-folder> \
  [--mode add|copy|move] [--destination <directory>] \
  [--organize single|hierarchy|date|month] \
  [--rename-template '{date}-{sequence}-{stem}{ext}'] \
  [--second-copy <directory>] [--preview minimal|standard|one-to-one] \
  [--no-recursive] [--skip-existing] --json
ravo catalog import-scan --catalog <library.sqlite> --input <file-or-folder> [--no-recursive] --json
ravo catalog video-info --catalog <library.sqlite> --asset-id <id> --json
ravo catalog video-frame --catalog <library.sqlite> --asset-id <id> [--time-us N] [--max-edge N] --output <absent.png> --json
ravo catalog list --catalog <library.sqlite> --json
ravo catalog folders --catalog <library.sqlite> --json
ravo catalog folder-relink --catalog <library.sqlite> --folder-id <id> --replacement <directory> --json
ravo catalog folder-remove --catalog <library.sqlite> --folder-uri <uri> --json
ravo catalog sets --catalog <library.sqlite> --json
ravo catalog set-create --catalog <library.sqlite> --name <name> [--kind manual|smart] [--query <json>] [--asset-id <id>]... [--revision N] --json
ravo catalog list --catalog <library.sqlite> [--set-id <id>] --json
ravo catalog facets --catalog <library.sqlite> \
  [--tag <keyword>] [--set-id <id>] [--camera <text>] [--camera-make <make>] [--camera-model <model>] \
  [--focal-length-mm N] [--captured-local-date YYYY:MM:DD] [--captured-after N] [--captured-before N] \
  [--country <text>] [--province-state <text>] [--city <text>] [--sublocation <text>] [--query <json>] --json
ravo catalog preview --catalog <library.sqlite> --asset-id <id> [--roi x,y,w,h] [--output <file.png>] --json
ravo catalog preview-rebuild --catalog <library.sqlite> [--asset-id <id>]... --json
ravo catalog sidecar-status --catalog <library.sqlite> [--asset-id <id>] --json
ravo catalog sidecar-sync --catalog <library.sqlite> [--asset-id <id>] --json
ravo catalog backup --catalog <library.sqlite> --backup <absent-directory> --json
ravo catalog backup-verify --backup <directory> --json
ravo catalog backup-restore --backup <directory> --output <absent-library.sqlite> --json
ravo catalog backup-policy --catalog <library.sqlite> [--schedule-dir <directory>] [--interval-minutes N] [--retention-count N] [--enabled true|false] --json
ravo catalog backup-run --catalog <library.sqlite> --json
ravo catalog fields --json
ravo catalog probe --catalog <library.sqlite> --asset-id <id> [--baseline] [--set <field>=<number>]... [--set-text <field>=<text>]... [--max-edge N] [--output <file.png>] --json
ravo catalog rate --catalog <library.sqlite> --asset-id <id> --rating 0-5 --json
ravo catalog refresh-metadata --catalog <library.sqlite> --asset-id <id> --json
ravo catalog xmp-status --catalog <library.sqlite> --asset-id <id> [--xmp <path.xmp>] --json
ravo catalog xmp-import --catalog <library.sqlite> --asset-id <id> [--xmp <path.xmp>] [--resolve abort|sidecar|catalog] --json
ravo catalog xmp-export --catalog <library.sqlite> --asset-id <id> [--xmp <path.xmp>] [--resolve abort|catalog|sidecar] --json
ravo catalog editor-open --catalog <library.sqlite> --asset-id <id> --user-initiated \
  [--editor <id>] [--invoke-os-open] [--revision N] --json
ravo catalog editor-register --catalog <library.sqlite> --asset-id <source-id> --input <editor-output> \
  --editor <id> [--editor-version <ver>] [--destination <dir>] [--auto-stack] [--revision N] --json
ravo catalog editor-show --catalog <library.sqlite> --asset-id <derived-id> --json
ravo catalog convert-foreign --catalog <new-empty-library.sqlite> \
  --foreign-source <source.lrcat|fixture-catalog.json> [--source-kind lightroom-classic|capture-one] --json
ravo catalog develop --catalog <library.sqlite> --asset-id <id> [--from-xmp <preset.xmp>] [--set <field>=<number>]... [--set-text <field>=<text>]... [--exposure-ev N] [--watermark-text <text>] --json
ravo catalog develop-apply --catalog <library.sqlite> --from-asset <id> --asset-id <id> [--asset-id <id>]... --fields exposure,temperature [--revision N] --json
ravo catalog recipe --catalog <library.sqlite> --asset-id <id> --json
ravo catalog mask --catalog <library.sqlite> --asset-id <id> --action list --json
ravo catalog companion-check --catalog <library.sqlite> --asset-id <id> [--asset-id <id>]... --json
ravo catalog tag --catalog <library.sqlite> --asset-id <id> [--add <tags>] [--remove <tags>] --json
ravo catalog metadata --catalog <library.sqlite> --asset-id <id> [--title <text>] [--description <text>] [--creator <text>] [--copyright <text>] --json
ravo catalog history --catalog <library.sqlite> --asset-id <id> --json
ravo catalog snapshot --catalog <library.sqlite> --asset-id <id> --label <label> --json
ravo catalog restore --catalog <library.sqlite> --asset-id <id> --history-id <id> --json
ravo catalog export --catalog <library.sqlite> --asset-id <id> --output <file> --format png|jpeg|tiff|tif|original|companion-jpeg [--quality 5..100] [--jpeg-subsampling auto|444|440|422|420] [--jpeg-max-bytes N] \
  [--png-bit-depth 8|16] [--png-compression 0..9] \
  [--tiff-sample-type uint8|uint16|float16|float32] [--tiff-compression none|deflate|deflate_predictor] \
  [--tiff-compression-level 1..9] [--tiff-grayscale-if-neutral] [--tiff-resolution-dpi 72..9600] \
  [--metadata full|no-location|none] --json
ravo catalog export-batch --catalog <library.sqlite> --asset-id <id> [--asset-id <id>]... \
  --output-dir <directory> [--filename-template '{stem}-{sequence}{ext}'] \
  --format png|jpeg|tiff|tif|original [the same typed format/privacy options] --json
ravo studio sessions [--workspace-root <checkout>] --json
ravo studio state [--session-id <id>] [--workspace-root <checkout>] --json
ravo studio video --session-id <id> --asset-id <id> --action play|pause|seek|volume|mute \
  --expect-session-revision N --expect-selection-revision N --expect-video-generation N \
  [--value <JSON-number-or-boolean>] --json
ravo studio develop [--session-id <id>] [--asset-id <id>] \
  [--expect-session-revision N] [--expect-selection-revision N] \
  --set <field>=<number> [--set ...] [--output <file.png>] [--max-edge N] --json
ravo studio preview [--session-id <id>] [--asset-id <id>] \
  [--expect-session-revision N] [--expect-selection-revision N] \
  --output <file.png> [--max-edge N] --json
```

`catalog mask` exposes local-group create/set/rename/duplicate/enable/invert/delete
with an explicit local ID, kind, strict field values and `--expect-revision`.
`studio mask` additionally requires `--expect-session-revision`,
`--expect-selection-revision` and `--expect-recipe-revision`, plus `--action`
and a JSON `--arguments` object. The live state identifies the current editing
scope; gesture actions bind their asset, local ID and token. These paths use
the same versioned masks as Studio rather than a second renderer.

`ravo catalog facets` reports capture and location facet values with their
asset counts. Without filters it counts the whole catalog and reports
`"scoped": false`. Passing any library filter (the same shorthand flags
`catalog list` accepts, or a full `--query` library-query document) restricts
every count to the assets that selection would list, reports `"scoped": true`,
and echoes the applied filters under `scope`. Scoped counts reuse the library
query predicates, so a scope the library refuses to list is refused here with
the same reason (for example `invalid_library_capture_date_facet`), and the
bounded value limit plus `truncated` reporting stay unchanged.

`ravo catalog xmp-status|xmp-import|xmp-export` is the explicit adjacent-XMP
interchange path (ADR-0120/0138). Conflict classes are
`missing|identical|catalog-newer|sidecar-newer|both-changed`; import/export
default to `--resolve abort` and require `catalog` or `sidecar` when the sides
disagree. Catalog fingerprints cover the CRS recipe **and** catalog-owned IPTC
Core, location quartet, ADR-0140 Extension/additional Core writables
(`headline`/`credit`/`source`/`instructions`/`usage_terms`/`job_id`), and
hierarchical keyword display paths. Sidecars may carry CRS PV2012 looks plus
`dc:` / `photoshop:` / `Iptc4xmpCore:` / `xmpRights:` /
`lr:hierarchicalSubject` packets; unsupported hierarchical keyword shapes fail
closed. Original media bytes are never rewritten. Capture refresh still leaves
catalog-owned Core/location/Extension/keywords untouched.

`ravo catalog editor-open|editor-register|editor-show` is the explicit
external-editor round-trip (ADR-0122/0139). `editor-open` requires
`--user-initiated`, records an open-intent under
`{catalog}.ravo/external-editor/open-intents/`, and returns `open_path` /
`open_uri` / `open_kind` (`original` or `derived_working_copy`) without
mutating originals or scripting editors; `--invoke-os-open` is the only CLI
path that may call platform `open`/`xdg-open` after that explicit flag.
`editor-register` copies editor output into `{catalog}.ravo/derived/`, writes
provenance, and optionally `--auto-stack`s source+derived (pick=derived),
fail-closing on stack conflict while retaining the derived asset.

`ravo catalog dng-status` and `ravo catalog dng-convert --catalog
<library.sqlite> --asset-id <id> [--output <path>]` are the ADR-0141 Copy-mode
DNG conversion stubs. Without a Packaging-recorded converter, `dng-convert`
returns `converter_available=false` with reason `dng_converter_unavailable`,
publishes no derived asset, and leaves the source original byte-identical.
`ravo catalog smart-preview --catalog <library.sqlite> --asset-id <id>
[--ensure]` reports browse-only Smart Preview status under
`{catalog}.ravo/smart-previews/`; `develop_fallback` is always false, and
`--ensure` reports `smart_preview_encoder_unavailable` until an encoder is
packaged. ADR-0146 adds offline-edit proxies under `{catalog}.ravo/offline-edit-proxies/` via `catalog offline-proxy-create|list|verify|pin|delete|evict|reconnect` (baked `recipe_baked_srgb8` consume uses `preview_apply_mode=identity_baked` while `media_state=proxy`; export fail-closed with `proxy_export_forbidden` until reconnect).

`ravo catalog ai-suggest --catalog <library.sqlite> --asset-id <id>
--suggestion-kind keyword|caption|focus|duplicate --user-initiated` creates an
ADR-0142 non-authoritative suggestion stub (`deterministic-suggestion-v1`).
`ai-suggestion` / `ai-suggestions` list inspectable confidence and model id;
`ai-suggestion-accept` applies keyword/caption (and caption headline) through
existing tag/metadata APIs; `ai-suggestion-reject` / `ai-suggestion-cancel`
leave the catalog unchanged; focus/duplicate acceptance never deletes peers.


`ravo catalog convert-foreign` is the read-only foreign-catalog conversion
entry point (ADR-0131/0164). It reads a closed Lightroom Classic `.lrcat` SQLite
catalog or a `ravo.foreign-catalog.fixture/v1` document,
never opens or migrates a vendor catalog in place, and writes only into a
`--catalog` the user already created and left empty; a non-empty destination
returns `destination_catalog_not_empty`. Originals are imported in Add mode and
verified byte-identical (SHA-256, size, mtime) after the run; `Move` is
rejected. Close Lightroom before importing: active journals/locks are rejected.
The native reader checks a private snapshot and maps original paths, ratings,
reject flags, standard color labels and hierarchical keywords. Unsupported
schemas and Capture One binaries/directories fail closed. No vendor runtime
is required. Every run returns a structured report counting
`imported`, `skipped`, `unsupported`, and `failed` items with per-item
`mapped_fields`, `unsupported_fields`, and reasons; mapped fields cover the
original plus rating, colour label, reject flag, IPTC Core/location text,
ADR-0140 Extension writables when present in the fixture, keyword paths, and
ADR-0086 CRS recipe fields, and unsupported foreign adjusts are counted rather
than silently dropped.

In Studio, create an empty library, then choose **File → Import Lightroom
Catalog...**. Native Lightroom reports use `ravo.lightroom-catalog-conversion/v1`.
The native path reads database-only Develop settings without requiring XMP
sidecars. Compatible independent groups use the CRS owner; crop/straightening
use Ravo geometry. Unsupported groups, custom labels and missing Adobe profiles
are explicit omissions, with their original content retained in the archive.
Current edits and virtual copies are independent; compatible history and saved
states become named Ravo snapshots. Collections become manual sets with parent
names in their display paths; smart rules are not evaluated and stored members
are static snapshots. Catalog XMP supplies supported descriptive metadata.
Missing originals are skipped; unavailable foreign-volume paths are unsupported
unless explicitly mapped. Photos stay at their original locations. The reader is bounded to 2 GB and one million
photos and validates structure rather than claiming every Lightroom version.
Cancellation retains imported photos; retrying requires a new empty library.

Use repeated `--foreign-map <foreign-directory>=<local-directory>` on
`catalog convert-foreign` for explicit volume or descendant-folder mapping;
the longest segment-prefix wins and an unavailable mapped path has no fallback.
Repeated `--foreign-id <id>` selects an explicit subset (at most 4096 IDs) for
conversion; unknown/duplicate IDs and copies without their selected masters
fail before writes. This selector requires `--expect-source-sha256 <observed-hash>`
from inspection and rejects a changed source before writes. The full source archive is still preserved. Reports state
source/selected photo counts and partial collection membership; omitting the
selector converts the complete source.
Schema 19 retains the original `.lrcat` as hashed 1 MiB chunks, included in
ordinary catalog backup/restore. `catalog foreign-sources --catalog <db> --json`
lists preserved sources; `catalog foreign-source-export --catalog <db>
--source-id <id> --output <new.lrcat> --json` verifies and exports exact bytes.
External `.lrcat-data`, Adobe profiles and preview bundles are not embedded.
Remaining RAWmakase integration gates are tracked in
[TODO_LIGHTROOM_IMPORT.md](../DevDocs/TODO_LIGHTROOM_IMPORT.md).

`catalog inspect-foreign --foreign-source <closed.lrcat> --json` is a read-only
inventory without a destination catalog. It reports photo/copy/edit/history/
snapshot/collection totals, available-original counts, serialized Develop field
and camera-profile frequencies, bounded parse diagnostics and sample identities,
uninterpreted nonempty tables and the companion `.lrcat-data` file/directory.
The contract is `ravo.lightroom-catalog-inspection/v1`. Catalog XMP may be plain
UTF-8 or length-prefixed zlib data; declared output lengths are validated before
decompression. Empty current settings mean no stored edit, not malformed Lua.

An existing output path returns structured `conflict`; it is never overwritten
implicitly. Catalog commands call the same services as Studio and serve as the
headless acceptance client.

`ravo develop-fields` and `ravo catalog fields` list every closed numeric or
text field, kind, and numeric range owned by the strict Develop helpers, plus
the canonical-mask prefixes. They do not require a catalog. `catalog probe` is
a read-only Develop diagnostic. It renders the current recipe, or the
synthesized product baseline with `--baseline`, through the same non-persistent
interactive-preview path as Studio. Repeated `--set name=value` and `--set-text
name=value` overrides accept the advertised fields, reject unknown, duplicate,
non-finite, invalid-resource, or out-of-range values, and return dimensions,
output-profile ID, RGB
sums/means/extrema/clipping counts, and display-luma mean. Optional
`--output <file.png>` writes a throwaway display PNG of that in-memory preview
with atomic no-replace publication; it is not a catalog preview record. The
command reloads the stored recipe and preview-record set after rendering and
fails if either changed. CLI logging remains file-only so machine JSON is the
only stdout content. An open Ravo Studio window also observes another client's
committed catalog revision within one second.

`ravo studio` is the selection-relative control surface. `sessions` discovers
live owner-only endpoints and marks this checkout; `state` returns the selected
asset, current and saved recipes, baseline-relative modified operations,
preview identity, versioned video state, and Settings navigation/backup draft.
Assistant credentials are excluded. `develop` observes or accepts explicit session/selection
revisions, commits one strict ordered `--set` batch through Studio, waits for
save and preview settlement, and can emit the resulting PNG. `preview` renders
the exact current recipe without mutation. Both image commands use the existing
CatalogService/Engine path and return a no-replace caller-owned artifact with
MIME type, dimensions, profile, and SHA-256. Multiple matching windows require
an explicit session ID. Process logs, open files, cache activity, and window
screenshots remain non-authoritative (ADR-0090).

## Source ownership and further reading

| Directory | Owner |
| --- | --- |
| `foundation/` | Errors, values, cancellation and bounded resources |
| `recipe/` | Versioned operations, parameters and recipes |
| `engine/` | Image processing, colour, CPU reference and QRhi adapter |
| `domain/` | Catalog/asset/import/video values and repository ports |
| `services/` | Catalog workflows, orchestration and durable task policy |
| `adapters/` | SQLite, filesystem, codecs and preview/recovery storage |
| `control/` | Bounded same-user local transport and session discovery |
| `cli/` | Supported command-line and JSON client |
| `desktop/` | C++ presenters/commands and Qt Quick views |
| `tests/fixtures/frozen/` | Frozen static reference fixtures |

The repository-root CMake builds only Ravo. The historic GTK application,
dynamic IOP ABI and OpenCL path are not runtime dependencies. Unaccepted
leftover algorithm ports are closed by
[ADR-0106](../DevDocs/adr/0106-close-legacy-algorithm-migration.md).

- [User handbook](../userdoc/README.md)
- [Architecture](../DevDocs/ARCHITECTURE.md)
- [Testing](../DevDocs/TESTING.md)
- [Dependency Workflow](../DevDocs/Dependency_Workflow.md)
- [Packaging](../DevDocs/Packaging.md)
- [Product queue](../DevDocs/TODO.md)
- [Developer documentation index](../DevDocs/README.md)
