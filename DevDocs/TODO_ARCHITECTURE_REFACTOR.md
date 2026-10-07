# Architecture refactor residuals

This queue is part of the existing correctness/performance stream in [TODO.md](TODO.md).
Current owners and invariants belong in [ARCHITECTURE.md](ARCHITECTURE.md);
validation policy belongs in [TESTING.md](TESTING.md). No schema, rendering
algorithm, public JSON, dependency or new product capability change is admitted.

## Remaining ownership migrations

- RF-04: move remaining Import page/progress/batch orchestration state
  from the root presenter into its specific C++ owner. Preserve
  the single selection authority, command registry, draft/options distinction,
  debounce policy, text focus and worker shutdown order. Library paging/model
  ownership still needs an explicit boundary review before moving it.
- RF-05: transfer remaining viewport/frame/ROI/GPU presentation ownership to Inspect.
  Keep current/saved edit state separate from observed displayed Recipe and scopes.
  Complete the image-provider and command/control hard cut to that read-only
  presenter. Preserve live-control revision guards, interactive progress under
  dragging, GPU owned-pixel handoff and baked-proxy semantics; remove root
  properties, fields, signals and old write entrances as each group switches.
- RF-07/08: audit error display/retry call sites and remaining request/result
  dependencies as those workflows move. Deduplicate only identical mappings;
  preserve committed-state context and all existing public type/version fields.
- RF-10: inspect remaining test-helper repetition during each migration.
  Share construction/injection helpers only where semantics match; retain
  business wiring, publication-failure windows and ordered test inventories.

## Qualification gaps and acceptance gates

- RF-00/11: retain independent baseline/candidate Release samples and measure
  browse/filter/paging, edit-to-visible latency, thumbnail demand, queue bounds,
  thread ownership, RSS and close/join latency under the same workload. Use
  baseline `7656ba368732755f94ccd7c2b529e6fa38adb16e`; do not accept pixel or
  performance changes by regenerating expected results.
- Full suites, performance/thermal stress and package qualification are deferred
  by the user's current validation constraint. Prioritize ownership migrations,
  single-job affected-target compilation, static gates and cheap focused checks.
  Do not enlarge timing windows or weaken pixel assertions to hide host load.
- Resolve the baseline Windows Release CI failures in the four
  `StudioLibraryPaging` tests before claiming a green platform baseline.
- Run feasible sanitizers and full Debug/Release tests on the candidate; run
  matching Windows/Linux builds, installed-package create/import/probe/reopen
  smoke, backup/restore/relocate and real corpus/ICC/GPU comparisons. Missing
  toolchains, private corpus or hardware remain qualification gaps, not passes.
- RF-11: search for old central methods, root state, signals, back-pointers,
  transitional adapters and document paths after the remaining migrations;
  verify one command path, one state writer and one resource owner per lane.
- Remove this TODO only after the remaining ownership migrations and evidence
  gates are actually satisfied; retain durable conclusions in their authorities.

```text
python3 configs/source_roots.py verify
python3 Ravo/tools/freeze_legacy_manifest.py --check
python3 Ravo/tools/check_ravo_dependency_boundary.py
cmake --preset mac_clang_debug -DBUILD_TESTING=ON
cmake --build --preset mac_clang_debug
cmake --build --preset mac_clang_debug --target RavoCodeQuality
ctest --test-dir build/mac_clang_debug --output-on-failure
```

Run CTest discovery and execution serially: concurrent discovery can write the
same generated GoogleTest registration files. Register the original enabled
identities/labels separately from tests actually run and skipped.
