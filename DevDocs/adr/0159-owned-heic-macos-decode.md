# ADR-0159: Owned macOS HEIC/HEIF decode

- Status: Accepted
- Date: 2026-09-06
- Supersedes: the blanket decode prohibition in
  [ADR-0118](0118-heic-heif-fail-closed-ingest.md) and
  [ADR-0123](0123-heic-owned-decode-packaging-gate.md) for the provider below.

## Decision

The requested HEIC/HEIF import support uses a dedicated private Ravo raster
adapter over macOS 14+ ImageIO/CoreGraphics/CoreFoundation. These Apple system
frameworks are dynamically linked OS components, updated with macOS; no codec
binary, libheif, libde265 or patent licence is redistributed or claimed by Ravo.
They are declared in the adapter CMake target and packaged notices. This is a
named system-library dependency, not a Qt plugin discovered opportunistically.
Windows/Linux retain `heic_decoder_unavailable` until their provider and package
contracts are admitted. No source-root pin or external download is introduced.

Recognition remains content-based. Decode selects the container's primary image,
applies orientation exactly once and returns owned opaque SDR RGB8 in sRGB,
matching the existing raster/Develop baseline. ImageIO explicitly decodes to SDR;
CoreGraphics performs the colour conversion. HDR gain maps, auxiliary images,
alpha and sequence navigation are not imported as separate assets. The media
type remains `image/heic`, including when the filename has another extension.

Encoded input is bounded to 256 MiB and the primary image to 64 million pixels
and 32768 pixels per edge before allocating output. Native objects are local
RAII owners. Cancellation is checked around native decode and while copying
rows; a native synchronous call itself cannot be interrupted. Work remains on
the existing service/Studio executors, which join before destruction. Truncated,
corrupt, oversized, unsupported and cancelled inputs return structured errors;
there is no alternate decoder, format relabel or source rewrite. Top-level ISO
BMFF boxes must fit the supplied bytes before ImageIO runs. Synthesized alpha
over an opaque container is `kValidation`/`invalid_heic_input`, not an
unsupported transparent image.

Import, thumbnail, preview, memory decode and export use the same raster port.
Catalog/recipe schema and atomic publication do not change. HEIC encoding is
not added: users export through the existing JPEG/PNG/TIFF/original-copy owners.

## Validation

Adapter tests generate non-private HEIC samples and check primary dimensions,
orientation, colour, path/memory equality, proportional scaling, truncation,
bounds, cancellation and source preservation. Service/CLI checks cover scan,
import, preview, reopen, export and conflict with no partial publication.
Current-user samples are read-only local evidence and are never committed.
macOS source builds and bundled framework linkage are checked locally;
Windows/Linux retain explicit unsupported-provider tests and are not claimed
as HEIC-capable packages.
