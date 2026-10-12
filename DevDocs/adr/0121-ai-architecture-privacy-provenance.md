# ADR-0121: AI architecture, privacy, provenance, and licence boundary

- Status: Accepted privacy/provenance/licence policy; stub proposal implementation superseded by ADR-0181
- Date: 2026-09-03
- Relates: AI-00 in [TODO.md](../TODO.md), ProductRoadmap AI section
- Extends: [ADR-0009](0009-p1-develop-recipe.md),
  [ADR-0043](0043-canonical-mask-graph-foundation.md),
  [ADR-0064](0064-atomic-metadata-refresh-and-export-privacy.md),
  [ADR-0105](0105-asset-versions-stacks-and-survey.md),
  [ADR-0119](0119-hierarchical-keywords.md)

## Context

Studio already has a conversational assistant panel (ADR-0081) that must not be
read as authorization for model inference, image upload, automatic catalog
mutation, or generated-pixel publication. AI-01…AI-05 need one dated boundary
for owners, privacy, provenance, and licence before any Ready tranche.

This ADR retains the privacy, provenance and licensing constraints for a future
real provider. Deterministic proposal/suggestion providers and their stores are
retired by [ADR-0181](0181-retire-unused-dependencies-and-product-stubs.md).
Current external AI grading uses the shared revision-bound CLI/MCP contract in
[ADR-0168](0168-ai-control-and-mcp.md), not an internal inference provider.
A future provider requires an explicit admission and package/dependency evidence.

## Decision

### Initiation, privacy, and redaction

- Every inference or apply is **explicit user initiation**. No import-time,
  browse-time, or idle background AI mutation.
- Default network posture: **no automatic upload**. A remote provider requires an
  explicit per-session or per-job confirmation that names destination, payload
  classes, and retention claim.
- Payload minimization is mandatory: prefer downscaled working buffers or
  recipe/mask JSON over originals; strip GPS/people identity unless the user
  opts a named privacy mode that permits them (ADR-0064 modes remain the export
  baseline; AI payloads get their own enumerated allow-list).
- Credentials never enter ordinary logs. Prompts, original paths, EXIF, catalog
  dumps, and raw provider responses are redacted from default logging; debug
  capture is an explicit developer switch with local-only files.
- **Providers must not train on user photos unless the user opts in to a named,
  recorded policy.** The default is no training / no retention beyond the job.
- Cancellation, timeout, retry budget, concurrency, and memory bounds are owned
  by the services façade and fail closed (no partial catalog write on cancel).

### Future provider boundary

A real provider must produce reviewable, versioned recipe/mask suggestions with
source asset and observed revisions, provider/model identity or weight hash,
settings and validated ordinary Develop fields. Reject/cancel leaves the catalog
unchanged; accepted edits use existing services, normal history and revision
checks. Unknown fields, stale revisions, invalid bounds, provider failure and a
missing model fail without partial state. This policy does not implement a
proposal store or authorize restoration of retired stub commands.

### Derived-asset contract (non-replayable pixels)

AI-05 and any generative fill that cannot round-trip through recipe ops publish
an **immutable derived asset/version** (ADR-0105), never mutating original bytes:

- source asset + revision, model identity/hash, settings, output content hash
- retention/backup/restore follow catalog backup policy; missing-model reopen
  still shows the stored pixels and provenance, and refuses regenerate-without-
  model rather than inventing pixels
- originals remain reference-only and byte-stable

### Hard product prohibitions

- No auto-reject, auto-delete, auto-publish, or silent identity-sensitive
  metadata write (AI-04). Suggestions are separated from catalog facts until
  explicit accept.
- No writable originals as a requirement of any AI path.
- No second live authority beside SQLite for accepted edits.

### Licence, runtime, and package (acceptance constraints)

Before the first shipped provider:

1. Dependency Workflow records the runtime/weight source, GPL/licence
   compatibility, third-party notices, and update channel.
2. Package size and optional download vs bundled weight are explicit in
   `Packaging.md`.
3. Local cache location is under the catalog or user cache root—not beside
   originals—and is rebuildable/deletable without catalog loss.
4. Telemetry is off by default; any telemetry ADR must be separate and opt-in.
5. A licensed evaluation corpus and human review protocol (cameras, lighting,
   genres, skin tones, edge cases) must exist before claiming AI-01 quality.

A future provider reports a missing model explicitly; no silent CPU
approximation of a generative model.

## Consequences

Real model quality, provider choice and generative publication remain gated in
TODO.md. The existing assistant is not implicit image-upload authorization.
Current CLI/MCP control does not claim a packaged inference model.

## Rejected alternatives

- Treating the existing chat panel as an inference authorization.
- Auto-running models on import or gallery idle.
- Writing AI results into adjacent XMP as a second authority.
- Auto-reject/delete based on aesthetic or duplicate scores.
- Requiring originals to be writable for generative workflows.
