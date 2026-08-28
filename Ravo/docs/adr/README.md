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
| [0028](0028-original-copy-publication-contract.md) | Accepted | Original copy uses bounded exact-byte streaming, exclusively owned temporary state, atomic no-replace publication, and complete structured failures without retiring the legacy plugin. |
| [0029](0029-colorcorrection-contract.md) | Accepted | Color Correction preserves explicit five-field affine D50 Lab behavior, strict 0029/0092 unmasked import, and one Develop/CLI/Catalog/Studio contract. |
| [0030](0030-typed-jpeg-export-options.md) | Accepted | JPEG export owns typed quality 5–100/default 95 and five frozen sampling modes through one service-to-adapter value contract without completing metadata or storage publication. |
| [0031](0031-colorcontrast-contract.md) | Accepted | Color Contrast owns a versioned D50 Lab axis-affine CPU contract, deterministic schema-v1 compatibility, and a strict default-unmasked legacy importer boundary. |
| [0032](0032-encoded-byte-publication-contract.md) | Accepted | Encoded bytes use shared private destination primitives for exclusive temporary ownership, synchronized contents, atomic no-replace publication, and stable failure context without completing I14 storage policy. |
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
