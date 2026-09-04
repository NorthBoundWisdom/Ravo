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
| [0010](0010-incremental-legacy-retirement.md) | Partially superseded by 0106 | Incrementally remove accepted Ravo leftover owners; remaining leftover C left with leftover-tree deletion rather than leftover-faithful ports. |
| [0011](0011-atomic-develop-publication.md) | Accepted | Publish recipe/history/revision atomically; Develop preview owns cancellation and late-result rejection by revision. |
| [0012](0012-explicit-channelmixerrgb.md) | Partially superseded by 0017 | `channelmixerrgb` V3 CPU mathematics use an explicit D50 workspace, adaptation, and canonical schema; ADR-0017 finalizes WB ownership. |
| [0013](0013-bayer-hotpixels-preprocess.md) | Accepted | `hotpixels` runs on an owned Bayer CFA copy under the frozen four-neighbor contract and enters the RAW cache key. |
| [0014](0014-bayer-cacorrect.md) | Accepted | `cacorrect` retains the Bayer two-pass tile statistics, polynomial shift fit, and avoid-color-shift path. |
| [0015](0015-migrate-all-non-ui-algorithms.md) | Partially superseded by 0106 | Remaining leftover algorithms were in-scope C++ ports; ADR-0106 makes unaccepted leftover IOPs leftovers rather than ports. |
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
| [0039](0039-explicit-export-option-controls.md) | Accepted; long-edge extended by 0113 | CLI and Studio expose the existing typed JPEG/PNG/TIFF export options through one explicit format/options intent; localized-filter inference and remembered codec settings are rejected. |
| [0040](0040-capture-time-gps-metadata.md) | Accepted | Catalog schema v5 persists typed capture datetime/offset/GPS and rendered JPEG/PNG/TIFF preserve the validated snapshot. |
| [0041](0041-colorharmonizer-smoothing-zero-vertical-slice.md) | Accepted; smoothing limitation extended by 0042 | Strict v1 singleton import and the initial Develop/CLI/Catalog/Studio surface; 0042 adds canonical positive smoothing while masks, GPU, and retirement remain later C14 work. |
| [0042](0042-colorharmonizer-canonical-roi-recursive-smoothing.md) | Accepted | Canonical ROI scale and private S2.2 reproduce C14 positive smoothing across the supported recipe/engine consumers while strict legacy import stays zero-evidence only. |
| [0043](0043-canonical-mask-graph-foundation.md) | Accepted; consumers extended by 0108–0112 | S3.1 owns a typed immutable canonical mask DAG, private ROI evaluator, normal mix, and named `supports_mask` operation consumers without completing M1/C14 retirement. |
| [0044](0044-studio-canonical-mask-authoring.md) | Accepted; click placement extended by 0114 | Studio authors bounded owned Color Harmonizer/Graduated ND canonical leaves through strict recipe helpers and read-only presenter maps; overlay, group/path/brush, M1, and C14 retirement remain later work. |
| [0045](0045-studio-mask-overlay-group-path.md) | Accepted; click placement extended by 0114 | Studio overlay, owned group editor, and path/brush complete the P0 mask surface; Color Harmonizer's frozen IOP retires while leftover GTK mask-manager consumers remain. |
| [0046](0046-catalog-raster-raw-import-routing.md) | Accepted | Catalog fully decodes JPEG/PNG/TIFF before publication and only TIFF RAW containers may fall through to LibRaw. |
| [0047](0047-first-frame-raw-cache-lifecycle.md) | Accepted; demosaic scope extended by 0096 | First-frame Bayer LibRaw/DNG lifecycle; ADR-0096 adds bounded DNG corrections, Bayer RCD/PPG and X-Trans Markesteijn. |
| [0048](0048-legacy-flip-orientation-contract.md) | Accepted | Leftover flip v2 orientation bits map to canonical rotate-then-flip; NULL/NONE stay identity because EXIF is applied at decode. |
| [0049](0049-legacy-crop-box-contract.md) | Accepted | Leftover crop v1–v3 left/top/right/bottom maps to canonical x/y/width/height; full-frame 0,0,1,1 is identity. |
| [0050](0050-ashift-rotation-and-export-scale.md) | Accepted; generalized by 0096; Studio long-edge extended by 0113 | Original rotation-only ashift boundary and Catalog export `max_edge`; ADR-0096 adds canonical generic Perspective. |
| [0051](0051-legacy-rgblevels-contract.md) | Accepted | Leftover rgblevels v1 maps to `ravo.color.rgblevels` with leftover LUT/clip math; auto-levels picker stays history-baked. |
| [0052](0052-legacy-rgbcurve-contract.md) | Accepted; extended by 0053 | Leftover rgbcurve v1 monotone-hermite maps to `ravo.color.rgbcurve`. Middle-grey compensation is 0053. |
| [0053](0053-rgbcurve-middle-grey-uncompensate.md) | Accepted | RGB curve `compensate_middle_grey` remaps nodes through the live working D50 matrix; `0060` imports. |
| [0054](0054-legacy-rawdenoise-contract.md) | Accepted; X-Trans extended by 0096 | Leftover Bayer rawdenoise v2 maps to `ravo.raw.denoise` wavelet/threshold; ADR-0096 adds the separate X-Trans path. |
| [0055](0055-colorreconstruction-bilateral-grid-contract.md) | Accepted | Color Reconstruction owns the full-frame D50 Lab bilateral grid, canonical spatial scale, strict 0052 import, and one Develop/CLI/Catalog/Studio path. |
| [0056](0056-source-exact-lab-sharpen.md) | Accepted | Sharpen schema v2 replaces the approximation with the frozen scale-aware separable D50 Lab L* USM and strict three-record import. |
| [0057](0057-source-linear-dark-channel-dehaze.md) | Accepted | Dehaze schema v2 replaces the shortcut with source-linear ambient/depth estimation and bounded RGB guided-filter transmission. |
| [0058](0058-ordered-canonical-retouch.md) | Accepted | Retouch owns ordered canonical-mask clone/heal/blur/fill regions, source geometry, wavelet scales, and strict frozen payload import. |
| [0059](0059-library-query-filter-contract.md) | Accepted | LibraryQuery strictly owns supported filters and stable sorting; recent-filter history and legacy-only fields are explicitly unsupported. |
| [0060](0060-studio-navigation-lifecycle.md) | Accepted | Studio owns bounded Fit/Fill/Actual/custom zoom, pan/navigator geometry, and asset/mode/zoom viewport reset timing. |
| [0061](0061-engine-owned-preview-scopes.md) | Accepted | Engine owns Histogram, Waveform, Parade, fixed D50 u*v* Vectorscope, and Split pixels for the displayed preview. |
| [0062](0062-asset-mutation-removal-transaction.md) | Accepted | Asset removal deletes row+revision transactionally; disk deletion uses adjacent quarantine and restores the original on database failure. |
| [0063](0063-explicit-no-automatic-sidecar-policy.md) | Partially superseded by 0097 | Catalog edits never read/write adjacent interchange sidecars; 0097 adds catalog-owned recovery mirrors. |
| [0064](0064-atomic-metadata-refresh-and-export-privacy.md) | Accepted | Capture metadata refresh publishes atomically; JPEG/PNG/TIFF share full/no-location/none privacy modes while original-copy stays exact. |
| [0065](0065-versioned-recipe-style-artifact.md) | Accepted; legacy resource cleanup extended by 0072 and selective overlays by 0097 | `.rstyle.json` schema v1 is a complete canonical Recipe template; CLI/Studio create, validate, and apply it while legacy dtstyle rejects. |
| [0066](0066-typed-desktop-language-setting.md) | Accepted; assistant settings added by 0081; window geometry added by 0115 | Typed desktop language; 0081 adds assistant endpoint/model/key; 0115 adds window size/position. Corrupt/write failures stay explicit and no old configuration keys migrate. |
| [0067](0067-bounded-preview-cache-lru.md) | Accepted | Preview PNGs use a 512 MiB hard byte budget with deterministic persistent LRU, atomic commits, explicit I/O errors, and cancellation before publication. |
| [0068](0068-typed-batch-export-storage.md) | Accepted; Studio long-edge extended by 0113 | Batch export preflights strict portable filename templates and preserves per-item atomic no-replace publication with explicit partial-delivery errors. |
| [0069](0069-post-output-dither-posterize.md) | Accepted | All frozen Dither/Posterize methods run after Output Color and before target-aware packing with deterministic serial TEA random state. |
| [0070](0070-canvas-and-output-frame-contract.md) | Accepted | Canvas grows linear working pixels while preserving the original mask content frame; final Frame reproduces frozen encoded-output borders after optional Dither. |
| [0071](0071-deterministic-text-watermark.md) | Accepted | Watermark uses a versioned built-in text raster instead of external SVG lookup, system fonts, or mutable metadata variables. |
| [0072](0072-retire-legacy-example-styles.md) | Accepted | Bundled `.dtstyle` examples and their exclusive generator are removed because the format is an all-or-nothing unsupported contract. |
| [0073](0073-color-zones-lab-curve-contract.md) | Accepted | Optional Color Zones owns three source-quantized D50 Lab curves, all frozen interpolation modes, canonical masks, and strict v5 import. |
| [0074](0074-monochrome-lab-filter-contract.md) | Accepted | Monochrome replaces the chroma shortcut with the frozen Lab colour filter, shared bilateral base, highlight envelope, masks, and strict v2 import. |
| [0075](0075-split-toning-hsl-contract.md) | Accepted | Split Toning replaces fixed saturation/compression with the full frozen shadow/highlight HSL pivot, masks, and strict v1 import. |
| [0076](0076-photo-inspect-toggle-actual-size.md) | Accepted | Inspect photo click toggles Actual 1:1 and restores the last Fit/Fill/custom view; QML owns the magnifier pointer, click pan, and GPU scale animation. |
| [0077](0077-compact-library-filter-bar.md) | Accepted | Filter checkbox reveals a default rating-star strip; other library predicates are added and removed as session chips. |
| [0078](0078-copy-paste-develop-edits.md) | Accepted; selective field owner extended by 0098/0107 | Copy/Paste Parameters use an explicit-field session clipboard and commit through ordinary history/undo. |
| [0079](0079-develop-set-inventory-and-probe-png.md) | Accepted; Studio observation added by 0080 | Recipe owns the CLI `--set` field inventory; `catalog probe --output` writes a throwaway PNG without mutating recipe or preview records. |
| [0080](0080-studio-observes-catalog-revision.md) | Accepted | Studio polls live catalog revision on the existing snapshot contract; MCP around `ravo` is not the control plane. |
| [0081](0081-studio-assistant-endpoint-panel.md) | Partially superseded by 0090 | Typed assistant URL/model/key settings and a floating non-modal Studio panel; assistant HTTP and credentials remain desktop-only. |
| [0082](0082-studio-develop-grading-workspace.md) | Accepted; clipboard groups superseded by 0097 | Studio default Develop order is a grading stack; Color EQ is separate from Graduated ND and Color Balance RGB uses wheels. |
| [0083](0083-color-eq-bands-and-white-balance-pick.md) | Accepted | Eight-band Color Equalizer editor; RAW inspect reports WB coefficients; Bayer CFA pick writes manual temperature coefficients. |
| [0084](0084-studio-grading-curves.md) | Accepted | First-class Curves section authors RGB and Tone operations; interpolators, histogram, and parametric regions. |
| [0085](0085-interchange-ready-grading-tools.md) | Accepted; response mapping partially superseded by 0088 | Vignette geometry, Camera Calibration on the grading path, HSL band names, and Detail NR before any Lightroom CRS adapter. |
| [0086](0086-lightroom-crs-interchange.md) | Accepted; response mapping partially superseded by 0088 | Fail-closed Camera Raw XMP import/apply onto accepted Develop owners; leftover empty-history swallow is rejected. |
| [0087](0087-progressive-develop-preview.md) | Accepted | Develop publishes an exact 960px memory preview before an exact persisted 1600px result, with foreground/background cache lanes, foreground scheduling, latency-first cache PNG, and deterministic CPU row partitions. |
| [0088](0088-lightroom-response-calibration.md) | Accepted | Lightroom-relative exposure, RAW sigmoid contrast, scene-EV highlights/shadows, and post-sigmoid display-sRGB point-curve calibration. |
| [0089](0089-exact-interactive-prefix-cache.md) | Accepted | Foreground 960px previews retain an exact pre-light RGB prefix and owner-scoped row team, with successful-only publication and a 30ms Release P90 intent-to-image gate. |
| [0090](0090-versioned-live-studio-control.md) | Accepted | Same-user, revision-checked live Studio selection/recipe control through CLI, with exact no-replace preview artifacts. |
| [0091](0091-monotonic-whites-blacks-response.md) | Accepted | Whites/Blacks use monotonic scene-EV luminance envelopes and a shared positive RGB scale; preview v8 retires subtractive black clipping. |
| [0092](0092-edge-preserving-tone-equalizer.md) | Accepted | Five controls drive the full nine-band Tone Equalizer through a normalized EV RBF and scale-stable log-guided mask; preview v9 retires sparse-curve halos and dark-detail loss. |
| [0093](0093-versioned-studio-locale-catalog.md) | Accepted | One embedded manifest owns Studio locales, aliases, catalogs, and memories; packaging and smoke require the complete set while machine contracts stay English. |
| [0094](0094-adaptive-profile-denoise.md) | Accepted | Profile Denoise calibrates Y0U0V0 wavelet noise from bounded MAD samples, gives Radius defined spatial behavior, separates luminance/chroma mixing, and invalidates fixed-profile pixels with preview v10. |
| [0095](0095-velvia-weighted-saturation-contract.md) | Accepted | Velvia owns the frozen low-saturation/midtone-weighted linear-RGB boost, typed strength/bias, canonical masks, strict 0063 import, and complete product persistence. |
| [0096](0096-reference-algorithm-assimilation-boundary.md) | Accepted | External research enters only through bounded Ravo owners with exact provenance and CPU-first gates; Texture is accepted, while Local Laplacian and the over-budget Filmulator model are rejected from production. |
| [0097](0097-catalog-recovery-sidecars-and-verifiable-backups.md) | Accepted; snapshot/restore extended by 0099 | Schema v6 owns retryable per-asset recovery generations and verified catalog backups that exclude originals and previews. |
| [0098](0098-selective-develop-presets.md) | Accepted | One stable Develop-field selection/merge owner serves `.rstyle.json` schema v2 and Studio Copy/Paste Parameters; both start with no fields selected. |
| [0099](0099-atomic-catalog-restore-and-operator-recovery.md) | Accepted | Verified backups restore only to absent destinations through staged support-first/catalog-last publication; cancellable chunked SQLite copy replaces `VACUUM INTO`, and CLI/Studio share recovery and preview-rebuild operations. |
| [0100](0100-paged-library-and-foreground-work-scheduling.md) | Accepted | Versioned keyset pages, a three-page sparse Studio model, one-item deterministic import dispatch, and the existing foreground lane bound 10,000-photo work without a second scheduler. |
| [0101](0101-verified-backup-scheduling-and-stable-folder-relink.md) | Accepted | Schema v8 schedules verified retention without deleting unknown paths; schema v9 gives direct containing folders stable IDs and an identity-checked transactional relink. |
| [0102](0102-planned-managed-import-workspace.md) | Accepted | One import workspace plans Add/Copy/Move, no-replace destinations, XMP and JPEG companions, and background preview policies. |
| [0103](0103-named-library-sets.md) | Accepted | Schema v10 persists manual membership collections and smart `LibraryQuery` sets with revision-checked mutations and paged listing. |
| [0104](0104-bounded-rename-and-verified-second-copy-ingest.md) | Accepted | Copy/Move ingest adds a bounded portable rename grammar and an independently byte-verified second-copy tree before catalog publication. |
| [0105](0105-asset-versions-stacks-and-survey.md) | Accepted | Schema v11 adds virtual copies of one original, collapsed stacks with a pick, and a Survey browse mode for N-up culling. Same-stem RAW+JPEG import is one RAW asset. |
| [0106](0106-close-legacy-algorithm-migration.md) | Accepted | Unaccepted leftover image algorithms are leftovers, not C++ ports; leftover tree retirement is a single deletion, not a leftover-faithful IOP queue. |
| [0107](0107-apply-develop-selection.md) | Accepted | CatalogService applies a chosen Develop field set to an explicit multi-selection with catalog-revision preflight, per-item partial failure, and cancellation that keeps completed photos. |
| [0108](0108-masked-color-balance-rgb.md) | Accepted | Color Balance RGB may carry one owned canonical mask; Studio authors it through the existing MaskEditor; extra blend modes and multi-instance remain out of scope. |
| [0109](0109-masked-exposure.md) | Accepted | Exposure may carry one owned canonical mask; Studio authors it through the existing MaskEditor; Highlights/Shadows/Curves remain global. |
| [0110](0110-masked-rgb-curve.md) | Accepted | RGB Curve may carry one owned canonical mask; Studio authors it through the existing MaskEditor; Tone Curve remains global. |
| [0111](0111-masked-tone-curve.md) | Accepted | Tone Curve may carry one owned canonical mask; Studio authors it through the existing MaskEditor; Highlights/Shadows remain global. |
| [0112](0112-masked-light-controls.md) | Accepted | Highlights, Shadows, Whites, and Blacks may each carry one owned canonical mask; unmasked neighbours stay fused; Contrast remains global. |
| [0113](0113-studio-export-long-edge.md) | Accepted | Studio export long-edge projects Catalog `max_edge`; 0 keeps rendered size and the fit never enlarges; original copy rejects resize. |
| [0114](0114-mask-click-placement.md) | Accepted | Clicking the Develop photo places circle/ellipse centers or gradient anchors through existing mask fields; Canvas/Perspective/rotate/flip reject. |
| [0115](0115-typed-studio-window-geometry.md) | Accepted | Typed Studio window size, position, and maximized state restore across launches; corrupt records repair and off-screen rectangles are fitted. |
| [0116](0116-histogram-assisted-parametric-mask.md) | Accepted | C++-owned histogram-assisted parametric thresholds from a Develop photo pick for authorized everyday consumers; Canvas/Perspective reject. |
| [0117](0117-export-box-sharpen-presets-and-restartable-jobs.md) | Accepted | Export box fit, post-resize output sharpen, reusable ExportOptions presets, and restartable batch jobs on CatalogService. |
| [0118](0118-heic-heif-fail-closed-ingest.md) | Accepted | HEIC/HEIF containers are recognized by ftyp brands and fail closed until an owned decoder ships; never pretend-JPEG or incidental ImageIO decode. |
| [0119](0119-hierarchical-keywords.md) | Accepted | Catalog-owned hierarchical keywords with stable IDs, schema v12 membership, and export privacy via existing tag packets. |
| [0120](0120-xmp-interchange-conflict-matrix.md) | Accepted | Adjacent XMP conflict matrix; explicit CRS import/export |
| [0121](0121-ai-architecture-privacy-provenance.md) | Accepted | AI architecture, privacy, provenance, licence boundary |
| [0122](0122-external-editor-derived-assets.md) | Accepted | External-editor output as derived asset with provenance |
| [0123](0123-heic-owned-decode-packaging-gate.md) | Accepted | HEIC owned-decode blocked on licence/package evidence |
| [0124](0124-iptc-core-catalog-subset.md) | Accepted | Catalog-owned IPTC Core quartet (title/description/creator/copyright), refresh isolation, export privacy, multi-select patches |
| [0125](0125-ptp-mtp-ingest-transport.md) | Accepted | Ingest transport URI + disconnect/cancel; filesystem-card first adapter (PTP USB residual) |
| [0126](0126-catalog-owned-location-fields.md) | Accepted | Catalog-owned IPTC location quartet (country/province_state/city/sublocation), schema v13, export no-location strips GPS+labels |
| [0127](0127-export-delivery-text-watermark.md) | Accepted | Export delivery text watermark reuses ADR-0071 via ExportOptions; no recipe mutation |
| [0128](0128-capture-metadata-library-facets.md) | Accepted | Camera/lens(focal)/capture-date facets over existing capture metadata; exact LibraryQuery selectors; bounded enumeration |
| [0129](0129-export-delivery-colour-and-frame.md) | Accepted | Export delivery colour override + ADR-0070 frame via ExportOptions; order after sharpen, before watermark; no recipe mutation |
| [0130](0130-catalog-location-library-filters.md) | Accepted | Exact LibraryQuery location selectors + bounded location facets over ADR-0126 writable columns; Studio/CLI chips |
| [0131](0131-foreign-catalog-conversion.md) | Accepted | Read-only Lightroom/Capture One → new Ravo catalog conversion; no in-place open; fixture-first Ready tranche residual |
| [0132](0132-viewport-roi-full-resolution-inspect.md) | Accepted | Actual Size 1:1 is a CPU CFA window of the visible crop; lens/perspective and full-frame ROIs reject; GPU display stays deferred. |
| [0133](0133-engine-gpu-preview-adapter.md) | Accepted; backend host extended by 0134 | GPU is an Engine adapter with no silent CPU fallback; first tranche owns device lifecycle, then RGB apply, then gold-gated demosaic. |
| [0134](0134-engine-qrhi-gpu-backend.md) | Accepted | One Engine QRhi compute backend replaces per-platform GPU kernels; `ravo_engine` may link Qt Gui for that adapter only. |
| [0135](0135-exif-lens-name-library-facet.md) | Accepted | Exif LensMake/LensModel capture columns + lens-name facet; keep focal-length proxy; schema v14 |
| [0136](0136-derived-tree-backup-restore.md) | Accepted | Verified backup/restore packages `{catalog}.ravo/derived/` + `external-editor/` (format v2); originals still excluded |
| [0137](0137-shoot-consistency-batch-proposals.md) | Accepted | AI-03 shoot-consistency: one proposal per destination; stub copies WB/exposure/tone/colour from reference |
| [0138](0138-xmp-adjacent-keyword-iptc-location-merge.md) | Accepted | Adjacent XMP keyword/IPTC Core/location merge under ADR-0120 conflict matrix |
| [0139](0139-external-editor-os-open-and-derived-stack.md) | Accepted | External-editor OS open-with payload + derived-pair auto-stack on register |
| [0140](0140-iptc-extension-core-subset.md) | Accepted | Bounded IPTC Extension / additional Core writables (schema v15) |
| [0141](0141-dng-conversion-smart-preview-policy.md) | Accepted | DNG Copy-mode side conversion + Smart Preview browse-only; fail-closed without packaged converter |
| [0142](0142-ai-metadata-culling-suggestions.md) | Accepted | AI-04 stub keyword/caption/focus/duplicate suggestions; explicit accept; never auto-reject/delete |
| [0143](0143-crs-process-version-matrix.md) | Accepted | CRS ProcessVersion supported vs fail-closed matrix; xmp-status reports version class |
| [0144](0144-per-display-icc-presentation.md) | Accepted | Per-display ICC is presentation-only; C++ owns preview→monitor; sRGB fallback; synthetic test profiles |
| [0145](0145-multi-instance-local-adjustments.md) | Accepted | Multi-instance Exposure/Color Balance RGB; name/bypass/reorder; mask leaves reuse ADR-0043/0116; XMP fail-closed |
| [0146](0146-offline-edit-proxy.md) | Accepted | Offline-edit proxy class distinct from browse-only Smart Preview; export fail-closed without original |
| [0147](0147-cull-exact-duplicate-and-burst-proposals.md) | Accepted | Exact-duplicate SHA-256 groups + burst proposals; no auto-delete; stack only on explicit accept |
| [0148](0148-native-ptp-mtp-session-and-resume.md) | Accepted | Native PTP/MTP session identity + platform matrix; fail-closed until packaged; resume checkpoints; ptp-stub fixture |
| [0149](0149-cull-near-duplicate-fingerprint.md) | Accepted | Bounded aHash near-duplicate groups; Hamming threshold; no auto-delete |
| [0150](0150-cull-keyboard-review-pick-reject.md) | Accepted | Schema v16 picked + transactional cull-review Pick/Reject/rating/colour with auto-advance; no auto-delete |
| [0151](0151-iq-cpu-gpu-consistency-gate.md) | Accepted | IQ-00 CPU gold for persist/export/reopen; GPU interactive within tolerances or fail-closed; first Ready hooks |
| [0152](0152-iq-denoise-evaluation-corpus.md) | Accepted | IQ-01 evaluation corpus contract; CPU denoise/camera-profile probes fail-closed without corpus |
| [0153](0153-specialize-tethered-studio-deferred.md) | Accepted | SPECIALIZE-01: tethered-studio deferred P2 candidate; fail-closed probe stub; HDR/Pano unselected |
| [0154](0154-external-editor-working-copy-reopen-abandon.md) | Accepted | EDITIN-01 working-copy reopen/abandon + conflict machine states |
| [0155](0155-cull-burst-stack-compare-pair.md) | Accepted | CULL-01 burst/stack Survey compare pair + adjacent step |
| [0156](0156-xmp-fail-closed-unrepresentable-multi-instance.md) | Accepted | XMP/CRS fail-closed for unrepresentable multi-instance locals |
