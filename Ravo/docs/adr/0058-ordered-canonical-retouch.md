# ADR-0058: Retouch owns ordered canonical regions and source geometry

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0045](0045-studio-mask-overlay-group-path.md)

## Context

P2's final Ready operation was `iop/retouch.c`. The frozen owner combined up
to 300 mask forms with ordered clone, heal, Gaussian/bilateral blur, and
erase/color fill actions. Forms could run on the original image, one of 15
à-trous detail scales, or the residual, and clone/heal source coordinates were
part of each legacy mask rather than the module parameter blob.

The four frozen Retouch fixture families contain enabled v1 singleton
revisions with circle, ellipse, path, and brush masks. Their surrounding
documents also contain unaccepted `rawprepare` or `basecurve` state, so they
are payload/mask census evidence rather than whole-document compatibility.

## Decision

- `ravo.repair.retouch` schema v1 owns
  `working_space=linear_rec709_d50`,
  `algorithm=ordered_regions_wavelet_v1`, bounded wavelet/merge/heal state,
  and an ordered region array. Each region references one canonical drawable
  leaf and records mode, scale, opacity, normalized source point, blur state,
  and fill state. An operation-level mask is invalid.
- Regions execute in stored order on owned image state. A later clone/heal may
  therefore read pixels published by an earlier region. Clone copies from a
  temporary source stamp; heal solves the frozen subtractive Laplace problem
  with red/black over-relaxation; fill applies erase or color plus brightness.
- Gaussian blur uses the frozen zero-boundary recursive coefficients.
  Bilateral blur converts the bounded stamp through the private D50 Lab bridge
  and uses the frozen spatial/lightness splat, three five-tap grid passes, and
  trilinear base-layer slice.
- Wavelet mode uses the frozen reflected separable à-trous decomposition,
  processes original/detail/residual regions at their declared scale, supports
  merge-from-scale reconstruction, and maps a requested residual to the
  actual maximum scale for small frames.
- Canonical masks own geometry and rasterization. Clone/heal derive their
  destination anchor from the referenced circle/ellipse center or first
  path/brush point, then apply the normalized source offset in attached-frame
  coordinates. Out-of-frame source pixels are clipped; no fabricated source
  sample is substituted.
- Strict legacy import accepts only the five evidenced enabled-v1 Retouch
  revisions and exact v9/v10 blend blobs. It decodes only the evidenced v6
  circle/ellipse/path/brush/group mask layouts, equal path feather and brush
  radii, source coordinates, and group opacity. Duplicate, dangling,
  asymmetric, proportional-ellipse, custom, malformed, or other state rejects.
- Develop, CLI, Catalog, and Studio share the same recipe. Studio authors
  bounded circle regions through the command boundary and exposes all four
  modes, source point, Gaussian/bilateral choice, and erase/color fill state.
- Memory preflight accounts for DWT, heal, local filtering, and mask evaluator
  planes. Invalid schema/parameters/graph/profile/scale, non-finite samples,
  allocation, and cancellation publish nothing. No fallback implementation is
  retained.

## Consequences

The old Retouch IOP and its exclusive OpenCL kernel are removed. Shared
`common/dwt*`, `heal*`, `bilateral*`, Gaussian, canonical mask code, old mask
special cases, ordering/style/manual text, and GTK paint helpers remain because
other leftovers or final cleanup gates still own them. Retiring M2 therefore
does not claim S2/S3/S5/S10/D0 completion.

## Rejected alternatives

- A single brush/heal shortcut. It would omit ordered multi-region state,
  clone, blur, fill, source geometry, and wavelet fixtures.
- Embedding shape coordinates inside Retouch parameters. Canonical mask
  ownership is already accepted and must remain the sole geometry graph.
- Importing entire Retouch fixture documents by silently absorbing non-default
  `rawprepare` or `basecurve`. Those operations retain independent migration
  gates.
- Reusing the old GTK form owner, preview pipe, global history, or OpenCL API.
  They violate Ravo's service/engine ownership and cancellation boundaries.
