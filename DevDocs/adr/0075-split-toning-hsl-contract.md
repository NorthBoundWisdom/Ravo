# ADR-0075: Split Toning owns the full shadow/highlight HSL transition

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0015](0015-migrate-all-non-ui-algorithms.md)

## Context

Ravo's early Split Toning approximation fixed both saturations at 0.5 and
compression at 0.15 while exposing a generic amount. The frozen owner has six
independent controls and defines exact HSL shadow/highlight branches around a
balance pivot. The approximation was not C20 acceptance.

## Decision

- `ravo.color.splittoning` schema v2 declares `linear_rec709`,
  `frozen_splittoning_v1`, shadow/highlight hue and saturation, balance,
  compression 0–100, and Ravo mix 0–1. Schema-v1 hue/balance/amount upgrades
  with the frozen 0.5 saturations and 33 compression defaults.
- Engine owns one private RGB↔HSL value implementation shared with existing
  HSL consumers. Compression is `(value/110)/2`; pixels below/above the pivot
  exclusion band receive source HSL lightness with the selected hue/saturation,
  frozen doubled distance weight, per-channel blend, and [0,1] clamp. Mix is
  applied to that weight; zero mix is bit-preserving identity.
- Canonical masks use the full-frame evaluator and normal mix. Input/profile/
  sample validation, row/pre-publication cancellation, allocation, output
  finiteness, source ownership, and output-buffer memory are explicit.
- Strict XMP import accepts only the exact enabled v1 singleton in 0062 with
  its 24-byte payload and blend-v9 default. Modified, masked, custom blend,
  multi-instance, or other state rejects.
- Recipe/CLI/Catalog/Studio/styles share all seven controls. Studio preserves
  imported masks read-only; the former `splitAmount` field is a compatibility
  alias that enables/disables via mix, not a separate algorithm.

## Consequences

C20 is accepted. The old `iop/splittoning.c`, its exclusive `extended.cl`
kernel, and darkroom icons are removed. Shared HSL users, picker, order,
module-group, manual, and frozen fixtures remain with their own owners.

## Rejected alternatives

- Keep fixed saturations/compression. It cannot reproduce the frozen fixture or
  presets.
- Implement Split Toning inside QML or encoders. It is recipe-visible linear
  RGB image math shared by preview and every export.
