# Ravo developer documentation

`DevDocs/` is the repository-owned source for architecture, product planning,
validation, dependency, packaging, compliance, and historical migration
records. Component `README.md` files remain beside the code they describe, and
`AGENTS.md` files remain at their scope roots for tool discovery.

Ravo's product north star is a professional, cross-platform photo manager and
non-destructive editor for working photographers, with optional AI-assisted
culling, retouching, and colour work that remains reviewable, reversible,
private by default, and reproducible enough to audit.

## Document authority

| Document | Owns | Does not own |
| --- | --- | --- |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Current target boundaries, image-pipeline defaults, ownership, lifecycle, threads, data, and failure behavior | Future work or run diaries |
| [MIGRATION.md](MIGRATION.md) | Accepted capability history, removed leftovers, and retirement decisions | Product backlog |
| [ProductRoadmap.md](ProductRoadmap.md) | Outcome order, product principles, and cross-layer decisions not ready for execution | Task-level status |
| [TODO.md](TODO.md) | Product execution queue: corpus/latency, Gallery evidence, professional workflow, and AI | Completed behavior, durable decisions, or package closeout |
| [TESTING.md](TESTING.md) | Test layers, fixtures, deterministic contracts, performance probes, and validation depth | Product priority |
| [adr/README.md](adr/README.md) | Accepted architecture decisions and supersession history | Mutable implementation status |

Product execution belongs only in [TODO.md](TODO.md).
Three-platform package evidence belongs in [Packaging.md](Packaging.md).

## Operations and compliance

| Document | Scope |
| --- | --- |
| [Dependency_Workflow.md](Dependency_Workflow.md) | FreeCM source roots, local integration, refresh, and publication order |
| [Packaging.md](Packaging.md) | Release packaging ownership, artifacts, and validation |
| [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) | Generated third-party attribution and licence notices packaged with Ravo |

The following remain separate owners and are not folded into `DevDocs/`:

- `Ravo/README.md`: current user-visible and machine-visible capability baseline;
- `userdoc/`: publishable user handbook;
- `FreeCM/`: independent submodule;
- `.codex/skills/`: executable agent workflows.

## Planning flow

A product idea moves through one direction only:

```text
ProductRoadmap -> dated ADR -> TODO -> code/tests -> current authorities
```

1. Keep an undecided cross-layer capability in `ProductRoadmap.md`.
2. Before implementation, accept a dated ADR that names the user outcome,
   owner, persisted or machine contract, cancellation/failure behavior,
   privacy and security constraints where relevant, and validation gate.
3. Add only the unfinished execution slice to `TODO.md`.
4. On completion, move durable facts to code, tests, `Ravo/README.md`,
   `ARCHITECTURE.md`, or `TESTING.md`, then delete the completed TODO item.
5. Record removed or explicitly rejected legacy behavior in `MIGRATION.md`.

## Maintenance rules

1. Keep one authority per topic. Do not duplicate current behavior across the
   roadmap, TODO, architecture, and migration documents.
2. TODO entries contain only unfinished work, dependencies, risks, concrete
   validation, and acceptance gates. They do not contain completed checklists.
3. Do not use target dates as a substitute for evidence. Priorities are ordered
   by user outcome, dependency, and release risk.
4. Remove obsolete plans, historical run diaries, and concept mockups instead
   of archiving competing descriptions.
5. Keep transient reports, private-corpus results, screenshots, and machine-
   local measurements outside the repository unless a stable test fixture or
   generated evidence owner explicitly requires them.
6. Update generated output, including third-party notices, only through its
   owning script.
7. For documentation-only changes, verify real paths, relative links, commands,
   terminology, and whitespace; do not claim an unrun platform check passed.
