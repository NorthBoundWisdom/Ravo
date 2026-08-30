# ADR-0011: Develop publication is atomic and preview work is revision-owned

- Status: Accepted
- Date: 2026-08-26
- Relates to: ADR-0009, root `TODO_LEGACY_MIGRATION.md` C1

## Context

Studio Develop already coalesced saves and previews, but recipe JSON, automatic
history and catalog revision were separate writes. A late history failure could
therefore expose a new recipe without its history/revision. Interactive preview
results were revision-checked after completion, while superseded RAW work kept
running under the window-lifetime token.

## Decision

- `CatalogRepository::commit_recipe` publishes the active recipe (or explicit
  baseline), optional deletion of history rows with `seq` greater than a cursor,
  the deduplicated automatic history entry, and catalog revision in one
  adapter-owned transaction. `RecipeHistoryWrite::kUnchanged` updates the
  current recipe without touching the stack so a later edit can still discard
  newer steps.
- Studio assigns a session-only coalescing key to a committed control. Adjacent
  commits for the same control keep one undo anchor and pass the exact history
  row returned by the preceding transaction. The repository replaces that row
  only while it remains the asset's newest ordinary history entry; a snapshot,
  another control, selection/view change, undo/redo, or intervening client row
  ends the group. A stale expected row appends instead of overwriting the
  intervening work.
- Any statement or commit failure rolls back the whole transaction; callers
  continue to see the previous recipe, history, and revision.
- A desktop-owned `PreviewRequestOwner` gives each in-flight Develop request a
  cancellation token and monotonically increasing revision. A new edit or asset
  selection cancels the borrowed token immediately.
- Completion is accepted only when both revision and selected asset still
  match. Window destruction cancels the same owner before the serial executor
  drains and releases `CatalogService`.
- RAW interactive preview and full render share the scene-linear CPU working
  buffer. The embedded JPEG remains browse-only even if an interactive caller
  requests embedded preference.

## Consequences

- Save failure can revert Studio memory state without compensating database
  writes.
- Repeated pauses and drags on one slider retain only its final history state;
  Undo returns to the value before that control-adjustment group.
- Rapid selection stops obsolete decode/render work and still rejects a result
  that races with cancellation.
- Tests can inject a SQLite history failure and prove recipe/history/revision
  rollback, and can compare interactive RAW pixels directly with the full CPU
  path.

## Rejected alternatives

- Compensating writes in `CatalogService`: a second failure could still leave a
  split state.
- One window-global cancellation token: it cannot stop superseded work without
  also permanently cancelling later requests.
