# ADR-0181: Retire unused dependencies and product stubs

- Status: Accepted
- Date: 2026-10-12
- Supersedes: stub-only scope of ADR-0121/0141/0142/0153 and unused migration-root admission

## Decision

The user requested removal of the code burdens identified by the code inventory.
Retire unused RawSpeed/LensFun direct dependency preparation, unimplemented DNG
conversion/Smart Preview/tethering probe surfaces, and deterministic local AI
Proposal/Suggestion production providers and their callers. Retired commands are
removed, not redirected or represented by compatibility switches. Real DNG decode,
offline edit proxies, external assistant and versioned CLI/MCP grading remain.

Services retain persistent business ownership, recipe retains parameter semantics,
desktop retains ephemeral state, and transport adapters remain thin. Removal adds
no executor, thread, dependency, schema, renderer or fallback. In-flight ordinary
tasks retain their existing cancellation/join lifecycle. Stub proposal/suggestion
JSON side directories are left untouched and no longer interpreted or produced.
Edits previously accepted through them are ordinary recipe/metadata and remain
readable without the retired provider. Removal never deletes existing side files.

Heavy workflow smoke belongs to a test-only composition of the same Studio owners.
Shipping startup/video acceptance remains supported. Repeated ordinary field
plumbing should share a recipe-owned descriptor source; coupled algorithms,
multi-instance and mask invariants stay explicit. Generation, if used, has one
owning script and is never manually patched.

The initial descriptor covers 43 uncoupled scalar assignments and 24 individual
resets. Coupled group resets remain explicit. The previous half-step schema probe
mislabelled `lensTcaR/B` as integers fixed at one; descriptors report the existing
continuous 0.9–1.1 acceptance instead. No render/parameter algorithm changes.
Import-only historical types/functions use a dedicated `legacy_develop.h` surface;
ordinary recipe serialization/identity helpers stay in `develop_recipe_values.cpp`.

Workflow smoke shares `ravo_studio_resources` with shipping Studio and is compiled
only with BUILD_TESTING. Its assertions are moved intact. Test inventory changes
remove only retired provider/absent-encoder cases and add a command-rejection
contract; remaining ordered identities and target membership are preserved.

Accepted historical XMP input and existing saved operation rendering remain
supported. Catalog schema migrations, transaction guards, atomic publication,
source protection and recovery are durability obligations, not removal candidates.
Historical ledger descriptions must not claim deleted old application consumers
are still awaiting retirement. No frozen image/recipe gold is changed.

## Validation

Each removal updates target membership, current capability/help, architecture,
testing and documentation links. Verify retired commands fail before catalog
mutation, source-root update remains offline and retains unrelated pins/settings,
and real RAW/DNG, proxy, import, metadata, recipe/history, recovery and MCP contracts
continue to pass. Broad public/header changes require the full functional suite
and code-size/dependency-boundary checks. Platform/package and timing evidence are
reported only when actually run.
