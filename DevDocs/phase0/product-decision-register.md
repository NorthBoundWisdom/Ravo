# Phase 0 Product Decision Register

> Historical decision register. ADR-0007 and the current root roadmap now
> decide the first SQLite catalog, reference-only import, external preview
> cache, and C++-backed Qt Quick/QML viewer slice. Unresolved legacy migration
> and later editing rows remain evidence, not blockers for the accepted baseline.

## Status

This register retains the product decisions that remain unresolved. ADR-0007
is the dated authority for the first product slice; other rows become decided
only when they contain a decision, accountable owner, review owner, date, and a
link to the supporting product or release record. Until then, Ravo must retain
the explicit `pending`, `defer`, `read-only`, or `unsupported` state recorded
elsewhere; it must not infer compatibility from a legacy source file or fixture
name.

No entry here authorizes a modification of frozen `legacy/src/`, an old catalog, or a
legacy expected image.

## Required ownership and freeze record

| Decision | Required record | Current state | Gate |
| --- | --- | --- | --- |
| First-release product owner | Name/role, decision authority, and effective date. | ADR-0007 records the current product direction; a named accountable release owner remains unassigned. | Required before packaged-release acceptance. |
| Ravo code-review owner | Name/role, review responsibility, and effective date. | Unassigned. | Required before packaged-release acceptance. |
| Frozen 0.9 reference | Immutable commit/tree IDs plus a complete hashed manifest of committed fixture inputs. | Complete: source/fixture commit `320970bf7c9cbbc6611cfc3eb60f8f2b0424b782` is recorded; `src` tree `a3ac761ecbb0cf668ecad49aff8bd0e29235f5f7`, fixture tree `1dc38893f39e113620aebbbdc927218ca4a2b8af`, five source images, and all 158 fixture directories are protected by the manifest and freeze checker. | No executable artifact or runtime result is required. Old configuration, compilation, execution, CTest, packaging, and CLI use are prohibited. |
| First-release scope and platforms | Retained workflows, supported operating systems, offline/privacy obligations, and release threshold. | The root roadmap targets a local, offline SQLite catalog/review/develop/export product on macOS, Windows, and Linux; packaging acceptance details remain open. | Required before declaring a packaged Ravo release accepted. |
| Parallel engineering isolation | CI owners, test corpus retention, package/signing IDs, and user-data directory policy. | Unresolved. | Required before a packaged Ravo candidate. |

## Legacy data and capability decisions

Each row requires one of `keep`, `defer`, `read-only migration`, or
`unsupported`. A non-empty rationale must explain data-loss, visual, security,
or operational consequences. The implementation owner must add a migration
test or an explicit rejection test before a `keep` or `read-only migration`
decision can claim support.

| Data or capability group | Source inventory | Required decision evidence | Current state |
| --- | --- | --- | --- |
| IOP registry | [Capability inventory](capability-inventory.md) lists the remaining legacy registrations, retired owners, and fixture presence. | Retained operation list; individual migration/rejection rationale; first required fixture and schema target. | Decisions are made one capability at a time under the root [`TODO_LEGACY_MIGRATION.md`](../../TODO_LEGACY_MIGRATION.md) acceptance gate. |
| RAW white balance | `temperature`, LibRaw camera metadata and `channelmixerrgb` CAT. | Unique stage owner, as-shot/daylight/manual schema, missing-metadata behavior, late-reference overlap and RAW fixture evidence. | **Keep four-channel temperature scaling and remove Kelvin/tint approximation.** Decision/review owner: WisdomToNorth, 2026-08-26. LibRaw `cam_mul`/`pre_mul` are the automatic metadata owners; missing values fail without generic fallback. Late reference requires an explicit later CAT and never mutates hidden global chroma state. |
| Masks and blend | Legacy XMP masks plus `src/develop` behaviour. | Canonical graph scope, supported shapes/blends, legacy migration/rejection strategy, and CPU test plan. | Deferred pending product and render-contract decision. |
| Metadata and GPS | XMP/EXIF plus the frozen product scope. | Read/write fields, orientation/ICC policy, GPS read-only boundary, and unsupported fields. | Unresolved. |
| Catalog | Existing catalog/database files. | Import, coexistence, backup, rollback, and user-data-dir policy. | ADR-0007 authorizes a new private SQLite schema and reference-only import; legacy catalog migration remains deferred. |
| XMP sidecars | Legacy XMP schema 6 and per-operation payloads. | Approved operation mappings, unknown-data policy, and readable rejection schema. | Empty history, the exact frozen `nop.xmp` built-in RAW baseline, and a schema-6/v5 manual zero-black unblended mask-free singleton exposure subset are proven; all other mappings remain unresolved or explicitly rejected. |
| Styles and presets | Legacy style/preset stores. | Compatibility choice, fragment schema, conflict policy, dry-run report, and data-loss explanation. | Deferred to the root roadmap’s post-queue decisions. |
| Local export | JPEG, PNG, TIFF, and original-file copy. | Format, bit depth, alpha, metadata, ICC, overwrite, and disk-full policy. | Ravo CatalogService/CLI/Studio accept JPEG/PNG/original copy and TIFF when the Qt plugin exists. Metadata/ICC embedding remains later. |
| Default display transform | `filmicrgb`, `sigmoid`, `agx`. | One default transform, explicit colour contract, recipe schema, CPU/RAW reference and overlap policy. | **Keep Sigmoid as default.** Decision/review owner: WisdomToNorth, 2026-08-25. `ravo.display.sigmoid` v1 uses linear sRGB, per-channel generalized log-logistic mapping, fixed Standard SDR targets, and recipe-owned contrast/skew/hue preservation. RAW baseline rendering enables it without marking an asset edited. RAW Studio Contrast is owned by Sigmoid; `ravo.core.contrast` remains the raster-input P1 control and old-recipe compatibility op. Highlights/shadows/whites/blacks remain pre-display controls. ADR-0015 promotes `filmicrgb` and `agx` as future explicit optional algorithms; neither becomes an implicit fallback. |
| HSL zoning | `colorequal`, `colorzones`. | One default hue-partition capability and explicit optional-operation overlap policy. | **Keep `colorequal` as default.** Decision/review owner: WisdomToNorth, 2026-08-25. `ravo.color.colorequal` v1 is the frozen `colorequal.c` dt UCS 22 8-node periodic RBF equalizer on the linear working image. ADR-0015 promotes `colorzones` as a future explicit optional algorithm, not a fallback. |
| Scene color grading | `colorbalancergb`, `colorbalance`, former Ravo lift/gamma/gain approximation. | Distinct schemas, working spaces, default formula, fixture/RAW evidence and overlap policy. | **Keep full independent `colorbalancergb` and `colorbalance`; remove the approximation.** Decision/review owner: WisdomToNorth, 2026-08-27. `ravo.color.colorbalancergb` v1 owns Filmlight Yrg grading with DT UCS default and explicit JzAzBz. [ADR-0025](../../Ravo/docs/adr/0025-legacy-colorbalance-contract.md) assigns the separate full Lab D50/ProPhoto lift/gamma/gain and slope/offset/power path to `ravo.color.colorbalance` v1. Both frozen owners are retired; neither operation is a fallback or alias for the other. |

## Completion protocol

1. The product owner selects a disposition and signs the row with a dated
   product record.
2. The code-review owner links the implementing Ravo target, tests, fixture,
   and any deliberate rejection behaviour.
3. The Phase 0 inventory and root TODO are updated together; an operation is
   not treated as retained merely because a descriptor, legacy XMP name, or
   old expected PNG exists.
4. A capability reaches `Ravo accepted` only after its CPU, error, cancellation,
   resource, and fixture evidence meets the migration policy. Old-owner removal
   follows the root active migration TODO gate, not a final bulk sweep.
