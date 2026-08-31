# Ravo developer documentation

Repository-owned development, architecture, execution, compliance, and legacy
evidence documents are centralized here. `README.md` files remain beside the
audience or component they introduce, and `AGENTS.md` files remain at their
scope roots for tool discovery.

The following are separate owners and are not folded into `DevDocs/`:

- `userdoc/`: publishable user handbook;
- `legacy/`: frozen darktable source and static reference notes;
- `FreeCM/`: independent submodule;
- `.codex/skills/`: executable agent workflows.

## Current authorities

| Document | Authority |
| --- | --- |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Target boundaries, ownership, lifecycle, threads, data, and failure behavior |
| [MIGRATION.md](MIGRATION.md) | Ravo/legacy boundary, accepted capabilities, leftovers, and retirement rules |
| [TESTING.md](TESTING.md) | Test layers, fixtures, deterministic contracts, and validation depth |
| [TODO_PHOTO_MANAGEMENT.md](TODO_PHOTO_MANAGEMENT.md) | Remaining P0/P1 release evidence |
| [TODO_LEGACY_MIGRATION.md](TODO_LEGACY_MIGRATION.md) | Paused legacy absorption and retirement execution queue |
| [TODO_GALLERY_PERFORMANCE.md](TODO_GALLERY_PERFORMANCE.md) | Release measurements and gated Gallery concurrency/cache candidates |
| [ProductRoadmap.md](ProductRoadmap.md) | Cross-layer product decisions not ready for either TODO |
| [adr/README.md](adr/README.md) | Accepted architecture decisions and supersession history |

## Operations and compliance

| Document | Scope |
| --- | --- |
| [Dependency_Workflow.md](Dependency_Workflow.md) | FreeCM source roots, local integration, refresh, and publication order |
| [Packaging.md](Packaging.md) | Release packaging ownership, artifacts, and validation |
| [GPU_Baseline.md](GPU_Baseline.md) | Ravo-only GPU correctness and performance admission gates |
| [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) | Third-party attribution and license notices packaged with Ravo |

## Frozen legacy evidence

| Document | Scope |
| --- | --- |
| [legacy/Baseline.md](legacy/Baseline.md) | Frozen commit/tree identities and live-oracle limitation |
| [legacy/CapabilityInventory.md](legacy/CapabilityInventory.md) | Machine-checked remaining IOP and fixture census |
| `legacy/retired-src-paths.txt` | Freeze-check exclusions for accepted retired source owners |
| `legacy/retired-host-data-paths.txt` | Freeze-check exclusions for accepted retired host-data owners |

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
