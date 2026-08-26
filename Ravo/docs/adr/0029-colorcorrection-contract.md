# ADR-0029: Preserve Color Correction as an explicit affine D50 Lab operation

- Status: Accepted
- Date: 2026-08-27

## Context

The frozen `legacy/src/iop/colorcorrection.c` version-1 owner is a five-float
D50 Lab operation. At commit time it derives independent a* and b* slopes from
highlight and shadow endpoints; for each pixel it preserves L* and applies the
two affine corrections under one saturation multiplier. It is disabled by
default. Its GTK two-dimensional plane, pointer/picker interaction, three
built-in presets, blend controls, and OpenCL kernel surround that CPU contract
but are not additional serialized mathematics.

Existing `ravo.color.colorchecker`, `ravo.color.colorbalance`,
`ravo.color.colorbalancergb`, and `ravo.color.colorcontrast` have different
schemas, algorithms, order positions, and cache identities. None can substitute
for the frozen affine Lab path. S1.1 already provides one engine-private,
source-derived linear-Rec709↔D50 Lab bridge, so Color Correction needs no new
public colour-science or profile API.

The complete 158-XMP census finds actual Color Correction operation records
only in 0029 and 0092. Both are enabled version 1, singleton, priority zero,
unnamed, and default-unmasked; they carry the exact default blend payload for
their respective blend versions 9 and 11. Other textual fixture hits are mask,
history, order, or presentation references and do not establish another
compatible operation state.

## Decision

- `ravo.color.colorcorrection` schema v1 is independent and contains exactly
  seven fields: `working_space=lab_d50`, `algorithm=affine_lab_v1`, finite
  `highlight_a`, `highlight_b`, `shadow_a`, `shadow_b` within [-40, 40], and
  finite `saturation` within [-3, 3]. Every field plus explicit operation
  presence participates in serialization, equality, reset, and cache identity.
  Editing a numeric field enables the operation; a field reset keeps presence,
  while whole-operation or Color-section reset removes it.
- Canonical Develop order places Color Correction after
  `ravo.color.colorbalancergb` and before `ravo.color.colorcontrast`. An absent
  or disabled operation is skipped. An explicitly present default operation is
  not an identity shortcut because the private RGB/Lab round trip is observable
  in float pixels and must survive recipe, Catalog, and Studio reopen.
- The engine accepts only declared linear-Rec709 RGB working pixels and reuses
  the S1.1 D50 bridge. Commit narrowing and evaluation preserve frozen float
  expression order: `a_scale = (highlight_a - shadow_a) / 100.0F` and likewise
  for b*; the shadow endpoint is the base; L* is copied; then each chromatic
  channel evaluates `saturation * (input + L * scale + base)`. There is no
  clamp, finite repair, transfer curve, or fallback profile.
- Parameter/schema, dimensions, RGB buffer length, declared profile, every
  input/output sample, and cancellation are checked before publication.
  Cancellation is observed before work, during input and output rows, and
  before return. Success owns a separate RGB buffer and retains the exact
  profile plus immutable exposure-analysis snapshot; any validation,
  allocation, arithmetic, or cancellation failure leaves the source unchanged
  and publishes no partial output. Generic working-buffer bytes remain covered
  by the render memory-budget gate.
- The strict legacy decoder accepts only the evidenced version-1 envelope:
  lexical enabled `1`, one operation, exact priority `0`, empty name,
  non-hand-edited name state, no mask, and one of the exact default blend-v9 or
  blend-v11 payloads represented by 0029/0092. History `num` is only the stable
  instance identifier, not an algorithm revision or processing-order rule.
  Unsupported versions, disabled or malformed enabled state, duplicates,
  names, nonzero/noncanonical priority, masks, custom blend, unknown
  attributes, malformed length, and non-finite parameters fail structurally.
- CLI render and the engine direct path must produce identical pixels while
  preserving RAW source hash, size, and modification time. Catalog
  preview/save/export/close/reopen retains explicit-default presence, cache
  identity, parameters, and pixels. Studio exposes one enabled flag and the
  five hard-bound numeric intents through the shared Develop command path; QML
  owns no colour mathematics.
- The GTK 2D plane/picker, its three presets, old blend UI, and OpenCL execution
  are not product contracts and are not recreated implicitly. The shared
  `host/data/kernels/basic.cl`, `common/iop_order.c`, `libs/modulegroups.c`,
  `common/usermanual_url.c`, example style order, and module pixmap keep their
  D0.3/D0.4 or later shared cleanup owners. Their remaining strings/resources
  do not make the deleted IOP a runtime owner.

## Consequences

Color Correction now has one schema, Develop presence model, CPU engine path,
strict importer, CLI/Catalog/Studio consumer path, and stable structured-error
boundary. Independent source-order and bit-golden tests distinguish the frozen
multiply/add order and D50 bridge from plausible double or direct-RGB
substitutes. Cancellation, invalid profile/dimensions/buffer/non-finite state,
owned output, source immutability, cache persistence, and the two real legacy
envelopes are covered.

This acceptance permits the exact `iop/colorcorrection.c` source and its
`add_iop` registration to retire atomically. It does not complete the general
mask graph, S1 as a whole, shared old order/registry/resource cleanup, GPU, or
legacy styles.

## Rejected alternatives

- Reuse Color Checker, either Color Balance operation, Color Contrast, or a
  simplified RGB control: none preserves the five-field affine D50 Lab math or
  independent presence/cache identity.
- Treat default parameters as absent or short-circuit their CPU path: the
  explicit D50 round trip is observable and must survive persistence.
- Use double-derived expressions, reorder the multiply/add sequence, clamp
  extended values, or repair non-finite samples: each changes the frozen CPU
  boundary.
- Accept every fixture text hit, infer order from history `num`, or silently
  drop masks/custom blend/multi state: those records do not prove a canonical
  operation graph.
- Port the GTK plane/picker, presets, or OpenCL kernel as part of C12: they need
  separate presentation, data, or backend ownership and are not required for
  the accepted CPU contract.
