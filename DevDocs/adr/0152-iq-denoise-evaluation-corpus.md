# ADR-0152: IQ-01 evaluation corpus contract and CPU denoise probe

- Status: Accepted
- Date: 2026-09-04
- Relates: IQ-01 in [TODO.md](../TODO.md)
- Extends: [ADR-0151](0151-iq-cpu-gpu-consistency-gate.md)
- Aligns with: Engine CPU gold for durable pixels; no learned-model admission

## Context

IQ-01 requires camera/profile and denoise quality admission evidence, not decode
alone. ADR-0151 froze CPU gold for persist/export. IQ-01 still needs:

- a dated **evaluation corpus contract** (licensed redistributable set plus
  private-camera extension) with provenance fields;
- a **denoise evaluation probe** owned by Engine CPU reference paths
  (`ravo.detail.denoiseprofile` / existing raw denoise), never by Studio QML
  and never by an undeclared learned runtime;
- fail-closed behavior when the corpus is absent so release claims cannot
  silently skip image-quality evidence;
- explicit rejection of surprise GPL/runtime/weight dependencies in this
  first Ready.

Full corpus matrix, blinded human review, and a first real learned denoise
provider remain residual.

## Decision

### Corpus contract (`ravo.iq.evaluation-corpus/v1`)

An evaluation corpus root contains `manifest.json` with at least:

- `schema` = `ravo.iq.evaluation-corpus/v1`, `schema_version` = 1
- `corpus_id`, `license`, `notice_path` (relative)
- `cases[]` entries with `case_id`, `kind`
  (`denoise_fixture` | `camera_profile_fixture`), optional
  `relative_path`, and provenance fields (`camera_make`, `camera_model`,
  `iso`, `illuminant`, `notes`)

Missing root, missing/invalid manifest, or empty `cases` is
`iq_corpus_unavailable` (or `iq_corpus_invalid`) — fail-closed. Private
extension sets may live beside the redistributable root but are never
required for the first Ready harness to compile.

### Denoise probe ownership (CPU reference)

Engine owns `evaluate_denoise_cpu_reference`:

1. Resolve corpus (explicit path or `RAVO_IQ_CORPUS_ROOT`).
2. Select a `denoise_fixture` case (or reject when none).
3. Run the deterministic CPU profile-denoise path with a documented strength
   on a fixture-backed working buffer (synthetic fixture allowed when the case
   marks `synthetic: true`).
4. Report machine-visible metrics (mean absolute delta vs undenoised CPU gold,
   finite checks, recipe/operation ids). GPU interactive path is out of scope.

Camera profile quality probe ownership shares the same corpus resolver and
fail-closed policy; first Ready may exercise noise-calibration document
presence/hash without claiming colour accuracy closure.

### Explicit non-goals (this ADR)

- Admitting ONNX/CoreML/TensorRT or any learned denoise weights
- Redistributing a full licensed photo corpus in-tree
- Claiming camera support from decode-only tests
- Silent fallback to “no denoise” when corpus is missing

## First Ready

- Publish contract constants under `ravo.iq.evaluation-corpus/v1` and
  `ravo.iq.denoise-evaluation/v1`.
- Contract tests: missing corpus fail-closes; fixture corpus with one
  synthetic denoise case returns a structured CPU evaluation report.
- No new GPL or proprietary runtime dependencies.

## Consequences

IQ-01 gains a testable admission gate scaffold. Expanding the corpus and
metrics, and admitting a real denoise provider, require further dated ADRs.

## C2 Ready

Expand the fixture evaluation workflow so a photographer-facing **support claim**
is backed by deterministic Engine evidence (not decode-only):

- `evaluate_iq_fixture_support` bundles CPU denoise + camera-profile probes with
  explicit `support_claim_status=fixture_evidence_ready`,
  `camera_product_support_claimed=false`, `learned_denoise_admitted=false`,
  and `cpu_gold_aligned` via ADR-0151 `require_cpu_gold_backend`.
- Denoise report adds `max_abs_delta` and claim/residual flags; camera-profile
  probe hashes present documents (SHA-256) and surfaces provenance fields.
- CLI: `ravo iq evaluate [--corpus <root>] [--strength <0..1>]` fail-closes with
  `iq_corpus_unavailable` when corpus is unset/missing.
- Studio (thin, no Main.qml growth): `iqQualityPolicy` + `evaluateIqQuality`
  expose the same contract and fail-closed result map.
- In-tree synthetic corpus: `Ravo/tests/fixtures/iq_evaluation_corpus/`.

### Residual (C3)

- Licensed redistributable + private real-photo corpus with blinded human review.
- Product camera-support certification from that corpus.
- Any learned denoise runtime/weights admission (blocked; requires AI-00 + dated ADR).

