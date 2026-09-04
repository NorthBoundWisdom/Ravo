# ADR-0137: Shoot-consistency batch proposals (AI-03 stub)

- Status: Accepted
- Date: 2026-09-04
- Relates: AI-03 in [TODO.md](../TODO.md), [ADR-0121](0121-ai-architecture-privacy-provenance.md)
- Extends: AI-01/AI-02 proposal store and apply/reject/cancel lifecycle

## Context

AI-01/AI-02 ship single-asset reviewable proposals. AI-03 needs reference-grade
consistency across an explicitly selected shoot without silent multi-asset
mutation, network inference, or packaged weights.

## Decision

### Proposal kind `shoot-consistency`

- Add `AiProposalKind::kShootConsistency` (`"shoot-consistency"`).
- Stub provider model `deterministic-shoot-consistency-v1` under
  `ravo.local.stub` (no network, no weights, no training, no auto-apply).
- ADR-0121 privacy rules unchanged: explicit `--user-initiated` required;
  proposals are durable session JSON under `{catalog}.ai_proposals/`.

### One durable proposal per destination

- Create requires a **reference asset id** and an explicit non-empty list of
  **destination asset ids**. The user must select every destination.
- Creates **one** pending proposal per destination (never one mega-proposal that
  mutates many assets). Reference is never a silent destination.
- Destination ids must be unique, non-empty, and must not include the reference.
- Session pending limit is checked against the whole batch before any publish.

### Stub field copy (v1)

- Copies allowlisted **global** develop fields from the reference recipe onto
  each destination proposal: white balance, exposure, tone, and colour owners
  already on the AI-01 allowlist.
- **No masks** in v1. **No crop/straighten** in v1 (crop only if identical
  aspect is deferred; first Ready omits geometry).
- Field values are absolute develop `--set` values taken from the reference;
  apply still goes through existing AI-01 `apply_ai_proposal` /
  `save_develop_with_history` validation.

### Apply / reject / cancel

- Per-destination apply/reject/cancel reuse existing AI-01 APIs.
- Multi-select apply, when used, follows the existing multi-selection revision
  contract (`apply_develop_selection` / expected revision).
- Cancelling or rejecting one destination mid-batch leaves already-applied
  destinations applied and other pending proposals untouched (report partial).

### CLI

```text
catalog ai-propose --proposal-kind shoot-consistency \
  --reference-asset <id> --destination-assets <id>… --user-initiated
```

Returns a JSON object with a `proposals` array (one entry per destination).

## Non-goals (explicit)

- Real model weights, network providers, or auto-apply.
- Auto-selecting destinations from folders/shoots.
- Mask or crop geometry copy in v1.
- Silent catalog mutation without per-destination proposals.

## Consequences

AI-03 first Ready ships a reviewable batch stub on main. Operators can propose
reference grades across selected assets and apply/reject/cancel per image.
URI/quality evaluation against real models remains residual packaging work.

## Rejected alternatives

- One mega-proposal that applies to many assets in a single commit.
- Auto-including folder members without explicit destination selection.
- Copying crop/masks in the first stub.
- Network or weight-backed providers before Packaging/Dependency evidence.
