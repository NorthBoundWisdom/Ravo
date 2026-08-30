---
name: ravo-live-grading
description: Read the photo currently selected in a running Ravo Studio from the current repository, inspect its live Develop recipe and an exact CLI-rendered preview, and help evaluate or iteratively improve the grade. Use for requests about the open Ravo photo, its current parameters, visual grading diagnosis, or revision-bound grading changes. Do not use for an offline named asset or generic image editing outside Ravo.
---

# Ravo Live Grading

Use Ravo's supported machine contract to bind every observation or edit to the photo the user
actually selected. The default invocation is read-only: identify the live photo, read current and
saved parameters, render the exact current recipe when visual analysis is useful, and report a
grading assessment. Mutate only when the user's current request explicitly asks to apply or adjust
values.

## Establish the current repository and executable

1. Read the root `AGENTS.md` and `Ravo/AGENTS.md`. Run `git branch --show-current` and
   `git status --short --branch`; preserve all existing changes.
2. Resolve the repository from the current working directory with `git rev-parse --show-toplevel`.
   Never hardcode a checkout path or reuse a CLI from a sibling checkout.
3. Locate built CLI candidates under that repository first, for example with:

   ```sh
   rg --files -uu <repo>/build | rg '/Ravo/cli/ravo(\.exe)?$'
   ```

   Do not configure, build, initialize dependencies, or change source-root state merely to satisfy
   a read request. If no usable CLI exists, report that fact and use `$build-repo` only when the
   user asks to prepare or rebuild the workspace.
4. Run a candidate from `<repo>` with:

   ```text
   ravo studio sessions --workspace-root <repo> --json
   ```

   Keep only live sessions whose `workspace_root` and resolved `executable_path` are under the
   current repository. Prefer the CLI from the same build root as the selected Studio executable.
   Zero matching sessions is an explicit not-running result. If more than one matching session
   remains, request a session ID; never choose by foreground window, launch time, PID order, logs,
   cache activity, or another incidental signal.

Process inspection may confirm a candidate executable path only. It is never authoritative for
catalog, selection, asset, or recipe identity.

## Read the live photo and recipe

Use the exact discovered session:

```text
ravo studio state --session-id <session-id> --json
```

Capture at least:

- session and selection revisions;
- catalog path/revision and selected asset ID/URI/media type/display name;
- recipe state, current and saved recipes, recipe revisions, baseline-relative
  `modified_operations`, and `pending_operations`;
- mask attachments and preview identity, including `matches_current_recipe`.

Use `current` for what the user is presently authoring and `saved` for durable catalog state.
State whether they differ. Treat `modified_operations` as the authoritative baseline-relative edit
set; do not call mandatory Input Color, default RAW Sigmoid, or Output Color a user adjustment when
the CLI omits them from that set.

For capture metadata, call `ravo catalog list --catalog <exact-catalog> --json` and select only the
observed asset ID. Do not read SQLite directly or guess from filenames.

When history identity matters, use
`ravo catalog history --catalog <exact-catalog> --asset-id <asset-id> --json` before and after the
operation. Parse the single JSON stdout envelope; stderr warnings are diagnostics, never state or
target identity.

## Read the exact image result

When the user asks for visual diagnosis or grading optimization, publish a unique read-only
artifact from the observed snapshot:

```text
ravo studio preview \
  --session-id <session-id> \
  --asset-id <asset-id> \
  --expect-session-revision <session-revision> \
  --expect-selection-revision <selection-revision> \
  --output <unique-no-replace.png> \
  --max-edge 1600 --json
```

Create the output in a unique temporary directory (`mktemp -d` on macOS/Linux or a private
`New-Item` temporary directory on Windows), then inspect the emitted PNG artifact directly with a
local image reader. Preserve its MIME type, dimensions, profile, SHA-256, and statistics. A live
window screenshot is not evidence. If `state.preview.matches_current_recipe` is false, say so; use
the explicit `studio preview` artifact—not the held window preview—for pixel analysis.

Re-read `studio state` before reporting. If session, asset, selection revision, or recipe revision
changed, discard the stale analysis and retry once against the new snapshot. If it changes again,
report the race rather than following a moving target.

## Assess the grade

Group the report by photographic intent rather than dumping JSON:

1. image identity and capture metadata;
2. saved versus pending state and masks;
3. baseline-relative changes for exposure/tonality, white balance and calibration, colour,
   curves, detail/noise, geometry, and effects;
4. visual findings supported by the exact artifact and its display-RGB statistics;
5. the smallest useful next adjustment, with expected tradeoffs.

Preserve exact numeric values. Convert stored radians to degrees or enum indices to display names
only when the mapping is deterministic; retain the raw value when ambiguity remains. Distinguish
scene controls from display/output controls and RAW-only behavior from raster behavior. Do not
infer clipping, neutrality, or persistence from appearance alone.

## Apply an explicitly requested optimization

An invocation that only says to read, inspect, assess, or suggest is not authorization to mutate.
When the user explicitly asks to apply changes:

1. Read a fresh `studio state` and bind the command to its session, selection, asset, and
   revisions.
2. Query `ravo develop-fields --json` before choosing field names or ranges.
3. Use `ravo studio develop` with exact `--expect-session-revision`,
   `--expect-selection-revision`, `--asset-id`, and strict `--set` values. Prefer a small coherent
   batch that tests one grading hypothesis. Optional `--output` must again be unique/no-replace.
4. Read `studio state` back afterwards and inspect the returned artifact. Report the exact fields,
   durable recipe state, history outcome, pixel statistics, and any visual tradeoff.

Stop on stale revision, selection change, unavailable command, invalid field/range, cancellation,
or output conflict. Never redirect the edit to the newly selected photo. If the requested state is
not expressible through the shared CLI contract, report or implement that missing contract as a
separate repository task; do not use QML introspection, direct SQLite writes, logs, accessibility
automation, or synthetic UI input.

## Hard boundaries

- Never use Computer Use, AppleScript/System Events, accessibility trees, coordinate input, or
  application screenshots for Ravo.
- Never infer the selected asset from the foreground window, process arguments, open files, logs,
  cache timestamps, or the newest preview.
- Never run or build `legacy/`; it is static evidence only.
- Never overwrite an output artifact or modify an original image.
- Never expose assistant credentials or unrelated settings.
- A grading recommendation is not a passing pixel contract; label subjective judgment as such.
