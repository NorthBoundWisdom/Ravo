# ADR-0078: Copy and paste use a selected-parameter session clipboard

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0065](0065-versioned-recipe-style-artifact.md)
- Extended by: [ADR-0098](0098-selective-develop-presets.md),
  [ADR-0107](0107-apply-develop-selection.md)

## Context

Studio could save and apply a complete recipe as a `.rstyle.json` file, and
each photo already owns a history/snapshot stack. Photographers also need to
copy chosen current parameters from one photo and paste them onto another
without creating a file, merging history stacks, or resetting unrelated edits.

## Decision

- The Edit left rail has exactly **Copy Parameters** and **Paste Parameters**.
  The former opens the same parameter-selection component used by selective
  preset saving. It lists only product-baseline-relative modifications and
  starts with nothing selected.
- An accepted copy stores one immutable `DevelopParams` snapshot plus the
  explicit sorted logical-field selection in a presenter-owned session
  clipboard. Empty, unknown, duplicate, stale, or unmodified selections reject
  without replacing an existing clipboard.
- **Paste Parameters** overlays only those fields through Recipe's shared
  `apply_develop_selected_fields` owner. Compound operations remain atomic and
  required canonical masks merge by stable ID while target-only graph state is
  preserved. The result uses the ordinary `commit_develop` path, so validation,
  undo, catalog recipe, history, progressive preview, and failure behavior stay
  the same as a normal edit.
- A missing selection or empty clipboard is unavailable. The clipboard is not
  written to the catalog or system pasteboard. Style files remain the portable
  artifact, and per-photo history/snapshots are not copied as a stack.
- The former complete clipboard plus **Paste Light** / **Paste Color** fixed
  groups are removed; user choice at copy time is the only partial-paste policy.

## Consequences

Copy/paste is a Studio session gesture over the same stable field inventory as
schema-v2 presets. CLI style-create/apply is unchanged. Applying the same
clipboard onto an explicit multi-selection is [ADR-0107](0107-apply-develop-selection.md).
OS clipboard exchange remains later work.

## Rejected alternatives

- Copying the entire history/snapshot stack onto the destination. That would
  overwrite another photo's chronology rather than apply a current look.
- Routing through a temporary `.rstyle.json` on disk. The session clipboard is
  enough and avoids leftover files.
- A complete clipboard with fixed Light/Color paste buttons. It makes copy
  implicit and forces users into coarse groups that can reset unrelated edits.
- A QML-owned field map. It would duplicate Recipe merge, mask, and validation
  policy in presentation code.
