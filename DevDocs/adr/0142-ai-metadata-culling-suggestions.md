# ADR-0142: AI-04 metadata/culling/similarity suggestion stubs

- Status: Accepted
- Date: 2026-09-04
- Relates: AI-04 in [TODO.md](../TODO.md), [ADR-0121](0121-ai-architecture-privacy-provenance.md)
- Extends: ADR-0121 proposal/privacy rules; existing keyword and writable-metadata APIs

## Context

AI-04 needs optional keyword/caption suggestions and focus/duplicate cues without
auto-reject, auto-delete, auto-publish, or silent identity-sensitive writes.
ADR-0121 already forbids those behaviours; this ADR accepts the first Ready
**stub** contract so operators can exercise create/list/accept/reject without
shipping model weights.

## Decision

### Suggestion kinds (non-authoritative)

| Kind | Stub payload | Accept behaviour |
| --- | --- | --- |
| `keyword` | suggested tag path(s) | apply via existing `set_tags` (merge) |
| `caption` | suggested `description` + optional `headline` | apply via `set_writable_metadata` |
| `focus` | focus/exposure cue text + confidence | mark accepted only (no catalog mutation) |
| `duplicate` | peer asset id(s) + confidence | mark accepted only; **never** reject/delete peers |

Suggestions are durable JSON under `{catalog}.ai_suggestions/`, separate from
catalog facts and from Develop `AiProposal` records.

### Lifecycle

- Create requires `--user-initiated` and stub provider `ravo.local.stub` /
  model `deterministic-suggestion-v1` (no network, no weights, no training).
- Reject/cancel change suggestion status only; catalog unchanged.
- Accept is explicit and goes through existing metadata/tag APIs for keyword/
  caption; focus/duplicate acceptance is acknowledgement only.
- No auto-reject, auto-delete, auto-publish, or import-time/idle suggestion
  application (ADR-0121).

### CLI

```text
catalog ai-suggest --suggestion-kind keyword|caption|focus|duplicate \
  --asset-id <id> --user-initiated
catalog ai-suggestion --suggestion-id <id>
catalog ai-suggestions [--asset-id <id>]
catalog ai-suggestion-accept --suggestion-id <id>
catalog ai-suggestion-reject --suggestion-id <id>
catalog ai-suggestion-cancel --suggestion-id <id>
```

## Non-goals (explicit)

- Real model weights, embeddings, or network providers.
- Auto-culling, auto-delete of near-duplicates, or silent keyword writeback.
- People/face identity metadata.
- Studio chrome in this tranche.

## Consequences

AI-04 first Ready ships reviewable stub suggestions on main. Operators accept
keyword/caption via ordinary metadata APIs; focus/duplicate remain cues until a
later quality tranche.

## Rejected alternatives

- Reusing Develop `AiProposal` field diffs for metadata suggestions.
- Auto-applying suggestions on import or gallery idle.
- Deleting or rejecting assets from a duplicate score.
