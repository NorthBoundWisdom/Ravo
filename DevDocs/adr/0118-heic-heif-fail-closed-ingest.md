# ADR-0118: HEIC/HEIF fail-closed ingest recognition

- Status: Accepted
- Date: 2026-09-03
- Extends: [ADR-0102](0102-planned-managed-import-workspace.md),
  [ADR-0104](0104-bounded-rename-and-verified-second-copy-ingest.md)

## Context

PRO-INGEST still needs PTP/MTP, HEIC/HEIF, DNG conversion, and Smart Preview
policy. Photographers already land `.heic` / `.heif` trees from phones on
ordinary mounted volumes. ADR-0102/0104 cover local Add/Copy/Move, but HEIC was
neither enumerated nor given an owned decoder. On hosts where Qt ImageIO can
open HEIC, an accidental fallback would decode foreign pixels and risk labeling
them as JPEG. QOI/RGBE already show the required pattern: content-magic
recognition and a structured unsupported failure that preserves source bytes.

## Decision

- HEIC/HEIF containers are recognized by ISO BMFF `ftyp` brands, not by
  extension alone. Recognized brands for this tranche are the HEIC/HEIF family
  (`heic`, `heix`, `hevc`, `hevx`, `heim`, `heis`, `hevm`, `hevs`, `heif`,
  `heifs`, `mif1`, `msf1`, `avci`, `avcs`). AVIF (`avif` / `avis`) remains a
  separate undecided format and is not claimed by this ADR.
- Until an owned decoder ships under an explicit dependency, licence, and
  package decision, probe, decode, decode-memory, import inspection, and
  catalog publication fail closed with `ErrorCode::kUnsupported`,
  `format=heic`, and `reason=unsupported_heic_input`. No asset, preview, or
  recipe row is published.
- Ravo never classifies a recognized HEIC/HEIF payload as JPEG/PNG/TIFF, never
  routes it through the JPEG/PNG/TIFF owners, and never enables decode through
  an incidental Qt ImageIO / platform plugin path.
- Import enumeration includes `.heic`, `.heif`, `.heics`, `.heifs`, and `.hif`
  so folder scans surface candidates as unsupported instead of silently
  omitting them. Extension without matching `ftyp` brands does not claim HEIC;
  content recognition wins, matching the QOI contract.
- Source bytes stay immutable. There is no silent HEIC-as-JPEG rewrite, on-ingest
  DNG replacement, or Smart Preview generation in this tranche.
- Add/Copy/Move, bounded rename, and verified second-copy continue to reuse
  CatalogService planners from ADR-0102/0104 once a future owned decoder marks
  a candidate supported. This ADR does not change those planners.
- Owned HEIC/HEIF decode (pixel ownership, colour, orientation, multi-image
  selection, package size, and licence gates), PTP/MTP transport, DNG
  conversion, Smart Previews, and PRO-PRESENT remain out of scope.

## Consequences

Phone HEIC/HEIF files become visible and explicitly unsupported instead of
invisible or mis-decoded. A later Ready tranche may introduce an owned decoder
without reopening pretend-JPEG or background original replacement. Tests cover
magic recognition, mislabeled extensions, structured import failure with zero
publication, cancellation, and source SHA-256/size/mtime preservation.

## Rejected alternatives

- Enabling Qt/ImageIO HEIC decode without an owned Ravo decoder contract.
- Treating HEIC as JPEG based on extension, thumbnail companions, or host
  plugins.
- Waiting for PTP/MTP hardware support before recognizing local HEIC trees.
- Shipping DNG conversion or Smart Previews as a substitute for HEIC policy.
