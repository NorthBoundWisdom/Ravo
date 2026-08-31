# ADR-0003: Version the machine contracts before implementing Ravo clients

- Status: Accepted
- Date: 2026-07-21

## Context

Ravo's CLI is both the first supported product surface and the intended entry
point for automation, contract tests, and legacy differential runners.  Recipe
files and JSON output will outlive individual C++ objects and any future
desktop client.  Serialising implementation layout, allowing human log output
to pollute stdout, or silently accepting unknown fields would make later
compatibility work untestable.

## Decision

- Ravo starts with `ravo-cli/v1` as its machine-readable CLI protocol.  A
  successful JSON response uses a `ravo.cli.result` envelope with version `1`,
  `ok: true`, `data`, and `diagnostics`; a failure uses the same envelope with
  `ok: false` and a structured `error` object.
- Canonical recipes are JSON documents with a numeric `schema_version`.
  Version `1` is the only schema written by the first implementation.  Reads
  must run an explicit upgrade path before validation; newer unknown schema
  versions, unknown top-level fields, and malformed parameter values are
  rejected rather than guessed.
- Operation IDs and each operation parameter schema have independent numeric
  versions.  A recipe operation always stores both.  The stable IDs reserved
  for the first vertical-slice descriptors are documented in
  [legacy capability census](../legacy/CapabilityInventory.md).
- JSON selected by `--json` is written only to stdout.  Human diagnostics,
  progress, and verbose logs use stderr.  Keys are emitted in stable order so
  command snapshots are meaningful.
- Exit status is part of the protocol: `0` success, `2` command usage, `3`
  input/not-found, `4` validation, `5` unsupported capability, `6` I/O or an
  output-path conflict, `7` cancelled, and `70` internal failure. The numeric
  values are asserted by contract tests.
- Stage 1 deliberately publishes no binary C++ ABI.  Public C++ headers offer
  source-level contracts within a single checkout only.  A stable embedding
  ABI requires a later ADR after engine lifetime, allocator, exceptions, and
  platform packaging are proven.

## Consequences

- The first CLI can be scripted without parsing human text, and future
  services or desktop clients can share the same recipe semantics without
  shelling out to the CLI.
- Version upgrades and explicit failures add implementation work now, but
  prevent opaque recipe corruption from becoming a compatibility promise.
- Snapshot tests must compare complete JSON output and must fail if stdout
  receives a log line.

## Rejected alternatives

- **Unversioned ad-hoc JSON**: it makes a successful parse indistinguishable
  from an accidental interpretation after the model changes.
- **A stable C++ ABI in Stage 1**: it would freeze allocation, exception, and
  ownership conventions before any external engine consumer exists.
- **Best-effort acceptance of unknown recipe data**: it can silently change an
  image edit.  Ravo must report a readable unsupported or invalid-data error.
