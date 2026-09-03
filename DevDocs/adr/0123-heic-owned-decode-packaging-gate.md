# ADR-0123: HEIC/HEIF owned-decode packaging and licence gate

- Status: Accepted
- Date: 2026-09-03
- Relates: PRO-INGEST remaining work in [TODO.md](../TODO.md);
  [ADR-0118](0118-heic-heif-fail-closed-ingest.md)
- Extends: [ADR-0118](0118-heic-heif-fail-closed-ingest.md),
  Dependency Workflow / [Packaging.md](../Packaging.md)

## Context

ADR-0118 recognizes HEIC/HEIF containers and fails closed without an owned
decoder. Photographers still need pixel decode (colour, orientation,
multi-image selection). Candidate libraries (notably libheif and codec
backends) carry GPL/LGPL and patent-surface questions that must not arrive as
an incidental ImageIO or silent static link.

This ADR accepts the **packaging/licence gate** only. It does **not** authorize
shipping a decoder, enabling Qt/ImageIO HEIC, or relaxing ADR-0118 fail-closed
recognition.

## Decision

1. **No decoder until Dependency Workflow records** a named runtime/source,
   SPDX licence set for every linked codec, GPL compatibility with Ravo’s
   distribution model, third-party notices text, and whether the decode path is
   optional/downloadable vs bundled.
2. **No incidental host decode.** macOS ImageIO, Windows WIC, or Qt plugins that
   happen to open HEIC remain forbidden substitutes (ADR-0118).
3. **If licence/package evidence is incomplete or GPL surprise risk remains,
   leave owned decode residual.** Keep `unsupported_heic_input` fail-closed.
   Do not land half-linked or `#ifdef`-only stubs that look shipped.
4. **Pixel/colour/orientation/multi-image contracts** stay out of this ADR; a
   later Ready tranche may accept them only after the packaging gate is green.

## Consequences

PRO-INGEST HEIC decode stays blocked on explicit packaging evidence. Product
behaviour remains fail-closed recognition. No illegal GPL surprise via an
undocumented dependency.

## Rejected alternatives

- Enabling Qt/ImageIO HEIC to “unblock” browsing.
- Vendoring libheif without SPDX/notices/update-channel records.
- Claiming decode readiness from recognition-only tests.
