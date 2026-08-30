# Ravo Architecture Decision Records

ADRs record important accepted decisions that constrain subsequent
implementation. New ADRs use incrementing four-digit identifiers and include
status, date, context, decision, consequences, and rejected alternatives. Do
not silently rewrite an accepted ADR; record a new ADR and mark the
supersession relationship when direction changes.

| ADR | Status | Decision |
| --- | --- | --- |
| [0001](0001-cpp20-headless-first.md) | Partially superseded by 0007 | C++20 headless engine/CLI first; no Rust in the first version |
| [0002](0002-ravo-consumes-src.md) | Superseded by 0004 | One-way Ravo replacement and eventual removal of legacy `src` ownership |
| [0003](0003-versioned-machine-contract.md) | Accepted | Versioned machine contracts for CLI JSON, recipes, and operation schemas |
| [0004](0004-freeze-09-ravo-only-growth.md) | Partially superseded by 0010 | Freeze 0.9; Ravo is the only growth path. See 0010 for retirement timing. |
| [0005](0005-qtcore-filesystem-adapter.md) | Partially superseded by 0007 | Ravo targets may directly use Qt6::Core when needed; ADR-0007 updates UI sequencing. |
| [0006](0006-explicit-colour-contract.md) | Accepted | Colour state is explicit and versioned at engine boundaries; third-party colour types remain private. |
| [0007](0007-first-usable-catalog-viewer.md) | Accepted | C++ and Qt Quick/QML first deliver an SQLite catalog, image import, and desktop viewer vertical slice. |
| [0008](0008-p0-review-catalog-v2.md) | Accepted | Catalog schema v2 persists P0 rating/color/reject and advances the preview contract. |
| [0009](0009-p1-develop-recipe.md) | Partially superseded by 0016 | Catalog schema v4 has one canonical recipe per image plus tags/metadata/history; ADR-0016 replaces the old lift/gamma/gain path. |
| [0010](0010-incremental-legacy-retirement.md) | Accepted | Incrementally remove accepted Ravo legacy owners through the active migration TODO; remaining leftovers still map to the freeze blob. |
| [0011](0011-atomic-develop-publication.md) | Accepted | Publish recipe/history/revision atomically; Develop preview owns cancellation and late-result rejection by revision. |
| [0012](0012-explicit-channelmixerrgb.md) | Partially superseded by 0017 | `channelmixerrgb` V3 CPU mathematics use an explicit D50 workspace, adaptation, and canonical schema; ADR-0017 finalizes WB ownership. |
| [0013](0013-bayer-hotpixels-preprocess.md) | Accepted | `hotpixels` runs on an owned Bayer CFA copy under the frozen four-neighbor contract and enters the RAW cache key. |
| [0014](0014-bayer-cacorrect.md) | Accepted | `cacorrect` retains the Bayer two-pass tile statistics, polynomial shift fit, and avoid-color-shift path. |
| [0015](0015-migrate-all-non-ui-algorithms.md) | Accepted | Migrate every remaining non-UI algorithm to C++ individually; GTK/Lua/dynamic ABI/OpenCL are ultimately removed rather than ported. |
| [0016](0016-filmlight-colorbalancergb.md) | Accepted | `colorbalancergb` retains Filmlight Yrg three-zone grading, the DT UCS default, and an explicit JzAzBz gamut contract. |
| [0017](0017-explicit-raw-temperature.md) | Accepted | `temperature` owns pre-demosaic as-shot/daylight/manual four-channel scaling; late reference uses explicit CAT only. |
| [0018](0018-explicit-input-color-profiles.md) | Accepted | Input profiles use explicit decode state, matrix/shaper or private ICC transforms, and profile-aware cache keys without sRGB fallback. |
| [0019](0019-explicit-output-color-profiles.md) | Accepted | Output profiles, proofing, gamut warnings, encoded ICC state, and profile-aware publication share one engine boundary without sRGB fallback. |
| [0020](0020-working-profile-rgb-primaries.md) | Accepted | RGB primary rotation, purity, and achromatic tint execute in the declared working profile before compatibility bridging. |
| [0021](0021-explicit-pre-input-profile-gamma.md) | Accepted | Unbreak-input-profile log/gamma correction runs on explicit source RGB before input colour conversion; picker/autotune remains unsupported without an analysis contract. |
| [0022](0022-final-display-packing-and-diagnostic-disposition.md) | Accepted | Final profiled RGB8 packing is engine-private; legacy channel/mask display branches are unsupported presentation adapters. |
| [0023](0023-jpeg-input-adapter-contract.md) | Accepted | JPEG input owns content recognition, EXIF scaling, strict APP2 ICC state, explicit RGB8 opacity, and atomic corrupt-input failure. |
| [0024](0024-exposure-analysis-and-metadata-contract.md) | Accepted | Exposure v2 owns manual/black, private RAW metadata, original-RAW deflicker analysis, and an exact unmasked-only legacy boundary. |
| [0025](0025-legacy-colorbalance-contract.md) | Accepted | Legacy Color Balance owns the full v4 Lab/ProPhoto CPU path, explicit presence, and an exact default-unmasked importer boundary distinct from Color Balance RGB. |
| [0026](0026-colorchecker-calibration-contract.md) | Accepted | Color Checker owns an explicit ordered D50 Lab polynomial/RBF fit, exact frozen solver and presets, and an enabled default-unmasked legacy boundary. |
| [0027](0027-radiance-rgbe-decoder-contract.md) | Accepted | Radiance RGBE uses a dedicated owned float decode contract while Qt raster and Catalog remain explicitly unsupported. |
| [0028](0028-original-copy-publication-contract.md) | Accepted; batch storage extended by 0068 | Original copy uses bounded exact-byte streaming, exclusively owned temporary state, atomic no-replace publication, and complete structured failures without retiring the legacy plugin. |
| [0029](0029-colorcorrection-contract.md) | Accepted | Color Correction preserves explicit five-field affine D50 Lab behavior, strict 0029/0092 unmasked import, and one Develop/CLI/Catalog/Studio contract. |
| [0030](0030-typed-jpeg-export-options.md) | Accepted | JPEG export owns typed quality 5–100/default 95 and five frozen sampling modes through one service-to-adapter value contract without completing metadata or storage publication. |
| [0031](0031-colorcontrast-contract.md) | Accepted | Color Contrast owns a versioned D50 Lab axis-affine CPU contract, deterministic schema-v1 compatibility, and a strict default-unmasked legacy importer boundary. |
| [0032](0032-encoded-byte-publication-contract.md) | Accepted; storage policy extended by 0068 | Encoded bytes use shared private destination primitives for exclusive temporary ownership, synchronized contents, atomic no-replace publication, and stable failure context; 0068 adds typed batch/path policy. |
| [0033](0033-typed-png-export-options.md) | Accepted | PNG export owns typed 8/16-bit depth and compression 0–9/default 5 through bounded private libpng RGB8/RGB16 paths; ADR-0037 supplies RGB16 and ADR-0038/0040 supply the metadata snapshot. |
| [0034](0034-typed-tiff-export-options.md) | Accepted | TIFF export owns typed sample/compression options through a bounded pinned LibTIFF encoder; ADR-0036/0037/0038/0040 add directory metadata, high-precision sources, and bounded packets while multipage masks and retirement remain later I13 work. |
| [0035](0035-colorharmonizer-core-contract.md) | Accepted | Color Harmonizer owns an exact 17-field, profile-aware dt-UCS/RYB smoothing-zero CPU core; ADR-0041 adds the product vertical slice. |
| [0036](0036-tiff-baseline-directory-metadata.md) | Accepted | TIFF export embeds bounded resolution, normalized destination, and current writable baseline main-IFD values before atomic publication; ADR-0038/0040 add packets while multipage masks and retirement remain later I13/S9/J6 work. |
| [0037](0037-high-precision-export-pixel-contract.md) | Accepted | Product export packs engine-owned profiled output into a tagged RGB8/RGB16/float value; PNG16 and TIFF uint16/float16/float32 consume that source without RGB8 expansion. |
| [0038](0038-embedded-export-metadata.md) | Partially superseded by 0040 | Rendered JPEG/PNG/TIFF exports embed one Catalog-owned public Exif/XMP/IPTC snapshot before ADR-0032 publication; ADR-0040 extends that snapshot with capture time/offset/GPS. |
| [0039](0039-explicit-export-option-controls.md) | Accepted | CLI and Studio expose the existing typed JPEG/PNG/TIFF export options through one explicit format/options intent; localized-filter inference and remembered codec settings are rejected. |
| [0040](0040-capture-time-gps-metadata.md) | Accepted | Catalog schema v5 persists typed capture datetime/offset/GPS and rendered JPEG/PNG/TIFF preserve the validated snapshot. |
| [0041](0041-colorharmonizer-smoothing-zero-vertical-slice.md) | Accepted; smoothing limitation extended by 0042 | Strict v1 singleton import and the initial Develop/CLI/Catalog/Studio surface; 0042 adds canonical positive smoothing while masks, GPU, and retirement remain later C14 work. |
| [0042](0042-colorharmonizer-canonical-roi-recursive-smoothing.md) | Accepted | Canonical ROI scale and private S2.2 reproduce C14 positive smoothing across the supported recipe/engine consumers while strict legacy import stays zero-evidence only. |
| [0043](0043-canonical-mask-graph-foundation.md) | Accepted | S3.1 owns a typed immutable canonical mask DAG, private ROI evaluator, normal mix, and two supported operation consumers without completing M1/C14 retirement. |
| [0044](0044-studio-canonical-mask-authoring.md) | Accepted | Studio authors bounded owned Color Harmonizer/Graduated ND canonical leaves through strict recipe helpers and read-only presenter maps; overlay, group/path/brush, M1, and C14 retirement remain later work. |
| [0045](0045-studio-mask-overlay-group-path.md) | Accepted | Studio overlay, owned group editor, and path/brush complete the P0 mask surface; Color Harmonizer's frozen IOP retires while leftover GTK mask-manager consumers remain. |
| [0046](0046-catalog-raster-raw-import-routing.md) | Accepted | Catalog fully decodes JPEG/PNG/TIFF before publication and only TIFF RAW containers may fall through to LibRaw. |
| [0047](0047-first-frame-raw-cache-lifecycle.md) | Accepted | First-frame Bayer LibRaw/DNG decode, structured RAW errors, corrupt PNG cache miss, and close/reopen preview; leftover wrappers and full demosaic stay later. |
| [0048](0048-legacy-flip-orientation-contract.md) | Accepted | Leftover flip v2 orientation bits map to canonical rotate-then-flip; NULL/NONE stay identity because EXIF is applied at decode. |
| [0049](0049-legacy-crop-box-contract.md) | Accepted | Leftover crop v1–v3 left/top/right/bottom maps to canonical x/y/width/height; full-frame 0,0,1,1 is identity. |
| [0050](0050-ashift-rotation-and-export-scale.md) | Accepted | Leftover ashift rotation-only maps to straighten; Catalog export `max_edge` owns final scale. Perspective ashift stays later G6. |
| [0051](0051-legacy-rgblevels-contract.md) | Accepted | Leftover rgblevels v1 maps to `ravo.color.rgblevels` with leftover LUT/clip math; auto-levels picker stays history-baked. |
| [0052](0052-legacy-rgbcurve-contract.md) | Accepted; extended by 0053 | Leftover rgbcurve v1 monotone-hermite maps to `ravo.color.rgbcurve`. Middle-grey compensation is 0053. |
| [0053](0053-rgbcurve-middle-grey-uncompensate.md) | Accepted | RGB curve `compensate_middle_grey` remaps nodes through the live working D50 matrix; `0060` imports. |
| [0054](0054-legacy-rawdenoise-contract.md) | Accepted | Leftover Bayer rawdenoise v2 maps to `ravo.raw.denoise` wavelet/threshold; X-Trans stays later. |
| [0055](0055-colorreconstruction-bilateral-grid-contract.md) | Accepted | Color Reconstruction owns the full-frame D50 Lab bilateral grid, canonical spatial scale, strict 0052 import, and one Develop/CLI/Catalog/Studio path. |
| [0056](0056-source-exact-lab-sharpen.md) | Accepted | Sharpen schema v2 replaces the approximation with the frozen scale-aware separable D50 Lab L* USM and strict three-record import. |
| [0057](0057-source-linear-dark-channel-dehaze.md) | Accepted | Dehaze schema v2 replaces the shortcut with source-linear ambient/depth estimation and bounded RGB guided-filter transmission. |
| [0058](0058-ordered-canonical-retouch.md) | Accepted | Retouch owns ordered canonical-mask clone/heal/blur/fill regions, source geometry, wavelet scales, and strict frozen payload import. |
| [0059](0059-library-query-filter-contract.md) | Accepted | LibraryQuery strictly owns supported filters and stable sorting; recent-filter history and legacy-only fields are explicitly unsupported. |
| [0060](0060-studio-navigation-lifecycle.md) | Accepted | Studio owns bounded Fit/Fill/Actual/custom zoom, pan/navigator geometry, and asset/mode/zoom viewport reset timing. |
| [0061](0061-engine-owned-preview-scopes.md) | Accepted | Engine owns Histogram, Waveform, Parade, fixed D50 u*v* Vectorscope, and Split pixels for the displayed preview. |
| [0062](0062-asset-mutation-removal-transaction.md) | Accepted | Asset removal deletes row+revision transactionally; disk deletion uses adjacent quarantine and restores the original on database failure. |
| [0063](0063-explicit-no-automatic-sidecar-policy.md) | Accepted | Catalog edits never automatically read/write sidecars; legacy XMP import is explicit and export XMP is newly embedded. |
| [0064](0064-atomic-metadata-refresh-and-export-privacy.md) | Accepted | Capture metadata refresh publishes atomically; JPEG/PNG/TIFF share full/no-location/none privacy modes while original-copy stays exact. |
| [0065](0065-versioned-recipe-style-artifact.md) | Accepted; legacy resource cleanup extended by 0072 | `.rstyle.json` is a complete canonical Recipe template; CLI/Studio create, validate, and apply it while legacy dtstyle rejects. |
| [0066](0066-typed-desktop-language-setting.md) | Accepted; assistant settings added by 0081 | Typed desktop language; 0081 adds assistant endpoint/model/key. Corrupt/write failures stay explicit and no old configuration keys migrate. |
| [0067](0067-bounded-preview-cache-lru.md) | Accepted | Preview PNGs use a 512 MiB hard byte budget with deterministic persistent LRU, atomic commits, explicit I/O errors, and cancellation before publication. |
| [0068](0068-typed-batch-export-storage.md) | Accepted | Batch export preflights strict portable filename templates and preserves per-item atomic no-replace publication with explicit partial-delivery errors. |
| [0069](0069-post-output-dither-posterize.md) | Accepted | All frozen Dither/Posterize methods run after Output Color and before target-aware packing with deterministic serial TEA random state. |
| [0070](0070-canvas-and-output-frame-contract.md) | Accepted | Canvas grows linear working pixels while preserving the original mask content frame; final Frame reproduces frozen encoded-output borders after optional Dither. |
| [0071](0071-deterministic-text-watermark.md) | Accepted | Watermark uses a versioned built-in text raster instead of external SVG lookup, system fonts, or mutable metadata variables. |
| [0072](0072-retire-legacy-example-styles.md) | Accepted | Bundled `.dtstyle` examples and their exclusive generator are removed because the format is an all-or-nothing unsupported contract. |
| [0073](0073-color-zones-lab-curve-contract.md) | Accepted | Optional Color Zones owns three source-quantized D50 Lab curves, all frozen interpolation modes, canonical masks, and strict v5 import. |
| [0074](0074-monochrome-lab-filter-contract.md) | Accepted | Monochrome replaces the chroma shortcut with the frozen Lab colour filter, shared bilateral base, highlight envelope, masks, and strict v2 import. |
| [0075](0075-split-toning-hsl-contract.md) | Accepted | Split Toning replaces fixed saturation/compression with the full frozen shadow/highlight HSL pivot, masks, and strict v1 import. |
| [0076](0076-photo-inspect-toggle-actual-size.md) | Accepted | Inspect photo click toggles Actual 1:1 and restores the last Fit/Fill/custom view; QML owns the magnifier pointer, click pan, and GPU scale animation. |
| [0077](0077-compact-library-filter-bar.md) | Accepted | Filter checkbox reveals a default rating-star strip; other library predicates are added and removed as session chips. |
| [0078](0078-copy-paste-develop-edits.md) | Accepted | Copy/Paste Edits use a session clipboard of complete DevelopParams and commit through ordinary history/undo. |
| [0079](0079-develop-set-inventory-and-probe-png.md) | Accepted; Studio observation added by 0080 | Recipe owns the CLI `--set` field inventory; `catalog probe --output` writes a throwaway PNG without mutating recipe or preview records. |
| [0080](0080-studio-observes-catalog-revision.md) | Accepted | Studio polls live catalog revision on the existing snapshot contract; MCP around `ravo` is not the control plane. |
| [0081](0081-studio-assistant-endpoint-panel.md) | Partially superseded by 0090 | Typed assistant URL/model/key settings and a floating non-modal Studio panel; assistant HTTP and credentials remain desktop-only. |
| [0082](0082-studio-develop-grading-workspace.md) | Accepted | Studio default Develop order is a grading stack; Color EQ is separate from Graduated ND; Color Balance RGB uses wheels; paste can apply Light or Color from a complete clipboard. |
| [0083](0083-color-eq-bands-and-white-balance-pick.md) | Accepted | Eight-band Color Equalizer editor; RAW inspect reports WB coefficients; Bayer CFA pick writes manual temperature coefficients. |
| [0084](0084-studio-grading-curves.md) | Accepted | First-class Curves section authors RGB and Tone operations; interpolators, histogram, and parametric regions. |
| [0085](0085-interchange-ready-grading-tools.md) | Accepted; response mapping partially superseded by 0088 | Vignette geometry, Camera Calibration on the grading path, HSL band names, and Detail NR before any Lightroom CRS adapter. |
| [0086](0086-lightroom-crs-interchange.md) | Accepted; response mapping partially superseded by 0088 | Fail-closed Camera Raw XMP import/apply onto accepted Develop owners; leftover empty-history swallow is rejected. |
| [0087](0087-progressive-develop-preview.md) | Accepted | Develop publishes an exact 960px memory preview before an exact persisted 1600px result, with foreground/background cache lanes, foreground scheduling, latency-first cache PNG, and deterministic CPU row partitions. |
| [0088](0088-lightroom-response-calibration.md) | Accepted | Lightroom-relative exposure, RAW sigmoid contrast, scene-EV highlights/shadows, and post-sigmoid display-sRGB point-curve calibration. |
| [0089](0089-exact-interactive-prefix-cache.md) | Accepted | Foreground 960px previews retain an exact pre-light RGB prefix and owner-scoped row team, with successful-only publication and a 30ms Release P90 intent-to-image gate. |
| [0090](0090-versioned-live-studio-control.md) | Accepted | Same-user, revision-checked live Studio selection/recipe control through CLI, with exact no-replace preview artifacts. |
