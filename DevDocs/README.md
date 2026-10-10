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

## Current implementation entry points

The architecture and testing authorities cover:

- Catalog durability, explicit XMP interchange, source-safe Add/Copy/Move,
  staged import planning, duplicate detection and cancellation.
  Import success/failure becomes idle only after final Gallery publication.
- Gallery/Inspect/Develop ownership, preview scheduling, immutable CPU/GPU
  image publication, visible-thumbnail priority, bounded cache repair and shared
  display-cache accounting, query-wide Cull intersections, revision-bound complete
  selection snapshots, colour management, masks and revision-bound controls.
- Video metadata, FFmpeg decoding, SDR frame artifacts, Qt Multimedia playback,
  mapped frame colour ranges and selection-bound teardown.
- Settings navigation, per-user language and workspace layout, current-catalog
  automatic-backup drafts and the existing recovery task owner.
- Shared menus, shortcuts and controls; complete localisation; local Studio
  sessions and CLI acceptance without UI automation.
- Three-job development CI and five-architecture Release build/test/package
  jobs, with independent clean-package startup gates and no release Debug stage.
  Compiler-cache checks share the restored directory with the whole job;
  Windows packaging preserves OS DLL ownership and builds validated DXBC shaders.

For video scope, begin with
[ADR-0165](adr/0165-video-library-and-playback.md). Explicit Qt-kit runtime
resolution and Linux PipeWire/VA-API requirements belong to
[Dependency Workflow](Dependency_Workflow.md) and [Packaging](Packaging.md).

For source-safe Lightroom Classic import, begin with
[ADR-0164](adr/0164-lightroom-catalog-reader.md) and
[ADR-0166](adr/0166-lightroom-data-preservation-and-develop.md).
[ADR-0167](adr/0167-foreign-conversion-checkpoints.md) specifies durable receipts,
atomic business commit proofs and conservative resume with member reconciliation. Remaining
RAWmakase integration gates are in [TODO_LIGHTROOM_IMPORT.md](TODO_LIGHTROOM_IMPORT.md).
For mask-scoped Develop,
begin with [ADR-0158](adr/0158-mask-scoped-develop-workspace.md).

## Execution documents

| Document | Unfinished work |
| --- | --- |
| [TODO.md](TODO.md) | Ordered product, correctness, performance and release gates |
| [TODO_ARCHITECTURE_REFACTOR.md](TODO_ARCHITECTURE_REFACTOR.md) | Remaining ownership/refactor qualification |
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
