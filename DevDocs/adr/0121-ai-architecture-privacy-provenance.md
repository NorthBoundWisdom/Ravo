# ADR-0121: AI architecture, privacy, provenance, and licence boundary

- Status: Accepted
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

This ADR accepts the architecture. Shipping a concrete local/remote model binary,
weight pack, or paid provider remains a packaging follow-through recorded in
`Packaging.md` / dependency workflow when first introduced—not a silent default.

## Decision

### Owners (no business rules in QML)

| Concern | Owner |
| --- | --- |
| Conversational UX / intent forwarding | Desktop presenter (existing assistant panel) |
| Inference provider port (local/remote) | Adapters behind a versioned services façade |
| Proposal create/validate/list | Services (`AiProposal` owner; name finalized with AI-01) |
| Apply proposal → recipe/mask | CatalogService / existing Develop + mask owners |
| Immutable generated-pixel derived asset | CatalogService version/stack path (ADR-0105), not in-place original rewrite |
| Credentials | Desktop-only secret store; never services logs or QML |
| Evaluation corpus protocol | DevDocs + private corpus (not product runtime) |

QML may display proposals and forward accept/reject/cancel only.

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

### Proposal contract (replayable recipe/mask work)

AI-01…AI-04 consume a versioned **proposal** artifact (not a silent recipe write):

- identity: proposal id, created time, source `asset_id`, observed catalog
  revision and recipe/recovery generation
- provider: vendor/model id, model version or weight content hash, parameters
- body: ordinary Develop fields and/or canonical mask graph fragments only
- optional confidence / alternatives
- validation: deterministic schema + Develop/mask bounds **before** apply
- lifecycle: reject/cancel changes nothing; apply goes through existing
  `save_develop` / mask commit / keyword APIs and creates normal history/undo

Unknown fields, stale revision, invalid bounds, provider failure, or missing
model fail without partial catalog state.

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

Offline / no-model behavior: proposals and apply UIs explain the missing
dependency; no silent CPU approximation of a generative model.

## Consequences

AI-01 may proceed as Ready design/implementation against the proposal contract
without choosing a final commercial provider in this ADR. AI-05 remains blocked
on proving proposal/mask workflows and on the derived-asset publication details
above. Threat/privacy posture is fail-closed, user-initiated, and non-training
by default.

## Rejected alternatives

- Treating the existing chat panel as an inference authorization.
- Auto-running models on import or gallery idle.
- Writing AI results into adjacent XMP as a second authority.
- Auto-reject/delete based on aesthetic or duplicate scores.
- Requiring originals to be writable for generative workflows.
