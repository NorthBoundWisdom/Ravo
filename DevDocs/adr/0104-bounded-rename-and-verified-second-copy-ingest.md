# ADR-0104: Bounded rename and verified second-copy ingest

- Status: Accepted
- Date: 2026-09-01
- Extends: [ADR-0102](0102-planned-managed-import-workspace.md)

## Context

ADR-0102 accepts deterministic Add, Copy, and Move from a local scanned root,
but preserves every source name and writes only one managed destination. A
mounted camera card therefore cannot apply a job naming convention or create
the independent on-ingest copy expected before the card is cleared. These
capabilities do not require catalog schema, collection identity, query, or
paging changes.

## Decision

- `ImportRequest` may carry an optional schema-v1 filename template and an
  optional second-copy root. Both are valid only for Copy or Move. Add remains
  a reference-only operation and rejects either option.
- The filename grammar is bounded UTF-8 with only `{date}`, `{stem}`,
  `{sequence}`, and `{ext}`. `date` is `YYYYMMDD` from the same capture-date or
  source-mtime value used by dated organization; `sequence` is the stable
  one-based position in the deterministic complete plan, padded to four
  digits. An omitted `{ext}` appends the original extension. Separators,
  traversal, control characters, portable device names, unknown tokens,
  oversized expansions, and names that differ only by ASCII case reject.
- The second-copy root must be an existing directory distinct from the primary
  destination. It receives the same organization, renamed basename, and
  same-stem XMP and JPEG companions as the primary tree. Only the primary
  media path is cataloged.
- Planning covers every selected primary and second-copy media/XMP/JPEG path
  before the first file is published. Existing files or symlinks, duplicate
  portable path keys, source/output aliasing, catalog URI conflicts, and
  ambiguous XMP or JPEG companions reject with zero media or catalog
  publication. Ravo never chooses a suffix or overwrites a target.
- Each output uses the existing bounded atomic no-replace copy. When a second
  copy is requested, the source, primary, and second copy are reopened and
  compared byte for byte under cancellation after publication. XMP and JPEG
  companions are verified the same way. The primary path enters the catalog
  only after all requested copies verify. A pre-catalog failure removes only
  files owned by that item; completed earlier items remain explicit partial
  delivery.
- Move removes the reverified source media, XMP, and JPEG companion only after
  both copies verify and the primary path is cataloged. Cleanup failure retains
  safe source bytes and remains a structured result as in ADR-0102.
- CLI executes the complete service operation synchronously. Studio keeps the
  template and second-copy selection as import-workspace session state and uses
  its existing serial import and cancellable preview owners. Rename and
  second-copy choices remain session state. The primary destination preference
  and derived content index follow the updated ADR-0102 contract.

## Failure and validation

Invalid request state, template expansion, root/path conflict, copy I/O,
verification mismatch, cancellation, catalog publication, and Move cleanup
retain distinct structured reasons and all relevant source/primary/second-copy
paths. Tests cover deterministic expansion, two-tree preflight, XMP carriage,
byte verification, cancellation and injected mismatch, per-item cleanup,
source SHA-256/size/mtime preservation, CLI JSON, and Studio intent wiring.

PTP/MTP transport, DNG conversion, metadata presets, Smart Previews, and
HEIC/HEIF decoding remain separate undecided PW2 contracts. A mounted card is
an ordinary explicit local source root; this decision does not guess a device
from process or UI state.

## Consequences

The live catalog continues to store only final primary URIs, so this tranche is
independent of named collections and requires no migration. Verification costs
one additional read of every requested media and sidecar copy; that deliberate
I/O is the integrity contract and is not replaced by size/mtime equality.
