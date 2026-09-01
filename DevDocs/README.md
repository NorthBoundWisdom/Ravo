# Ravo developer documentation

Repository-owned development, architecture, execution, compliance, and legacy
evidence documents are centralized here. `README.md` files remain beside the
audience or component they introduce, and `AGENTS.md` files remain at their
scope roots for tool discovery.

The following are separate owners and are not folded into `DevDocs/`:

- `userdoc/`: publishable user handbook;
- `FreeCM/`: independent submodule;
- `.codex/skills/`: executable agent workflows.

## Current authorities

| Document | Authority |
| --- | --- |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Target boundaries, ownership, lifecycle, threads, data, and failure behavior |
| [MIGRATION.md](MIGRATION.md) | Accepted capabilities, leftovers, and retirement rules |
| [TESTING.md](TESTING.md) | Test layers, fixtures, deterministic contracts, performance probes, and validation depth |
| [TODO_PHOTO_MANAGEMENT.md](TODO_PHOTO_MANAGEMENT.md) | Remaining P0/P1 platform, corpus, and interactive-latency release evidence |
| [TODO_GALLERY_PERFORMANCE.md](TODO_GALLERY_PERFORMANCE.md) | Release measurements and gated Gallery concurrency/cache candidates |
| [TODO_PRO_WORKFLOW.md](TODO_PRO_WORKFLOW.md) | Ranked Lightroom/Capture One user-outcome gaps; not independently ready |
| [ProductRoadmap.md](ProductRoadmap.md) | Cross-layer product decisions not ready for a TODO |
| [adr/README.md](adr/README.md) | Accepted architecture decisions and supersession history |

## Operations and compliance

| Document | Scope |
| --- | --- |
| [Dependency_Workflow.md](Dependency_Workflow.md) | FreeCM source roots, local integration, refresh, and publication order |
| [Packaging.md](Packaging.md) | Release packaging ownership, artifacts, and validation |
| [GPU_Baseline.md](GPU_Baseline.md) | Ravo-only GPU correctness and performance admission gates |
| [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) | Third-party attribution and license notices packaged with Ravo |

## Maintenance rules

1. Keep one authority per topic. ADRs retain durable decisions; current facts
   belong in architecture, migration, testing, code, or tests.
2. TODOs contain only unfinished work, dependencies, risks, validation commands,
   and acceptance gates. Delete a completed TODO after moving durable facts.
3. Remove obsolete plans, historical run diaries, and concept mockups instead of
   archiving competing descriptions.
4. Update every tracked reference in the same change when a document moves or is
   renamed. Do not leave redirects or compatibility copies.
5. Keep transient reports and private measurements outside the repository.
6. Update generated evidence only through its owning script.
