---
name: build-repo
description: Configure, build, test, run, or diagnose the Ravo C++20 project using its FreeCM-managed source roots and host tooling. Use for dependency materialization, build failures, validation after code changes, test selection, or choosing a Debug/Release configuration.
---

# Build Ravo

Use the active FreeCM workspace and generated CMake presets. Keep dependency preparation,
configuration, compilation, testing, and runtime diagnosis as separate observable steps.

## Establish the workspace

1. Read the root `AGENTS.md` and `README.md`.
2. Run `git branch --show-current` and `git status --short --branch`; preserve existing changes.
3. Read the ignored active `source_roots.lock.jsonc`, then run:

   ```sh
   python3 configs/source_roots.py show --format json
   python3 configs/source_roots.py resolve --format json
   python3 configs/source_roots.py verify
   ```

4. Check that `FreeCM/`, `source_roots.lock.jsonc`, and `CMakePresets.json` exist.
5. If the workspace has never been prepared and the task authorizes initialization, run:

   ```sh
   git submodule update --init FreeCM
   python3 configs/source_root_workflow.py --init
   python3 configs/source_root_workflow.py --update
   ```

`--init` is the only network-enabled dependency step. Use `--update` to resolve and
materialize the active lock offline and regenerate presets. It does not fetch seeds, build
dependencies, configure Ravo, or run tests.

Ordinary build and diagnosis tasks start with the read-only commands above. Do not mutate lock
mode, seeds, concrete roots, generated presets, or build outputs unless the task actually asks
for workspace preparation, dependency refresh, a lock change, materialization, or cleanup.

Never edit or build inside `build/dependency_source_roots`; it is replaceable materialized
output. Seed repositories under `build/dependency_seed_repos` are also parent-managed by
default and become editable only after one is explicitly selected as a `manual` checkout.
Durable dependency changes belong in `source_roots.lock.jsonc.in`, `configs/source_roots.py`,
and consuming CMake files. The ignored `source_roots.lock.jsonc` is the current checkout's
effective configuration and may be hand-edited for local machine settings or a `manual`
dependency override, but it must not be committed.

## Inspect and jointly develop dependencies

Read the effective state before diagnosing dependency code or paths:

```sh
python3 configs/source_roots.py show --format json
python3 configs/source_roots.py resolve --format json
python3 configs/source_roots.py verify
```

This host exposes `show`, not the `status` command used by some other FreeCM bindings. The
root lock declares direct dependencies; `resolve` reports the recursive closure found from
local seed templates.

For local dependency development:

1. Select a real editable checkout. Never edit `build/dependency_source_roots/*`.
2. Set `depsMode` to `manual` in the ignored active lock and set only the relevant
   `depsManualPath.<dependencyName>`; empty entries keep their managed resolution.
3. Run `python3 configs/source_root_workflow.py --update`, then repeat `show`, `resolve`, and
   `verify` before configuring Ravo.
4. Build the affected Ravo targets on this repository's active platform, and run Ravo tests
   only when the task calls for behavioral validation.
5. Push the dependency commit first, query its published branch or tag with
   `git ls-remote <remote> <published-ref>`, confirm the returned SHA, then update the
   tracked template and revalidate in pinned mode.

Prefer a developer-provided sibling checkout. A clean seed repository may be selected as the
manual checkout only when that choice is explicit and the seed is then treated as a real Git
repository; do not modify it first and hope generated materialization preserves the changes.
Even when selected as a manual path, a managed seed remains owned by `--init`: a clean seed may
be synchronized back to its default branch and a dirty seed makes init fail. Do not run `--init`
during seed-based joint development.
See `DevDocs/Dependency_Workflow.md` for the complete cross-platform workflow.

## Use FreeCM maintenance actions

The host exposes the current FreeCM workflow actions directly:

```sh
python3 configs/source_root_workflow.py --refreshpin
python3 configs/source_root_workflow.py --pinlatest
python3 configs/source_root_workflow.py --update
python3 configs/source_root_workflow.py --cleanbuild --dry-run
```

- `--refreshpin` is offline and aligns active dependency commits with the tracked pinned template.
  Both locks must already be in `pinned` mode; it neither changes mode nor materializes roots.
- `--pinlatest` is offline and selects only commits already visible in local seeds. It creates
  a local candidate; it does not prove publication and must not leave a reviewed baseline in
  `latest` mode.
- `--update` is the normal offline materialization and generated-preset step after an authorized
  active-lock change.
- `--cleanbuild --dry-run` lists conservative cleanup targets. Actual `--cleanbuild` preserves
  `build/dependency_seed_repos` and `build/dependency_source_roots`; use it only when cleanup
  is requested or stale build state is demonstrated.

For an explicitly requested full dependency refresh: require clean host, FreeCM, seed, and
concrete-root worktrees and preserve a private snapshot of the original active lock. Run networked
`--init`, offline `--pinlatest`, the smallest meaningful Ravo build/tests, verify every selected
commit on its remote, and update the tracked template. Then explicitly change only the active lock
mode back to `pinned`, run `--refreshpin`, `--update`, and revalidate pinned mode. The CLI
`--pinlatest` leaves the active lock in `latest`; do not assume it performs this cleanup. At the
first candidate failure, leave the template untouched, restore the original active lock and
materialized state through `--update`, then report the original failure rather than masking it.

When explicitly refreshing the FreeCM gitlink, work from the host root:

```sh
git submodule update --remote --checkout FreeCM
```

Never run `git -C FreeCM pull` in the normally detached submodule. An unchanged gitlink is a
silent no-op. A changed gitlink requires lock compatibility, command-manifest, source-root, and
host build validation before it is ready for review.

## Keep project actions explicit

`configs/freecm.commands.jsonc` uses manifest version 2. Config is the active command context;
Build, Run, Test, and Package variants bind to compatible Config IDs and do not configure as a
hidden side effect. Validate workflow edits with:

```sh
python3 -m repomgrcpp.tools.repo_tool check-lock-compat --repo-root .
python3 FreeCM/tools/validate_repo_commands.py .
```

## Select a Ravo build

On Windows, use the PowerShell entrypoint so the MSVC developer environment and the Ravo-only
preset are selected consistently:

```powershell
& .\Ravo\tools\freecm_project.ps1 -Action Configure -Configuration Debug
& .\Ravo\tools\freecm_project.ps1 -Action Build -Configuration Debug
```

Use `Release` for packaging-like staging, performance work, or representative GPU measurements:

```powershell
& .\Ravo\tools\freecm_project.ps1 -Action Configure -Configuration Release
& .\Ravo\tools\freecm_project.ps1 -Action Build -Configuration Release
```

On macOS and Linux, use the cross-platform Python entrypoint with the corresponding host preset
generated by FreeCM:

```sh
python3 Ravo/tools/freecm_project.py --action Configure --configuration Debug
python3 Ravo/tools/freecm_project.py --action Build --configuration Debug
```

The project actions configure only `Ravo/`. Do not configure, compile, run, test, or package the
frozen 0.9 application as a fallback path.

## Enable and run tests

Tests are opt-in and belong only to the Ravo graph. On Windows:

```powershell
& .\Ravo\tools\freecm_project.ps1 -Action Test -Configuration Debug
```

On macOS and Linux:

```sh
python3 Ravo/tools/freecm_project.py --action Test --configuration Debug
```

Do not run leftover software as a live oracle. Do not
report tests as passing when only configure/build was requested or executed.

## Diagnose failures

Classify the earliest real failure before editing:

- dependency root missing or wrong commit: inspect the active lock, then rerun `--update`;
- generated preset stale: change the template/generator if needed, then rerun `--update`;
- configure failure: capture the first missing package, compiler, or CMake diagnostic;
- compile failure: rebuild the narrow target with verbose output when needed;
- link/runtime failure: inspect the selected binary and loader paths, not a different preset;
- stale Ravo data or runtime artifact: install into a new staging prefix before drawing conclusions.

Do not install Homebrew packages or change system configuration unless the user authorized it.
Do not hide a failure by turning off a required feature.

## Validation depth

- Documentation-only changes: check Markdown links, commands, and `git diff --check`.
- FreeCM gitlink, manifest, CMake, or dependency changes: run the relevant lock/manifest checks,
  an authorized `--update`, configure Ravo, and build an affected target.
- C/C++ changes: build the affected Ravo target and run relevant Ravo tests when the task calls
  for behavioral validation.
- Catalog/import/desktop changes: add schema/service/codec contract coverage and perform the
  smallest real create/open/import/view desktop acceptance after the headless tests pass.
- Broad core changes: run the complete Ravo unit/contract set unless the user explicitly limits
  the turn to compilation.
- GPU or performance changes: follow `DevDocs/GPU_Baseline.md` with a Release build.

Report the host configuration, targets, tests, result, and any checks not run. A missing
dependency or asset is a limitation, not a passing result.
