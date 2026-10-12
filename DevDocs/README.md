# Ravo developer documentation

Start with the [product overview](../README.md) for the photographer-facing
workflow, the [user handbook](../userdoc/README.md) for task instructions, and
the [capability reference](../Ravo/README.md) for the current source baseline,
build commands and CLI.

This directory holds engineering authorities and unfinished execution work.
It does not duplicate the handbook or serve as a chronological change log.

## Document authority

| Question | Authority |
| --- | --- |
| What can the current product do? | [Ravo/README.md](../Ravo/README.md) |
| Where does state live, and who owns work and resources? | [ARCHITECTURE.md](ARCHITECTURE.md) |
| Which historical behavior is accepted, removed or unsupported? | [MIGRATION.md](MIGRATION.md) |
| What establishes correctness and regression coverage? | [TESTING.md](TESTING.md) |
| How are dependencies resolved and published? | [Dependency_Workflow.md](Dependency_Workflow.md) |
| How are packages built, deployed and qualified? | [Packaging.md](Packaging.md) |
| Which licences and notices ship? | [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) |
| What design decisions constrain implementation? | [ADR index](adr/README.md) |
| What should happen next? | [TODO.md](TODO.md) |
| Which broader product questions remain undecided? | [ProductRoadmap.md](ProductRoadmap.md) |

## Implementation entry points

Use the architecture and test authorities above for current owners and coverage.
These decisions explain the larger workflows without duplicating their state here:

- [Local adjustments](adr/0158-mask-scoped-develop-workspace.md) and
  [HDR/Panorama](adr/0163-hdr-panorama-derived-assets.md).
- [Lightroom import](adr/0164-lightroom-catalog-reader.md),
  [data preservation](adr/0166-lightroom-data-preservation-and-develop.md) and
  [conversion checkpoints](adr/0167-foreign-conversion-checkpoints.md).
- [Video](adr/0165-video-library-and-playback.md); host/runtime requirements
  belong in Dependency Workflow and Packaging.
- [AI control/MCP](adr/0168-ai-control-and-mcp.md),
  [usage and extension guide](../Ravo/README.md#ai-grading-and-mcp),
  [machine ownership](ARCHITECTURE.md#machine-automation) and
  [contract validation](TESTING.md#ai-grading-and-mcp-contracts).
- [Interactive performance protocol](TESTING.md#interactive-performance-comparison).
- [Shared Studio resources, QML module output and Windows smoke deployment ordering](ARCHITECTURE.md).
- [Settings dialog and existing preference/backup owners](ARCHITECTURE.md#catalog-recovery-and-backup).
- [Code retirement](adr/0181-retire-unused-dependencies-and-product-stubs.md),
  [nested field semantics](adr/0182-declared-nested-fields-and-contextual-ranges.md)
  and [documentation/test scope](adr/0183-document-authorities-and-runtime-test-coverage.md).
- [Explicit AI batch annotations and readback](adr/0184-bounded-ai-batch-annotations.md),
  including the dated Resolve reference assessment and existing-owner boundaries.
- [Explicit JPEG EXIF colour and preview failure completion](adr/0185-explicit-jpeg-exif-colour-and-preview-failure.md).

## Execution documents

| Document | Unfinished work |
| --- | --- |
| [TODO.md](TODO.md) | Ordered product, correctness, performance and release gates |
| [TODO_CODE_RETIREMENT.md](TODO_CODE_RETIREMENT.md) | Remaining parameter projection consolidation and resource-linkage host/package qualification |
| [TODO_LIGHTCRAFT_ADOPTION.md](TODO_LIGHTCRAFT_ADOPTION.md) | Command/MCP, model, mask, camera/lens and workflow adoption; performance qualification in idle-CPU windows |
| [TODO_RAW_RENDERING_PROFILES.md](TODO_RAW_RENDERING_PROFILES.md) | RAW rendering/profile qualification |
| [TODO_VIDEO_SUPPORT.md](TODO_VIDEO_SUPPORT.md) | Video host, audio, corpus and package qualification |
| [TODO_LIGHTROOM_IMPORT.md](TODO_LIGHTROOM_IMPORT.md) | Lightroom data, Develop, offline identity and corpus qualification |
| [ProductRoadmap.md](ProductRoadmap.md) | Product-level questions requiring a bounded decision |

A green build is not a substitute for the corpus, platform or package evidence
required by a specific gate. Do not mark unrun or conditionally skipped
qualification as passed.

## Operations and compliance

The source-root workflow keeps ignored active locks and managed checkouts
separate from the published pinned template. Dependency repositories are
published and verified before the parent records a new SHA.

FFmpeg remains pinned to the Qt kit's admitted 7.1.5 release during dependency
refreshes; its source-root workflow and runtime constraints are owned by
[Dependency Workflow](Dependency_Workflow.md).

The package graph produces macOS ARM64/Intel DMGs, Windows x86_64 ZIP and
Linux x86_64/ARM64 AppImage/DEB packages. Tagged release publication requires the
same successful source SHA and clean-runner final-package startup. Runtime,
signing and host requirements remain in [Packaging.md](Packaging.md).

Repository constraints are in [AGENTS.md](../AGENTS.md) and
[Ravo/AGENTS.md](../Ravo/AGENTS.md). Agent skills live under `.codex/skills/`
and `.grok/skills/`; FreeCM is an independent submodule.

## Planning flow

```text
Product question → dated ADR → unfinished TODO → implementation and tests
                 → current capability/architecture/testing authorities
```

Keep undecided cross-layer capabilities in the roadmap. An accepted decision
names ownership, persistence, lifecycle, failure/cancellation, resource and
privacy boundaries, and a validation gate before implementation starts.

On completion, move durable facts into code, tests and the relevant authority;
remove completed execution items. Record deliberately removed or unsupported
historical behavior in the migration ledger.

## Maintenance rules

- Keep one stable authority for each topic; link to it rather than copying a
  second version of its contracts.
- Write task instructions in the handbook and product introductions in the
  root README. Keep implementation detail out of ordinary user instructions.
- TODOs contain unfinished work, risks, dependencies and acceptance gates.
  Avoid completed checklists, run diaries and transient measurements.
- Rename or delete documents with all tracked references updated in the same
  change. Do not leave redirects or duplicate old-name copies.
- Update generated material through its owning generator, including
  translations and build/package templates.
- Keep private reports, source media, local paths, active locks, presets and
  build output out of tracked documentation.
- Validate links, commands, facts and whitespace; report unavailable platforms
  and missing evidence explicitly.
