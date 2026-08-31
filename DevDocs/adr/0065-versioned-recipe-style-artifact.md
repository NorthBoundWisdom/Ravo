# ADR-0065: Reusable presets are complete versioned recipe-style artifacts

- Status: Accepted; selective overlays extended by ADR-0098
- Date: 2026-08-29
- Extends: [ADR-0003](0003-versioned-machine-contract.md)

## Context

The old style and preset owners stored dynamic IOP parameter blobs, module
versions, and legacy ordering in database/UI state. Ravo already had a complete
versioned recipe and mask graph but no portable reusable preset surface. A
replacement must reproduce complex operations such as Retouch without reviving
old ABI params or making a second operation schema.

## Decision

- `.rstyle.json` schema v1 contains a bounded name, optional description, and
  one canonical Recipe whose asset is exactly
  `style-template` / `ravo-style://template` with no content hash.
- Creating a style replaces only source identity with that placeholder. Every
  operation, enabled/bypass state, output/input profile, mask graph, Retouch
  source geometry, and deterministic recipe order is preserved.
- Applying a style replaces only the placeholder with the target asset
  identity. Engine validates both template and applied recipe before Studio
  commits through the ordinary recipe/history/undo path.
- Parser rejects unknown/missing/wrong-type/newer fields, invalid placeholder,
  oversized files/text, malformed recipe/mask/operation state, and legacy
  `<darktable_style>` as `unsupported_legacy_dtstyle`. There is no opaque IOP
  parameter fallback.
- CLI owns `recipe style-create`, `style-validate`, and `style-apply` with
  conflict-safe complete-file output. Studio exposes explicit save/apply file
  dialogs and uses the same parser/serializer. Style files are user-owned
  portable artifacts, not hidden recent presets or catalog schema.

## Consequences

L2's GTK `libs/styles.c` module and registration are removed. Shared old
`common/styles*`, presets, undo/history, example `.dtstyle` evidence, and
dynamic module consumers remain under S10/D0 until zero-consumer cleanup.
Legacy `.dtstyle` is deliberately not converted because its operations may not
have accepted Ravo schemas.

## Rejected alternatives

- A parallel parameter/preset schema. It would drift from Recipe and fail for
  masks or future versioned operations.
- Best-effort legacy style import that drops unknown modules. A style must be
  reproducible, not silently partial.
- Persisting a hidden per-catalog preset database before a sharing/lifecycle
  requirement exists. Explicit artifacts are portable and have one owner.
