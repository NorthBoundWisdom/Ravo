# ADR-0127: Export delivery text watermark (narrow)

- Status: Accepted
- Date: 2026-09-03
- Relates: PRO-EXPORT remaining work in [TODO.md](../TODO.md)
- Extends: [ADR-0071](0071-deterministic-text-watermark.md),
  [ADR-0117](0117-export-box-sharpen-presets-and-restartable-jobs.md),
  [ADR-0032](0032-encoded-byte-publication-contract.md)
- Supersedes: the ADR-0117 deferral of “watermark” for **export delivery** only
  (colour-conversion/frame overlays remain undecided)

## Context

ADR-0071 already owns a deterministic develop-pipeline text watermark
(`ravo.output.watermark`) with a built-in glyph set. PRO-EXPORT still listed
watermark as undecided because photographers often want a **delivery-only**
overlay on JPEG/PNG/TIFF export (and batch jobs/presets) without baking that
mark into the Develop recipe or history. A second pixel owner, SVG/logo pack,
or system-font path would reopen ADR-0071’s portability and failure rules.

## Decision

### One watermark mathematics owner

- Export delivery watermark reuses the **ADR-0071** text watermark contract and
  engine stage (bounded ASCII + `{stem}`/`{asset_id}`, nine-position alignment,
  opacity/scale/rotation, fail-closed layout). No second glyph set, SVG/PNG
  lookup, or system font.
- There is **one** pixel composition owner: the existing encoded-output watermark
  stage. Export must not invent a QML or adapter-side overlay.

### ExportOptions ownership (not Develop recipe)

- Delivery watermark is an optional field on **ExportOptions** / export presets /
  restartable jobs (ADR-0117), not a silent mutation of the asset Develop
  recipe or history.
- When the export watermark option is absent/disabled, export behaviour matches
  today’s recipe-only path (Develop watermark still applies if present in the
  recipe).
- When enabled, CatalogService applies the ADR-0071 watermark parameters from
  ExportOptions for that render only. Recipe bytes on disk stay unchanged.
- Original-copy rejects any watermark option (exact source bytes).

### Privacy and non-goals

- Watermark text is not catalog metadata; export privacy modes (ADR-0064) do not
  strip or invent watermark glyphs. Location/Core packet rules are unchanged.
- Colour-conversion frame / decorative borders beyond ADR-0070 Frame remain
  **undecided**.
- Logo/SVG watermark, per-user font packs, and EXIF/token expansion beyond
  ADR-0071 remain unsupported.

### Implementation tranche

- This ADR accepts the product contract. Shipping ExportOptions fields, preset
  JSON, Studio/CLI controls, and equality tests is the next Ready slice; until
  that lands, PRO-EXPORT watermark stays **accepted / unimplemented**.

## Consequences

PRO-EXPORT watermark is no longer blocked on an open decision. Delivery marks
share ADR-0071 determinism without becoming a second live Develop authority.
Colour-conversion/frame stays residual.

## Rejected alternatives

- A separate export-only rasterizer or QML overlay.
- Baking export watermark into Develop recipe/history by default.
- Waiting for logo/SVG support before accepting text delivery marks.
- Treating colour-conversion frame as in-scope for this ADR.
