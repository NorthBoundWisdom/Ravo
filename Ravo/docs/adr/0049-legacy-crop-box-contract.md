# ADR-0049: Leftover crop box maps to canonical x/y/width/height

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0048](0048-legacy-flip-orientation-contract.md)

## Context

P2 orientation/crop follows leftover flip import. Studio already authors a
normalized crop rectangle and keeps it inside the photo through rotate/flip.
Leftover `iop/crop.c` stores left/top/right/bottom (`cx`, `cy`, `cw`, `ch`) plus
optional `ratio_n`/`ratio_d` export aligners. The strict XMP importer rejected
every crop history as `unsupported_legacy_operation`.

Frozen evidence is crop v1 in `0094`/`0095` (24-byte payload) and current crop
v3, which uses the same struct. Preview crop uses the box; ratio snapping
applies only on export/thumbnail pipelines.

## Decision

- Leftover crop v1/v2/v3 is a dedicated importer. Enabled singleton, unmasked
  default blends (`gz11`, the colorin `gz14` twin, and the `0094`/`0095` `gz14`
  guide-five blob), and a 24-byte `cx,cy,cw,ch,ratio_n,ratio_d` payload are
  required. Masks and other attributes reject.
- `cx,cy,cw,ch` clamp with leftover `MIN_CROP_SIZE` 0.01 and map to canonical
  `x,y,width,height` where `width = cw-cx` and `height = ch-cy`. An empty box
  after clamp rejects with `unsupported_legacy_crop_box`.
- Full-frame `0,0,1,1` is identity and adds no recipe operation, matching Studio
  defaults. `ratio_n`/`ratio_d` are not imported; export dimension snapping
  stays later G7 work.
- Canonical `crop_working` remains the product pixel path. Leftover
  `(int)` truncation and `MAX(4, …)` export ROI are not this contract.

## Consequences

Identity crop histories stay two-operation recipes. Real leftover boxes with
evidenced blends become `ravo.geometry.crop`. Tests cover the 24-byte identity
and quarter boxes, the frozen `0095` v1 payload, empty-box rejection, and a
16×16 pixel crop of `0.25,0.25,0.75,0.75`.

## Rejected alternatives

- Treating leftover crop as a builtin nop, which cannot represent a real box.
- Importing `ratio_n`/`ratio_d` as Studio aspect lock. Those integers snap
  export pixel counts; the authored box already encodes the frame.
- Replacing canonical `llround` crop with leftover `(int)` truncation in this
  tranche. That ROI edge delta stays later G4 ALG work with mask/distort.
