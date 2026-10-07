# Architecture refactor residuals

This queue is part of the existing correctness/performance stream in [TODO.md](TODO.md).
Current owners and invariants belong in [ARCHITECTURE.md](ARCHITECTURE.md);
validation policy belongs in [TESTING.md](TESTING.md). No schema, rendering
algorithm, public JSON, dependency or new product capability change is admitted.

## Source-plan reconciliation input

- Reconcile the source RF-00..RF-11 / B00..B20 plan against committed `main`,
  the ownership contracts and this residual queue without reopening completed
  migrations. The source wiki currently redirects to authentication and no
  callable authenticated document/browser reader is available. Obtain a readable
  source-plan export or authorized read interface before claiming that full
  source-plan reconciliation is verified. This input dependency is separate from
  the external qualification waivers below; available local code/test evidence
  does not prove the contents of an unread plan.

## Qualification gaps and acceptance gates

The 2026-10-08 authorization waives waiting for the external gates below during
the current local refactor tranche. They remain **Waived / External gap /
Untested**, never Passed: Windows/Linux toolchains and denied Windows CI logs;
private corpus, ICC/GPU, thermal, native-display and physical hardware evidence;
desktop TSan without instrumented Qt; and installed-package acceptance requiring
signing authorization or a real deployment environment. Local completion does
not close these long-term gates or authorize removal of this queue.

- RF-00/11: extend the retained independent fixture/synthetic Release samples
  to representative corpus and thermal qualification. Measure browse/filter/
  paging, edit-to-visible latency, thumbnail demand, queue bounds, thread
  ownership, RSS and close/join latency under the same workload. Keep baseline
  `7656ba368732755f94ccd7c2b529e6fa38adb16e`; do not admit pixel or performance
  changes by regenerating expected results. Offscreen publication and process
  thread/RSS observations do not qualify native frame presentation or private
  corpus behavior.
- For any qualification-driven repair, run serial affected-target builds,
  static gates and focused checks; repeat full applicable suites when the
  shared-surface/lifecycle change requires them. Do not enlarge timing windows
  or weaken pixel assertions to hide host load.
- Resolve the baseline Windows Release CI failures in the four
  `StudioLibraryPaging` tests before claiming a green platform baseline.
  Baseline CI run `37586731106` remains failed; job metadata is readable but
  run and individual-job log retrieval return HTTP 403 with the current account.
  Obtain runner diagnostics and a matching Windows toolchain before accepting
  a proposed repair as platform evidence.
- Desktop TSan remains unqualified: queued-handoff reports reproduce with a
  Qt-only payload. Obtain a matching instrumented Qt SDK and rerun the
  unsuppressed Import/Inspect/command/lifecycle gates; joined executor/service
  checks and ASan lifetime evidence do not qualify desktop synchronization.
  Do not suppress callbacks or skip the mandatory Studio smoke to get a build.
- Run matching Windows/Linux builds and installed-package create/import/probe/
  reopen/backup/restore/relocate smoke against the actual deployed runtimes.
  Plain CMake staging does not deploy Qt frameworks and cannot satisfy this gate.
  The macOS deployment owner signs its payload; signing, release and publication
  require separate explicit authorization.
- Qualify real corpus/ICC/GPU comparisons and native display/hardware behavior.
  Missing toolchains, private corpus or hardware remain gaps, not passes.
- Remove this TODO only after these evidence gates are actually satisfied;
  retain durable conclusions in their existing authorities.

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
