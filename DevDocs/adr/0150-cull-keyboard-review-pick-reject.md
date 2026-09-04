# ADR-0150: Keyboard-first cull Pick/Reject/rating/colour with auto-advance

- Status: Accepted
- Date: 2026-09-04
- Relates: CULL-01 in [TODO.md](../TODO.md)
- Extends: [ADR-0008](0008-p0-review-catalog-v2.md),
  [ADR-0147](0147-cull-exact-duplicate-and-burst-proposals.md)
- Does not supersede ADR-0147 duplicate/burst reports or ADR-0149 near-dup.

## Context

CULL-01 residuals include keyboard-first Pick/Reject/rating/colour-label with
auto-advance. ADR-0008 already persists rating, colour label, and reject.
Photographers also need an explicit **Pick** flag (mutually exclusive with
Reject), a single transactional mutation that can combine flag + rating +
colour, optional advance to the next asset in the current selection or library
query order, and undo via the returned previous review state (recipe history
does not own review flags). No auto-delete.

## Decision

### Catalog schema v16 — `picked`

- `asset.picked INTEGER NOT NULL DEFAULT 0` beside existing `rejected`.
- Domain `ReviewState` gains `bool picked = false`.
- **Mutual exclusion:** `picked` and `rejected` must not both be true. Setting
  pick clears reject; setting reject clears pick; `--unflag` / clear clears both.
- Opening a v15 catalog migrates the column in one transaction.

### Cull review mutation (`ravo.cull.review/v1`)

- `CatalogService::apply_cull_review(CullReviewRequest)` applies any subset of:
  flag (`pick` | `reject` | `unflag`), `rating`, `color_label` in **one**
  `update_review` + one revision bump (transactional).
- Returns `CullReviewResult` with updated asset, `previous_review` (for client
  undo by re-applying prior state), catalog revision, and optional
  `next_asset_id` when `auto_advance` is set.
- Auto-advance order: explicit `selection_asset_ids` when non-empty; otherwise
  `list_assets(query)` (default empty query = full library sort). Next is the
  first id after the mutated asset in that order; absent when last / not found.
- **No auto-delete.** Missing asset / invalid rating / dual flag fail closed.
- Existing `set_rating` / `set_color_label` / `set_rejected` remain; `set_picked`
  is added; `set_rejected(true)` clears pick and `set_picked(true)` clears reject.

### CLI

```text
catalog cull-review --catalog <path> --asset-id <id> \
  [--pick | --reject | --unflag] [--rating N] [--color-label NAME] \
  [--auto-advance] [--selection-asset-id <id>]... [--query-json <doc>] \
  [--revision N] [--json]
```

At least one of pick/reject/unflag/rating/color-label is required.
`--pick` / `--reject` / `--unflag` are mutually exclusive.

### Non-goals (explicit)

- Studio QML chrome / shortcut remapping (presenter may call the same API later).
- Library filters for unreviewed/picked/rejected/duplicate/burst (separate residual).
- Durable review history table; undo is previous-state replay from the result.
- Auto-delete of rejected originals.

## Consequences

CULL-01 gains a testable keyboard cull mutation contract with Pick, combined
review writes, auto-advance, and undo payload without inventing delete authority.

## Rejected alternatives

- Encoding Pick as a special rating or as `rejected=false` only.
- Separate revision bumps per field for one keyboard action.
- Storing review undo only in recipe history.
