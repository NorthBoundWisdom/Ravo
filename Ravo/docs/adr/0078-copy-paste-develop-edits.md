# ADR-0078: Copy and paste apply a session clipboard of complete develop edits

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0065](0065-versioned-recipe-style-artifact.md)

## Context

Studio could save and apply a complete recipe as a `.rstyle.json` file, and
each photo already owns a history/snapshot stack. Photographers also need to
copy the current edit from one photo and paste it onto another without creating
a file or merging history stacks.

## Decision

- **Copy Edits** stores the active photo's current `DevelopParams` in a
  presenter-owned session clipboard. Before/After copies the committed recipe,
  not the transient before view.
- **Paste Edits** applies that clipboard to the active photo through the
  ordinary `commit_develop` path, so undo, catalog recipe, and history append
  stay the same as a normal edit. An empty clipboard or missing selection is
  unavailable; equal source and destination is a no-op.
- The clipboard is complete, not a partial module list. It is not written to
  the catalog or the system pasteboard. Style files remain the portable
  artifact. Per-photo history and snapshots are not copied as a stack.

## Consequences

Copy/paste is a Studio session gesture over the existing recipe contract. CLI
style-create/apply is unchanged. Multi-photo paste and OS clipboard exchange
are later work. ADR-0082 keeps the clipboard complete and adds Paste Light /
Paste Color as named grade-group apply, not a second clipboard.

## Rejected alternatives

- Copying the entire history/snapshot stack onto the destination. That would
  overwrite another photo's chronology rather than apply a current look.
- Routing through a temporary `.rstyle.json` on disk. The session clipboard is
  enough and avoids leftover files.
- Partial IOP/module pickers. A style/recipe is all-or-nothing.
