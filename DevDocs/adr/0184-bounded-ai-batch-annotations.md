# ADR-0184: Bounded AI batch annotations and explicit readback

- Status: Accepted
- Date: 2026-10-12
- Extends: [ADR-0180](0180-revision-bound-catalog-annotations.md)

## Decision and reference assessment

The user requested current DaVinci Resolve research and immediate expansion of
Ravo's AI control. Blackmagic's official download catalog identifies 21.1.1
(2026-10-02); its 21.1 announcement admits external AI assistants for project
analysis, media organization, settings and batch rendering. Resolve 21 also adds
Photo workflows and neural search/retouch. These distinguish application control
from packaged inference: Ravo extends its existing shared control contract rather
than claiming a new model, semantic search or generative renderer.
Shared annotation discovery now declares the existing 4096-byte metadata and
512-byte keyword-path limits directly from Domain constants, replacing overly
wide advertised limits without changing accepted mutation values.

The useful adoption is bounded workflow composition: read explicit targets,
submit independently reviewed changes, preserve operation identity, and inspect
partial outcomes. Ravo does not admit arbitrary script execution, another state
store, or MCP-only business logic. The official 21.1 reference manual, chapter
201, page 4318, confirms a built-in MCP server, assistant setup and look/batch
workflows, including AI-created scripts. Ravo retains typed revision-bound
commands. Community adapters are separate implementations; their tool counts
do not describe the native server.

| Reference capability | Ravo adoption boundary |
| --- | --- |
| Photo/album grading, looks and LUTs | Existing recipe/mask/evaluate/commit owners; retain CPU/color correctness |
| AI media organization | This tranche adds explicit per-item review, captions/IPTC and keyword batches plus readback |
| Batch rendering/deliverables | Existing export services/CLI; MCP planning/execution remains an LC-03 acceptance gate |
| Neural content search and retouch | Requires real provider, weights/licence, resource/privacy and corpus admission; not implied by MCP |

Services owns a versioned annotations readback and batch use case over the
existing annotation transaction owners. Reads require an explicit catalog revision
and 1–32 unique asset IDs; they return bounded annotation states, with explicit
unavailable states when the aggregate projection budget is exceeded. A changed
revision rejects the read rather than mixing catalog generations.

Batch payloads contain 1–32 explicit unique asset IDs with per-item existing v1
annotation edits, within 64 KiB. Parse every edit and preflight every target before
the first write. Execute in request order, carrying each confirmed revision into
the next transaction. Transactions are per asset, not globally atomic. Stop on
the first cancellation, conflict, failure or unknown outcome; retain committed
items and mark undispatched items explicitly. Recovery warnings retain committed
status. Unknown commit outcomes must never be replayed with a new ID.
Batch diagnostics are limited to 4 KiB each and 16 warnings per item, with
explicit truncation flags; status, confirmed revisions and commit facts remain
unchanged. Truncated text retains UTF-8 boundaries.

Studio uses its existing executor, binding cancellation source and operation
receipts; selection/catalog/window changes cancel outstanding work and teardown
joins that owner. Single-asset annotation behavior stays unchanged. CLI is the
mandatory acceptance client; MCP only projects readback and typed review,
metadata and keyword batch tools. No dependency, database schema, image algorithm,
network listener, inference provider or fallback is added.

## Validation

Service contracts cover strict bounds, duplicate/missing targets, revision fences,
preflight neutrality, per-item readback, recovery warnings, partial cancellation,
competing writers, unknown outcomes and unchanged source/recipe/history. Actual
CLI/MCP subprocesses cover schemas, explicit target sets, receipts, replay,
stale bindings, cancellation and EOF/window destruction. Build affected targets,
run RavoCodeQuality and full functional contracts on available hosts; report
unavailable platforms, corpus/package and performance evidence separately.

## Research sources

- [Official download catalog](https://www.blackmagicdesign.com/api/support/us/downloads.json)
- [Resolve 21.1 announcement](https://www.blackmagicdesign.com/media/partial/release/20260908-03)
- [Resolve 21 features](https://www.blackmagicdesign.com/media/release/20260414-01)
- [Official reference manual](https://documents.blackmagicdesign.com/UserManuals/DaVinciResolveReferenceManual.pdf)
