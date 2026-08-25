# ADR-0010: Retire accepted legacy owners incrementally

- Status: Accepted
- Date: 2026-08-25
- Supersedes: ADR-0004 deletion timing only
- Relates to: ADR-0004, root `TODO.md`

## Context

ADR-0004 froze 0.9 and deferred all old-owner deletion to M7 so the leftover
tree stayed an immutable oracle. The product owner then asked to rewrite
legacy capabilities in Ravo one at a time and delete the corresponding old
implementation after each Ravo acceptance, leaving only explicit leftover.

Keeping the entire `legacy/src` tree forever after a capability is owned by
Ravo makes the leftover inventory dishonest. Deleting files before Ravo
acceptance would destroy static evidence still needed for the rewrite.

## Decision

- Ravo remains the only production growth path. Production code still must
  not include, link, or load `legacy/src`.
- Configuring, compiling, or running the 0.9 application remains forbidden.
- After a `TODO.md` item meets its Ravo-accepted gate, delete only the named
  old owner files. Record them in
  [`../phase0/legacy-retired-src-paths.txt`](../phase0/legacy-retired-src-paths.txt).
- Remaining leftover files must continue to match the freeze-commit blobs.
  `check_freeze_reference.py` verifies that invariant plus the retired set.
- Shared decode, fixture, and leftover GTK/Lua/OpenCL paths stay until their
  own acceptance or the leftover list in `TODO.md` §5.
- The first retired owners are `libs/export.c` and `libs/export_metadata.c`,
  replaced by `CatalogService::export_asset`.

## Consequences

- Freeze checks no longer require a bit-identical `legacy/src` tree.
- A mistaken retirement is a documentation-and-checker change, not a silent
  source edit.
- M7 still cleans leftover that Ravo will never own.

## Rejected alternatives

- **Keep ADR-0004’s M7-only deletion:** leftover would keep product owners
  that Ravo already replaced.
- **Delete imageio encode and decode together:** import still needs decode.
