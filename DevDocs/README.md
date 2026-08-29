# Ravo developer notes

Live documentation for the Ravo build and GPU work. Frozen Darktable 0.9 source
maps live in [`legacy/docs/`](../legacy/docs/).

| File | Description |
| --- | --- |
| [Dependency_Workflow.md](Dependency_Workflow.md) | FreeCM lock modes, `--update`, and publication order |
| [Packaging.md](Packaging.md) | FreeCM release packaging, platform artifacts, and CI ownership |
| [GPU_Baseline.md](GPU_Baseline.md) | CPU gold samples and later GPU performance gates |
| [ProductRoadmap.md](ProductRoadmap.md) | Deferred cross-layer capabilities after the accepted mask graph, Studio overlay/group/path/brush surface, and Color Harmonizer retirement |
| [phase0/README.md](phase0/README.md) | Historical frozen-fixture evidence, contracts, and decision records |
| [concepts/p0-library-grid.svg](concepts/p0-library-grid.svg) | Ravo Studio concept-reference artwork |
| [../hooks/README.md](../hooks/README.md) | Host installer for FreeCM commit-time formatting |
| [../Ravo/README.md](../Ravo/README.md) | Engine, CLI, Studio, and the current product slice including Color Reconstruction/Zones/Monochrome, source-exact Lab sharpening, Retouch, Dehaze, Canvas, Output Frame/Dither, and deterministic text Watermark |
| [../Ravo/ARCHITECTURE.md](../Ravo/ARCHITECTURE.md) | Target, ownership, lifecycle, metadata-analysis, command, data, and engine-private algorithm boundaries |
| [../Ravo/MIGRATION.md](../Ravo/MIGRATION.md) | Legacy migration policy, ledger, and leftover boundary |
| [../Ravo/TESTING.md](../Ravo/TESTING.md) | Test ownership, fixtures, and validation contracts |
| [../Ravo/docs/adr/README.md](../Ravo/docs/adr/README.md) | Architecture decisions and supersession relationships |
| [../.codex/skills/i18n-translation-workflow/SKILL.md](../.codex/skills/i18n-translation-workflow/SKILL.md) | Ravo Studio source extraction, Chinese translation memory, and catalog workflow |
| [../TODO_LEGACY_MIGRATION.md](../TODO_LEGACY_MIGRATION.md) | Unfinished legacy migration execution |
| [../legacy/README.md](../legacy/README.md) | Frozen 0.9 reference tree |

## Maintenance rules

1. Describe current ownership, invariants, failure behavior and reproducible
   validation commands. Do not append implementation diaries, completed
   checklists or per-run samples.
2. Keep one authority per topic. Fold durable conclusions into that document
   and delete superseded designs instead of preserving competing summaries.
3. Treat code, manifests, checkers and repository workflows as executable
   truth. Update the owning document in the same change when their contract
   changes.
4. Keep transient measurements and reports under `build/` or `/tmp`; document
   only stable metric definitions and hard gates.
5. Update generated documentation only through its owning generator.
6. Put unfinished execution work in a root `TODO_<TOPIC>.md`. File-local
   refactoring notes stay with the implementation, not in a repository-wide
   backlog.
