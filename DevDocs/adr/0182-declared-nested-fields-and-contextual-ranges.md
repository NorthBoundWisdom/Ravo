# ADR-0182: Declared nested fields and contextual ranges

- Status: Accepted
- Date: 2026-10-12
- Extends: [ADR-0181](0181-retire-unused-dependencies-and-product-stubs.md)

## Decision

Recipe's immutable scalar descriptor also owns independent Profile Gamma and
Primaries field assignment, reset, clamp/default and schema. Primaries degree
inputs retain the existing multiply-by-pi then divide-by-180 evaluation order;
canonical storage stays radians. Finite-only assignment and explicit nonfinite
clamp defaults remain unchanged. The table owns no state, allocation-backed
callback, task or thread; accessors borrow a caller's DevelopParams only for the
synchronous call. Special group/presence/mask/instance handlers stay explicit.

Crop positions/extents and RGB parametric split metadata use declared Number
types and coarse bounds rather than probing an identity recipe. Their accepted
values depend on other fields: crop position/extent and ordered split spacing.
DevelopSetField carries an additive `context_dependent` flag; CLI field v1 emits
it only when true, and Studio capabilities/MCP forward the same result. Bounds
are necessary limits, not a guarantee that every value is valid for every recipe.
The existing strict edit owner remains authoritative and rejects coupled changes
atomically. No mutation payload, persisted recipe, algorithm or renderer changes.

The former seed probe omitted the already-supported `rgbCurveSplit2` entirely:
none of its trial values was legal in the identity context. Declaration restores
discovery of this existing field while retaining every previously listed field.

Identity probing remains only for undeclared fields pending explicit semantics;
it is not used as a fallback for fields in the descriptor/context table. No new
dependency, cache, schema migration, transport policy or silent fallback.

## Validation

Compare the complete CLI field inventory before/after: preserve IDs/order and
unaffected schema; describe intentional contextual metadata corrections. Unit
contracts cover fractional crop/split values in different valid contexts, atomic
invalid rejection, finite-only nested assignment, nonfinite defaults, degree
float bits, individual reset/presence and recipe serialization round-trip. Real
CLI/MCP discovery must expose the same contextual metadata. Run available-host
functional contracts, QML smoke and size/boundary checks; report other hosts and
performance qualification separately.
