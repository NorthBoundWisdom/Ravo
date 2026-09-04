# ADR-0155: Burst/stack Survey compare pair and adjacent step

- Status: Accepted
- Date: 2026-09-04
- Relates: CULL-01 in [TODO.md](../TODO.md)
- Extends: [ADR-0105](0105-asset-versions-stacks-and-survey.md),
  [ADR-0147](0147-cull-exact-duplicate-and-burst-proposals.md),
  [ADR-0150](0150-cull-keyboard-review-pick-reject.md)
- Does not supersede ADR-0147 propose/accept or ADR-0105 Survey slot ownership.

## Context

CULL-01 residuals include synchronized 1:1/focus inspection for burst members
and fast Survey compare. ADR-0147 can stack a user-accepted burst; ADR-0105
Survey already shows two or four **selected** assets as exact previews. Missing
is a machine contract that resolves an ordered compare pair inside a durable
library stack (accepted burst or ordinary stack) and steps previous/next
without QML owning membership or preview work. Ephemeral burst-proposal IDs are
not a durable compare authority.

## Decision

### Burst/stack compare pair (`ravo.cull.burst-compare/v1`)

- Pure helper `resolve_burst_compare_pair(stack, focus_asset_id, step)` and
  `CatalogService::resolve_burst_compare_pair(request)` load the asset’s
  `stack_id`, require the stack to exist with **at least two** members, and
  return:
  - `stack_id`, ordered `member_ids`,
  - `focus_index` / `focus_asset_id` after applying `step`
    (`current` | `previous` | `next`),
  - `compare_asset_id` = the adjacent member toward the step direction when
    possible, otherwise the other neighbor (so Survey always has two distinct
    slots).
- `previous` / `next` move focus along `member_ids` and **clamp** at the ends
  (no wrap). Missing asset, missing stack, singleton stack, or focus not in
  the stack fail closed.
- No catalog mutation. No auto-delete. Preview pixels stay on CatalogService
  `request_preview` via existing Survey ownership.

### CLI

```text
catalog cull-burst-compare --catalog <path> --asset-id <id> \
  [--step current|previous|next] [--json]
```

### Studio

- Commands open or step a Survey pair from the selected asset’s stack:
  set selection to `[focus, compare]`, keep focus primary, enter Survey.
- Optional 1:1: when an inspect ROI is already active, stepping refreshes the
  same normalized ROI on the new focus (presentation sync only; no second ROI
  authority in QML).
- QML does not invent stack membership or compare math.

### Non-goals

- Using unaccepted burst-proposal reports as durable compare membership.
- Auto-stacking on compare open.
- Side-by-side Develop pipelines or Before/After recipe compare (unchanged).
- Auto-delete / auto-reject.

## Consequences

CULL-01 gains a testable first Ready for stack/burst Survey pair compare and
adjacent keyboard stepping shared by service, CLI, and Studio.

## Rejected alternatives

- QML walking selection order without a C++ stack contract.
- Wrapping at stack ends (hides the end of a burst during cull).
- Treating ADR-0147 proposal IDs as durable membership without accept/stack.
