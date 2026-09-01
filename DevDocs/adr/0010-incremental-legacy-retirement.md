# ADR-0010: Retire accepted legacy owners incrementally

- Status: Partially superseded by
  [ADR-0106](0106-close-legacy-algorithm-migration.md) for remaining leftover C
- Date: 2026-08-25
- Supersedes: ADR-0004 deletion timing only
- Relates to: ADR-0004, `DevDocs/MIGRATION.md`

Remaining leftover C is deleted with `legacy/` under leftover TODO L0 rather
than per-IOP leftover-faithful ports (ADR-0106). Incremental retirement already
applied to accepted owners whose leftover files were removed before that
decision.

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
- After an explicitly resumed legacy migration TODO item meets its
  Ravo-accepted gate, delete only the named old owner files. Record them in
  the retired leftover path list.
- Remaining leftover files must continue to match the freeze-commit blobs.
  `check_freeze_reference.py` verifies that invariant plus the retired set.
  Leftover `iop/CMakeLists.txt` and `libs/CMakeLists.txt` may drop retired
  registrations and are not blob-compared to the freeze.
- Shared decode, fixture, and leftover GTK/Lua/OpenCL paths stay until their
  own acceptance or the leftover list in `DevDocs/MIGRATION.md`.
- The first retired owners are `libs/export.c` and `libs/export_metadata.c`,
  replaced by `CatalogService::export_asset`. `iop/tonecurve.c` is retired after
  `ravo.core.tonecurve` acceptance; leftover `rgbcurve.c` stays.
  `iop/sigmoid.c` is retired after `ravo.display.sigmoid` acceptance;
  unselected `filmicrgb.c` and `agx.c` stay as explicit leftover.

## Consequences

- Freeze checks no longer require a bit-identical `legacy/src` tree.
- A mistaken retirement is a documentation-and-checker change, not a silent
  source edit.
- After the migration queue is empty, the product evidence TODO still requires a
  separate archive/removal decision for leftover that Ravo will never own.

## Rejected alternatives

- **Keep ADR-0004’s final-bulk-only deletion:** leftover would keep product owners
  that Ravo already replaced.
- **Delete imageio encode and decode together:** import still needs decode.
