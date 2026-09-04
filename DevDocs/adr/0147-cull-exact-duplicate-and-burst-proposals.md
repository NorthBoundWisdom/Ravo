# ADR-0147: Exact-duplicate hash groups and burst grouping proposals

- Status: Accepted
- Date: 2026-09-04
- Relates: CULL-01 in [TODO.md](../TODO.md)
- Extends: [ADR-0105](0105-asset-versions-stacks-and-survey.md),
  [ADR-0142](0142-ai-metadata-culling-suggestions.md) (deterministic, non-AI)
- Does not supersede ADR-0142 stub duplicate *cues*; this ADR owns catalog-fact
  duplicate/burst **assistance** without model weights.

## Context

CULL-01 needs high-throughput review assistance before AI-04 quality work.
Photographers need exact same-bytes detection and burst grouping proposals that
never auto-delete, auto-reject, or conflate virtual copies, RAW+JPEG companions,
or distinct byte identities. ADR-0142 duplicate suggestions remain non-authoritative
cues; operators still need a deterministic, hash-based catalog report.

## Decision

### Exact duplicate groups (`ravo.cull.exact-duplicate/v1`)

- `CatalogService::find_exact_duplicate_groups` hashes each listed asset's on-disk
  bytes (SHA-256) and groups members that share a digest.
- Outcomes per group:
  - `same_file` — every member shares one `normalized_uri` (virtual copies /
    versions of one file). Reported for clarity; **not** an ingest-duplicate
    delete candidate.
  - `same_bytes` — identical SHA-256 across **distinct** `normalized_uri` values.
    Actionable exact-duplicate assistance.
  - `distinct_version` reserved; v1 does not emit mixed same_file+same_bytes
    groups (members are partitioned by URI class first).
- Never conflate: virtual copies of one URI stay `same_file`; files with different
  bytes never share a group; derived assets only appear when their bytes match.
- Missing originals are listed under `skipped` with `original_missing`; they do
  not form groups.
- **No auto-delete, auto-reject, or catalog mutation** from this report.

### Burst grouping proposals (`ravo.cull.burst-proposal/v1`)

- `CatalogService::propose_burst_groups` clusters assets that share camera
  make+model (or both absent as `unknown`) and whose `captured_unix_s` values
  fall within a bounded window (default 1s; CLI `--burst-window-seconds`).
- Groups require ≥2 members with capture timestamps. Assets without
  `captured_unix_s` are skipped.
- Proposals are **ephemeral reports** (no durable watch-folder / auto-stack).
  Optional `accept_burst_group_proposal` (user-initiated) stacks members via
  existing ADR-0105 `stack_assets` (pick = earliest capture, then asset id).
  Dismiss is a no-op (do not call accept).
- **No automatic delete or reject.**

### CLI

```text
catalog cull-exact-duplicates --catalog <path> [--json]
catalog cull-burst-propose --catalog <path> [--burst-window-seconds N] [--json]
catalog cull-burst-accept --catalog <path> --user-initiated \
  --asset-id <id>... [--revision N] [--json]
```

`cull-burst-accept` takes explicit member asset ids from a prior propose report
(no silent re-query). Fail-closed on stack conflict / stale revision.

### Non-goals (explicit)

- Perceptual / near-duplicate embeddings (later CULL tranche).
- Keyboard Pick/Reject chrome and Studio 1:1 sync (later).
- Auto-stack on import, auto-delete, or AI ranking.
- Treating ADR-0142 stub duplicate suggestions as catalog facts.

## Consequences

CULL-01 gains a testable first Ready for exact hash duplicates and burst
proposals without mutating originals or inventing delete authority.

## Rejected alternatives

- Reusing ADR-0142 AI stub duplicate cues as the only duplicate surface.
- Auto-stacking bursts on import or propose.
- Grouping by size+mtime `content_fingerprint` (not byte identity).
