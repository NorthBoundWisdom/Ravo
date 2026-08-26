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
