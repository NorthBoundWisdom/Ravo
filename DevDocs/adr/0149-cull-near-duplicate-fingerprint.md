# ADR-0149: Bounded near-duplicate fingerprint assistance

- Status: Accepted
- Date: 2026-09-04
- Relates: CULL-01 in [TODO.md](../TODO.md)
- Extends: [ADR-0147](0147-cull-exact-duplicate-and-burst-proposals.md)
- Does not supersede ADR-0147 exact-byte groups or ADR-0142 AI stub cues.

## Context

CULL-01 residuals include perceptual / near-duplicate assistance without AI
weights or embeddings. Exact SHA-256 groups (ADR-0147) miss visually identical
re-encodes and slight crops. A first Ready must stay deterministic, bounded,
and never auto-delete.

## Decision

### Fingerprint (`ravo.cull.near-duplicate/v1`)

- Decode each listed asset's original to an sRGB raster (Qt-backed load for
  JPEG/PNG/TIFF; RAW and undecodable paths are skipped with structured reasons).
- Compute a **64-bit average hash (aHash)**: scale to 8×8 grayscale, compare each
  pixel to the block mean, pack bits MSB-first into a hex fingerprint.
- Group members whose fingerprints are within a caller-bounded Hamming distance
  (default **5**; CLI `--near-dup-max-hamming`). Identical fingerprints are the
  primary signal; distance ≤ threshold merges via union-find.
- Cap emitted groups (`max_groups`, default 200) and never mutate the catalog.
- **No auto-delete, auto-reject, or stack.** Exact-byte duplicates remain ADR-0147.

### Non-goals

- Learned embeddings / AI similarity (ADR-0142 / AI-04).
- Keyboard Pick/Reject chrome (separate residual).
- Treating near-dup groups as delete authority.

## Consequences

CULL-01 gains a testable near-duplicate report that deepens review assistance
without packaging model weights or inventing delete semantics.

## Rejected alternatives

- Embedding-only near-dup as the first Ready.
- Auto-stacking or auto-rejecting near-dup members.
- Using `content_fingerprint` (size+mtime) as visual similarity.
