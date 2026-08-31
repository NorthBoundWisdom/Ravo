# ADR-0006: Keep colour state explicit at the Ravo engine boundary

- Status: Accepted
- Date: 2026-07-22

## Context

The frozen application distributes colour decisions across input/output IOPs,
pixelpipe state, metadata, and UI configuration. Recreating that implicit
state in a new engine would make a recipe or output file ambiguous: a buffer
could look valid while its primaries, transfer function, ICC profile,
adaptation, alpha association, or rendering intent were unknown.

Ravo needs a CPU reference path before any GPU adapter or desktop client, and
different clients must not silently choose different display or export colour
defaults. At the same time, a Phase 1 metadata registry must not pretend that
legacy `colorin`, `colorout`, or every old profile parameter is already
compatible.

## Decision

- Every public decoded-image, preview-resource, render-request, and render-result
  contract will carry an explicit, versioned colour description. It identifies
  pixel format, alpha association, source/target encoding, and either a stable
  embedded-profile reference or an explicit absence/error state.
- A CPU colour transform is an engine operation with declared source, target,
  intent, and conversion policy. Clients never infer a transform from a widget,
  monitor, catalog preference, filename, or untagged byte buffer.
- The first output implementation must either embed the declared output profile
  or return a structured unsupported/validation error. Original-file copy is
  byte-preserving and does not reinterpret image or metadata colour state.
- Unknown, malformed, or unsupported input colour metadata is reported
  structurally. It is never silently substituted with sRGB, a monitor profile,
  or a legacy darktable setting. A future product decision may define an
  explicit opt-in fallback policy, with a new contract version and fixture
  coverage.
- lcms, codec, platform, and future GPU colour objects remain private adapter
  implementation details. Their headers, handles, exceptions, and allocator
  rules do not enter foundation, recipe, engine, CLI, or JSON schemas.
- The first vertical slice records input colour metadata, output profile/hash,
  alpha/orientation behaviour, and numerical tolerance beside its fixture. No
  `colorin` or `colorout` legacy mapping is accepted until those records and a
  canonical mapping test exist.

## Consequences

- Phase 2 adds explicit colour types and synthetic tests before the first RAW
  or raster render can claim success. This is a contract requirement, not an
  instruction to copy legacy IOP structs or defaults.
- CLI and future services can agree on an image's declared colour state without
  sharing a UI framework or calling a CLI subprocess. The initial command-line
  spelling remains an implementation detail until the render command is added.
- Packaging work must include the license notices and runtime obligations for
  whichever private colour adapter is selected. This extends the existing Qt
  packaging obligation in ADR-0005 without adding a network download or a
  public third-party ABI.
- GPU work remains deferred until CPU colour behaviour has fixture evidence;
  a GPU resource cannot encode or choose a public colour policy.

## Rejected alternatives

- **Assume sRGB whenever metadata is absent**: it changes photo appearance
  without an auditable recipe or output decision.
- **Expose lcms or codec handles from the engine**: it freezes third-party
  lifetime and allocation rules into clients that do not need them.
- **Copy `colorin`/`colorout` parameters verbatim**: those blobs are legacy
  module data, not a versioned Ravo colour contract.
- **Defer all colour choices to the desktop**: the CLI would then produce
  client-dependent results and could not be a CPU reference oracle.
