# ADR-0087: Progressive Develop preview

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0011](0011-atomic-develop-publication.md),
  [ADR-0047](0047-first-frame-raw-cache-lifecycle.md),
  [ADR-0067](0067-bounded-preview-cache-lru.md)
- Relates to: [ADR-0086](0086-lightroom-crs-interchange.md)

## Context

Applying a substantial preset to a high-resolution RAW waited for the complete
1600px CPU render and PNG publication before Studio showed any changed pixels.
The saved recipe, render, cache encoding, and publication were one serial
latency path even though only the first visible response is interactive. A
single linear-working cache entry also made a lower-resolution preview evict
the already prepared settled working image.

## Decision

**2026-10-02 amendment:** Studio now renders both live interaction and settlement
at `kDefaultPreviewMaxEdge` (1600px), using the same prepared working generation.
Dragging must not downgrade the displayed image to 960px. The native-display
request admits existing cached GPU operations at 1600px, while standard-size
CPU-pixel probes/comparison/persistence/export keep CPU-gold ownership. Smaller
explicit preview requests remain supported. The one-in-flight/latest-pending
bounds, cancellation, source identity and exact recipe authority are unchanged.
The 1600px target refers to the displayed cropped photo: Engine geometry sizing
raises prepared source density before crop/constrained perspective, capped at
native dimensions. RGB-only edits retain that source generation. Cache identity
uses prepared dimensions and cached metadata describes the final rendered photo.

- An ordinary committed Develop change saves through the existing atomic
  recipe/history/revision transaction, then renders the same parameters at
  `kDefaultPreviewMaxEdge` (1600px). Studio publishes those owned RGB pixels
  directly and queues the same request revision at `kDefaultPreviewMaxEdge`
  for persisted settlement. There is no approximate recipe or reduced
  operation set.
- Studio retains the existing one-in-flight, one-pending-save, and
  one-latest-pending-preview bounds. Repeated pure-interactive intents finish
  the active pixel frame instead of cancelling it into starvation; completed
  frames publish in increasing request order and the latest parameters run
  next. The presenter records the parameters represented by each frame, so a
  progressive intermediate never claims to match newer current state. Save,
  selection, close, comparison, and non-interactive supersession still cancel
  the token and reject a late result by revision and asset. Crop guides, mask
  overlay, and before/after retain their explicit behavior.
- Owned interactive pixels become the QML image resource before preview
  identity and scopes. A second presenter-owned serial worker keeps only the
  latest immutable frame, computes its pixel/profile SHA-256 and selected
  diagnostic, and publishes them only when analysis, preview, and asset
  revisions still match. Live control reports identity loading during that
  bounded interval. New frames cancel analysis between bounded image rows and
  diagnostic stages; window destruction cancels and waits for both workers.
- CatalogService retains two bounded linear-working slots for explicit smaller
  and standard-size requests. Studio live and settled display share the latter.
  Both remain keyed by asset, source
  fingerprint, target size, and RAW/input-colour preprocess state. New RAW
  ownership and close clear both slots. An explicitly requested smaller frame
  materializes the settled-size linear working first when that slot is empty,
  then box-filters it to the interactive size. Interactive 960 pixels therefore
  match a downsampled settled linear image rather than a second CFA demosaic.
  Settled 1600, preview-cache PNG, and export remain native at their requested
  size. Gallery browse does not promote or downscale across size classes.
- Gallery thumbnail decode/working state is a separate bounded background lane.
  A Develop request cancels active thumbnail work and takes foreground queue
  priority, so browse work cannot evict or queue ahead of the selected photo.
  Entering Develop renders the display-size interactive stage before settlement to
  prepare the first slider interaction.
- Rebuildable preview-cache PNG uses libpng's latency-first mode and one write
  into the documented maximum output bound. Normal engine/output export PNG
  retains the ordinary compression path.
- Independent CPU rows use static contiguous partitions with caller
  participation and at most 16 workers. Each row checks cancellation. Failure
  to start a worker returns a structured I/O error; it does not silently run a
  different path. Per-pixel arithmetic and reduction order remain
  deterministic. Denoise wavelet scratch stores its three real YUV channels
  without a fourth padding component.
- Recipe owns dense tone/RGB point-curve LUT construction. It prepares the
  selected interpolator once per curve and samples the same positions and
  scalar evaluator results that Engine previously requested one at a time.
  Engine remains the LUT consumer, and exact scalar/LUT equality for every
  supported interpolator is a unit-test contract.

## Consequences

Studio can show the changed look before full preview settlement, while the
persisted result, cache identity, and export path remain exact. Studio keeps
one standard-size working generation for both dragging and settlement. The
optional smaller slot serves explicitly requested low-edge previews only.
Develop pays one demosaic and then reuses that generation for sliders. Fast cache PNG may consume more
of the existing 512 MiB LRU budget, but it is disposable and pixel/profile
equivalent when decoded. Progressive drag frames may briefly trail the newest
slider position, but they never regress after a newer frame and the final
queued intent remains exact. The analysis worker adds at most one active and
one pending implicitly shared image per Studio window. This decision adds
neither a GPU/CPU fallback nor a process-wide worker pool.

## Rejected alternatives

- Lowering quality or skipping expensive operations in the first stage. That
  would create a second visual recipe and visible correction jumps unrelated
  to resolution.
- Replacing the settled preview with an upscaled interactive image. That would
  make cache and export inspection non-representative.
- A process-global persistent worker pool in this tranche. Sampling showed
  pixel arithmetic, not thread construction, as the remaining CPU cost; a new
  global lifecycle owner was not justified.
