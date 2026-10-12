# ADR-0183: Document authorities and runtime test coverage

- Status: Accepted
- Date: 2026-10-12

## Decision

The user requested repository documentation and redundant-test cleanup, explicitly
choosing removal of static text and duplicate UI checks while retaining critical
runtime workflows. Remove tests that only read QML source and search for labels,
object names, property spelling or control order. Mixed tests retain their actual
presenter, command and window assertions. Remove unused source-reading helpers
and compile definitions. Architectural dependency checks remain enforced.

Runtime coverage stays with existing owners: service tests prove transactions,
source safety, history, cancellation and recovery; command/CLI/MCP tests prove
revision-bound intents and structured results; Qt component/window tests and the
test-only Studio workflow smoke prove keyboard, focus, crop/mask gestures,
dialogs and composed QML loading. No renderer, schema, task, thread boundary,
timeout, pixel oracle or fallback changes. Existing task cancellation, late-result
rejection and shutdown/join coverage remains enabled.

Update the split inventory only for the explicitly retired source-only cases;
retain every other ordered identity, enabled state and target membership. File
renames are hard cuts. This is not permission to rebaseline arbitrary test loss.

Documentation keeps one authority per topic as listed in `../README.md`. Remove
the obsolete root review and superseded stub-only ADRs; keep AI privacy/provenance
policy and backup v3 compatibility in their existing durable authorities. Merge
architecture-refactor qualification gaps into TODO.md rather than retaining a
parallel queue. Missing corpus, hardware, instrumented Qt, deployed packages and
Windows/Linux evidence remains unfinished, never implicitly passed by cleanup.
Per-operation numerical truth remains in code, independent tests, frozen fixtures
and the migration ledger/operation ADRs, rather than a second test-policy census.

## Validation

Check every remaining case against the pre-cleanup ordered inventory minus the
reviewed retirement set, and verify CMake target membership. Run offline source
root update, configure, build the affected test targets and RavoCodeQuality, then
the functional suite including workflow/localization smoke. Check repository
document paths, anchors, retired references and `git diff --check`. Performance
qualification remains separate and may wait for an idle host. Report platform
and package checks only when actually run.
